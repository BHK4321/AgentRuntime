#pragma once

#include "task_graph.hpp"
#include "event.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <chrono>
#include <exception>
#include <queue>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <functional>
#include <unordered_map>
#include <utility>

class Scheduler {
public:
    using EventSink = std::function<void(const RuntimeEvent&)>;
    using TaskSink = std::function<void(const Task&)>;
    using TaskBatchSink = std::function<void(const std::vector<Task>&)>;
    using TaskResolver = std::function<Task::Work(const Task&)>;

    explicit Scheduler(TaskGraph& graph, std::size_t worker_count = 2,
                                             EventSink event_sink = {},
                                             std::chrono::milliseconds lease_timeout = std::chrono::seconds(5),
                                             TaskSink task_sink = {},
                                             TaskBatchSink task_batch_sink = {},
                                             TaskResolver task_resolver = {})
                : graph_(graph), worker_count_(worker_count), event_sink_(std::move(event_sink)),
                    task_sink_(std::move(task_sink)), task_batch_sink_(std::move(task_batch_sink)),
                    task_resolver_(std::move(task_resolver)), lease_timeout_(lease_timeout) {
        if (worker_count_ == 0) {
            throw std::invalid_argument("worker count must be greater than zero");
        }
    }

    ~Scheduler() {
        shutdown();
    }

    void start() {
        std::lock_guard lock(mutex_);
        if (started_) {
            throw std::logic_error("scheduler already started");
        }
        started_ = true;
        accepting_ = true;
        stopping_ = false;
        pending_ = graph_.pending_size();
        for (std::size_t index = 0; index < worker_count_; ++index) {
            workers_.emplace_back([this] { worker_loop(); });
        }
        lease_monitor_ = std::thread([this] { lease_monitor_loop(); });
        condition_.notify_all();
    }

    void submit(Task task) {
        ensure_accepting();
        resolve_work(task);
        const auto task_id = task.id;
        {
            std::lock_guard lock(mutex_);
            ++pending_;
        }
        try {
            graph_.add_task(std::move(task));
        } catch (...) {
            std::lock_guard lock(mutex_);
            --pending_;
            throw;
        }
        if (graph_.is_terminal(task_id)) {
            std::lock_guard lock(mutex_);
            --pending_;
        }
        if (task_sink_) {
            task_sink_(graph_.snapshot(task_id));
        }
        condition_.notify_all();
        emit(EventType::TaskSubmitted, task_id, {}, graph_.idempotency_key(task_id));
    }

    void submit_batch(std::vector<Task> tasks) {
        ensure_accepting();
        for (auto& task : tasks) {
            resolve_work(task);
        }
        const auto count = tasks.size();
        std::vector<std::string> task_ids;
        task_ids.reserve(count);
        for (const auto& task : tasks) {
            task_ids.push_back(task.id);
        }
        {
            std::lock_guard lock(mutex_);
            pending_ += count;
        }
        try {
            graph_.add_tasks(std::move(tasks));
        } catch (...) {
            std::lock_guard lock(mutex_);
            pending_ -= count;
            throw;
        }
        std::size_t terminal_count = 0;
        for (const auto& task_id : task_ids) {
            if (graph_.is_terminal(task_id)) {
                ++terminal_count;
            }
        }
        if (terminal_count != 0) {
            std::lock_guard lock(mutex_);
            pending_ -= terminal_count;
        }
        std::vector<Task> snapshots;
        snapshots.reserve(task_ids.size());
        for (const auto& task_id : task_ids) {
            snapshots.push_back(graph_.snapshot(task_id));
        }
        if (task_batch_sink_) {
            task_batch_sink_(snapshots);
        } else if (task_sink_) {
            for (const auto& task : snapshots) {
                task_sink_(task);
            }
        }
        condition_.notify_all();
        for (const auto& task_id : task_ids) {
            emit(EventType::TaskSubmitted, task_id, {}, graph_.idempotency_key(task_id));
        }
    }

    void wait_until_idle() {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [this] { return pending_ == 0 || failure_; });
        if (failure_) {
            std::rethrow_exception(failure_);
        }
    }

    void shutdown() {
        {
            std::lock_guard lock(mutex_);
            if (!started_) {
                return;
            }
            accepting_ = false;
            stopping_ = true;
        }
        condition_.notify_all();
        if (lease_monitor_.joinable()) {
            lease_monitor_.join();
        }
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        workers_.clear();
        started_ = false;
    }

    void set_renew_lease_while_running(bool enabled) {
        renew_lease_while_running_.store(enabled);
    }

    std::string cancel(const std::string& task_id) {
        const auto [state, terminal_count] = graph_.cancel(task_id);
        if (terminal_count != 0) {
            std::lock_guard lock(mutex_);
            pending_ -= terminal_count;
        }
        emit(EventType::TaskCancelled, task_id);
        if (state == TaskState::Blocked) {
            emit(EventType::TaskBlocked, task_id);
        }
        condition_.notify_all();
        return task_state_name(state);
    }

private:
    void resolve_work(Task& task) const {
        if (task_resolver_ && task.type != "cpp_callback") {
            task.work = task_resolver_(task);
        }
        if (!task.work) {
            throw std::invalid_argument("task has no executable work: " + task.id);
        }
    }

    struct Assignment {
        std::string task_id;
        std::uint64_t run_token = 0;
        std::chrono::steady_clock::time_point lease_expires_at{};
    };

    struct RetryEntry {
        std::chrono::steady_clock::time_point retry_at;
        std::string task_id;

        bool operator>(const RetryEntry& other) const {
            return retry_at > other.retry_at;
        }
    };

    void ensure_accepting() const {
        std::lock_guard lock(mutex_);
        if (!started_ || !accepting_) {
            throw std::logic_error("scheduler is not accepting tasks");
        }
    }

    void worker_loop() {
        const auto worker_id = "worker-" + std::to_string(next_worker_id_++);
        while (true) {
            {
                std::lock_guard lock(mutex_);
                if (stopping_) {
                    return;
                }
                last_heartbeats_[worker_id] = std::chrono::steady_clock::now();
            }
            emit(EventType::WorkerHeartbeat, {}, worker_id);
            promote_due_retries();
            std::string task_id;
            Task::Work work;
            TaskContext context;
            std::uint64_t run_token = 0;
            if (graph_.try_take_ready(task_id, work, context, run_token)) {
                execute(task_id, std::move(work), context, worker_id, run_token);
                {
                    std::lock_guard lock(mutex_);
                    const auto assigned = running_by_worker_.find(worker_id);
                    if (assigned != running_by_worker_.end() &&
                        assigned->second.run_token == run_token) {
                        running_by_worker_.erase(assigned);
                    }
                    if (stopping_) {
                        return;
                    }
                }
                continue;
            }

            std::unique_lock lock(mutex_);
            if (!retry_queue_.empty()) {
                condition_.wait_until(lock, retry_queue_.top().retry_at, [this] {
                    return stopping_ || graph_.has_ready_tasks() || retry_is_due();
                });
            } else {
                condition_.wait(lock, [this] {
                    return stopping_ || graph_.has_ready_tasks() || !retry_queue_.empty();
                });
            }
            if (stopping_) {
                return;
            }
        }
    }

    void lease_monitor_loop() {
        std::unique_lock lock(mutex_);
        while (!stopping_) {
            condition_.wait_for(lock, std::chrono::milliseconds(10), [this] {
                return stopping_;
            });
            if (stopping_) {
                return;
            }
            const auto now = std::chrono::steady_clock::now();
            std::vector<std::pair<std::string, Assignment>> expired;
            for (const auto& [worker_id, assignment] : running_by_worker_) {
                if (now < assignment.lease_expires_at) {
                    continue;
                }
                if (reclaimed_tokens_[worker_id] == assignment.run_token) {
                    continue;
                }
                reclaimed_tokens_[worker_id] = assignment.run_token;
                expired.emplace_back(worker_id, assignment);
            }
            if (expired.empty()) {
                continue;
            }
            lock.unlock();
            for (const auto& [worker_id, assignment] : expired) {
                emit(EventType::WorkerLeaseExpired, assignment.task_id, worker_id);
                if (assignment.task_id.empty()) {
                    continue;
                }
                if (!graph_.reclaim_running(assignment.task_id, assignment.run_token)) {
                    continue;
                }
                {
                    std::lock_guard erase_lock(mutex_);
                    const auto assigned = running_by_worker_.find(worker_id);
                    if (assigned != running_by_worker_.end() &&
                        assigned->second.run_token == assignment.run_token) {
                        running_by_worker_.erase(assigned);
                    }
                }
                emit(EventType::TaskReclaimed, assignment.task_id, worker_id);
                condition_.notify_all();
            }
            lock.lock();
        }
    }

    bool retry_is_due() const {
        return !retry_queue_.empty() &&
               retry_queue_.top().retry_at <= std::chrono::steady_clock::now();
    }

    void promote_due_retries() {
        std::lock_guard lock(mutex_);
        while (!retry_queue_.empty() &&
               retry_queue_.top().retry_at <= std::chrono::steady_clock::now()) {
            const auto task_id = retry_queue_.top().task_id;
            retry_queue_.pop();
            graph_.requeue(task_id);
        }
    }

    void execute(const std::string& task_id, Task::Work work,
                 const TaskContext& context, const std::string& worker_id,
                 std::uint64_t run_token) {
        emit(EventType::TaskStarted, task_id, worker_id, graph_.idempotency_key(task_id));
        {
            std::lock_guard lock(mutex_);
            const auto now = std::chrono::steady_clock::now();
            last_heartbeats_[worker_id] = now;
            running_by_worker_[worker_id] = Assignment{
                task_id, run_token, now + lease_timeout_};
        }
        const auto started_at = std::chrono::steady_clock::now();
        std::atomic<bool> running_work{true};
        std::thread lease_renewer;
        if (renew_lease_while_running_.load()) {
            lease_renewer = std::thread([this, worker_id, &running_work] {
                while (running_work.load()) {
                    {
                        std::lock_guard lock(mutex_);
                        const auto now = std::chrono::steady_clock::now();
                        last_heartbeats_[worker_id] = now;
                        const auto assigned = running_by_worker_.find(worker_id);
                        if (assigned != running_by_worker_.end()) {
                            assigned->second.lease_expires_at = now + lease_timeout_;
                        }
                    }
                    emit(EventType::WorkerHeartbeat, {}, worker_id);
                    const auto slice = std::min(lease_timeout_ / 3, std::chrono::milliseconds(50));
                    const auto sleep_for = slice.count() > 0 ? slice : std::chrono::milliseconds(1);
                    std::this_thread::sleep_for(sleep_for);
                }
            });
        }
        try {
            work(context);
        } catch (...) {
            running_work.store(false);
            if (lease_renewer.joinable()) {
                lease_renewer.join();
            }
            if (!graph_.owns_run(task_id, run_token)) {
                return;
            }
            const auto delay = graph_.retry_delay(task_id);
            if (graph_.can_retry(task_id)) {
                {
                    std::lock_guard lock(mutex_);
                    retry_queue_.push(RetryEntry{
                        std::chrono::steady_clock::now() + delay, task_id});
                }
                 emit(EventType::TaskRetryScheduled, task_id, {},
                     graph_.idempotency_key(task_id));
                condition_.notify_all();
                return;
            }

            const auto terminal_count = graph_.fail(task_id, run_token);
            if (terminal_count == 0) {
                return;
            }
            std::lock_guard lock(mutex_);
            if (!failure_) {
                failure_ = std::current_exception();
            }
            pending_ -= terminal_count;
            condition_.notify_all();
            emit(EventType::TaskFailed, task_id, {}, graph_.idempotency_key(task_id));
            return;
        }

        running_work.store(false);
        if (lease_renewer.joinable()) {
            lease_renewer.join();
        }

        if (!graph_.owns_run(task_id, run_token)) {
            return;
        }
        if (context.cancellation && context.cancellation->load()) {
            const auto terminal_count = graph_.finish_cancelled(task_id, run_token);
            if (terminal_count != 0) {
                std::lock_guard lock(mutex_);
                pending_ -= terminal_count;
            }
            emit(EventType::TaskBlocked, task_id);
            condition_.notify_all();
            return;
        }
        if (graph_.deadline_exceeded(task_id, started_at)) {
            const auto terminal_count = graph_.fail(task_id, run_token);
            if (terminal_count == 0) {
                return;
            }
            {
                std::lock_guard lock(mutex_);
                if (!failure_) {
                    failure_ = std::make_exception_ptr(
                        std::runtime_error("task deadline exceeded: " + task_id));
                }
                pending_ -= terminal_count;
            }
            emit(EventType::TaskTimedOut, task_id);
            condition_.notify_all();
            return;
        }
        if (!graph_.complete(task_id, run_token)) {
            return;
        }
        emit(EventType::TaskCompleted, task_id);
        {
            std::lock_guard lock(mutex_);
            --pending_;
        }
        condition_.notify_all();
    }

    void emit(EventType type, const std::string& task_id = {},
              const std::string& worker_id = {},
              std::optional<std::string> idempotency_key = {}) const {
        if (event_sink_) {
            event_sink_(RuntimeEvent{type, task_id, worker_id, std::move(idempotency_key)});
        }
    }

    TaskGraph& graph_;
    const std::size_t worker_count_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<std::thread> workers_;
    std::priority_queue<RetryEntry, std::vector<RetryEntry>, std::greater<>> retry_queue_;
    std::exception_ptr failure_;
    std::size_t pending_ = 0;
    bool started_ = false;
    bool accepting_ = false;
    bool stopping_ = false;
    EventSink event_sink_;
    TaskSink task_sink_;
    TaskBatchSink task_batch_sink_;
    TaskResolver task_resolver_;
    std::atomic_size_t next_worker_id_{0};
    const std::chrono::milliseconds lease_timeout_;
    std::thread lease_monitor_;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> last_heartbeats_;
    std::unordered_map<std::string, std::uint64_t> reclaimed_tokens_;
    std::unordered_map<std::string, Assignment> running_by_worker_;
    std::atomic<bool> renew_lease_while_running_{true};
};
