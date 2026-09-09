#pragma once

#include "task.hpp"

#include <cstddef>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class TaskGraph {
public:
    void restore_tasks(const std::vector<Task>& tasks) {
        std::lock_guard lock(mutex_);
        for (const auto& task : tasks) {
            tasks_.emplace(task.id, task);
        }
        for (const auto& task : tasks) {
            std::size_t unresolved = 0;
            for (const auto& dependency : task.dependencies) {
                dependents_[dependency].push_back(task.id);
                if (tasks_.at(dependency).state != TaskState::Completed) {
                    ++unresolved;
                }
            }
            indegree_[task.id] = unresolved;
            if (task.state != TaskState::Completed &&
                task.state != TaskState::Failed &&
                task.state != TaskState::Blocked && unresolved == 0) {
                tasks_.at(task.id).state = TaskState::Ready;
                ready_.push(task.id);
            }
        }
    }

    void add_task(Task task) {
        std::lock_guard lock(mutex_);
        validate_new_task(task, {});
        validate_acyclic({task});
        insert_task(std::move(task));
    }

    void add_tasks(std::vector<Task> tasks) {
        std::lock_guard lock(mutex_);
        std::unordered_map<std::string, bool> incoming_ids;
        for (const auto& task : tasks) {
            if (task.id.empty() || tasks_.contains(task.id) || incoming_ids.contains(task.id)) {
                throw std::invalid_argument("duplicate or empty task id: " + task.id);
            }
            incoming_ids.emplace(task.id, true);
        }
        for (const auto& task : tasks) {
            validate_new_task(task, incoming_ids);
        }
        validate_acyclic(tasks);

        for (const auto& task : tasks) {
            tasks_.emplace(task.id, task);
        }
        for (const auto& task : tasks) {
            std::size_t unresolved = 0;
            for (const auto& dependency : task.dependencies) {
                dependents_[dependency].push_back(task.id);
                if (tasks_.at(dependency).state != TaskState::Completed) {
                    ++unresolved;
                }
            }
            indegree_[task.id] = unresolved;
            if (unresolved == 0) {
                ready_.push(task.id);
            }
        }
    }

    bool try_take_ready(std::string& task_id, Task::Work& work, TaskContext& context) {
        std::lock_guard lock(mutex_);
        if (ready_.empty()) {
            return false;
        }
        task_id = ready_.front();
        ready_.pop();
        Task& task = tasks_.at(task_id);
        task.state = TaskState::Running;
        ++task.attempts;
        work = task.work;
        context = TaskContext{
            task.id, task.attempts, task.idempotency_key, task.cancellation};
        return true;
    }

    std::chrono::milliseconds retry_delay(const std::string& task_id) const {
        std::lock_guard lock(mutex_);
        const Task& task = tasks_.at(task_id);
        return task.retry_delay * (1ULL << (task.attempts - 1));
    }

    bool can_retry(const std::string& task_id) const {
        std::lock_guard lock(mutex_);
        return tasks_.at(task_id).attempts < tasks_.at(task_id).max_attempts;
    }

    void requeue(const std::string& task_id) {
        std::lock_guard lock(mutex_);
        Task& task = tasks_.at(task_id);
        task.state = TaskState::Waiting;
        ready_.push(task_id);
    }

    std::size_t cancel(const std::string& task_id) {
        std::lock_guard lock(mutex_);
        std::size_t terminal_count = 0;
        Task& task = tasks_.at(task_id);
        if (task.cancellation) {
            task.cancellation->store(true);
        }
        if (task.state == TaskState::Waiting || task.state == TaskState::Ready) {
            mark_blocked(task_id, terminal_count);
        }
        return terminal_count;
    }

    bool deadline_exceeded(
        const std::string& task_id,
        std::chrono::steady_clock::time_point started_at) const {
        std::lock_guard lock(mutex_);
        const auto deadline = tasks_.at(task_id).deadline;
        return deadline.count() > 0 &&
               std::chrono::steady_clock::now() - started_at >= deadline;
    }

    void complete(const std::string& task_id) {
        std::lock_guard lock(mutex_);
        tasks_.at(task_id).state = TaskState::Completed;
        for (const auto& dependent : dependents_[task_id]) {
            const auto dependent_state = tasks_.at(dependent).state;
            if (dependent_state != TaskState::Waiting &&
                dependent_state != TaskState::Ready) {
                continue;
            }
            if (--indegree_[dependent] == 0) {
                ready_.push(dependent);
            }
        }
    }

    std::size_t fail(const std::string& task_id) {
        std::lock_guard lock(mutex_);
        std::size_t terminal_count = 0;
        mark_failed(task_id, terminal_count);
        return terminal_count;
    }

    bool is_terminal(const std::string& task_id) const {
        std::lock_guard lock(mutex_);
        const auto state = tasks_.at(task_id).state;
        return state == TaskState::Completed || state == TaskState::Failed ||
               state == TaskState::Blocked;
    }

    std::optional<std::string> idempotency_key(const std::string& task_id) const {
        std::lock_guard lock(mutex_);
        return tasks_.at(task_id).idempotency_key;
    }

    bool has_ready_tasks() const {
        std::lock_guard lock(mutex_);
        return !ready_.empty();
    }

    std::size_t size() const {
        std::lock_guard lock(mutex_);
        return tasks_.size();
    }

    std::size_t pending_size() const {
        std::lock_guard lock(mutex_);
        std::size_t pending = 0;
        for (const auto& [task_id, task] : tasks_) {
            (void)task_id;
            if (task.state != TaskState::Completed &&
                task.state != TaskState::Failed &&
                task.state != TaskState::Blocked) {
                ++pending;
            }
        }
        return pending;
    }

    Task snapshot(const std::string& task_id) const {
        std::lock_guard lock(mutex_);
        return tasks_.at(task_id);
    }

private:
    void mark_failed(const std::string& task_id, std::size_t& terminal_count) {
        Task& task = tasks_.at(task_id);
        if (task.state == TaskState::Failed || task.state == TaskState::Blocked ||
            task.state == TaskState::Completed) {
            return;
        }

        task.state = TaskState::Failed;
        ++terminal_count;
        for (const auto& dependent : dependents_[task_id]) {
            mark_blocked(dependent, terminal_count);
        }
    }

    void mark_blocked(const std::string& task_id, std::size_t& terminal_count) {
        Task& task = tasks_.at(task_id);
        if (task.state != TaskState::Waiting && task.state != TaskState::Ready) {
            return;
        }

        task.state = TaskState::Blocked;
        ++terminal_count;
        for (const auto& dependent : dependents_[task_id]) {
            mark_blocked(dependent, terminal_count);
        }
    }

    void validate_acyclic(const std::vector<Task>& incoming_tasks) const {
        std::unordered_set<std::string> incoming_ids;
        for (const auto& task : incoming_tasks) {
            incoming_ids.insert(task.id);
        }

        std::unordered_map<std::string, std::vector<std::string>> incoming_dependencies;
        for (const auto& task : incoming_tasks) {
            for (const auto& dependency : task.dependencies) {
                if (incoming_ids.contains(dependency)) {
                    incoming_dependencies[task.id].push_back(dependency);
                }
            }
        }

        std::unordered_set<std::string> visiting;
        std::unordered_set<std::string> visited;
        for (const auto& task : incoming_tasks) {
            const auto& task_id = task.id;
            if (visited.contains(task_id)) {
                continue;
            }
            if (has_cycle(task_id, incoming_dependencies, visiting, visited)) {
                throw std::invalid_argument("task graph contains a cycle");
            }
        }
    }

    static bool has_cycle(
        const std::string& task_id,
        const std::unordered_map<std::string, std::vector<std::string>>& dependencies,
        std::unordered_set<std::string>& visiting,
        std::unordered_set<std::string>& visited) {
        if (visiting.contains(task_id)) {
            return true;
        }
        if (visited.contains(task_id)) {
            return false;
        }

        visiting.insert(task_id);
        const auto dependencies_it = dependencies.find(task_id);
        if (dependencies_it == dependencies.end()) {
            visiting.erase(task_id);
            visited.insert(task_id);
            return false;
        }
        for (const auto& dependency : dependencies_it->second) {
            if (has_cycle(dependency, dependencies, visiting, visited)) {
                return true;
            }
        }
        visiting.erase(task_id);
        visited.insert(task_id);
        return false;
    }

    void validate_new_task(
        const Task& task,
        const std::unordered_map<std::string, bool>& incoming_ids) const {
        if (task.id.empty()) {
            throw std::invalid_argument("task id cannot be empty");
        }
        if (tasks_.contains(task.id)) {
            throw std::invalid_argument("duplicate task id: " + task.id);
        }
        if (task.idempotent &&
            (!task.idempotency_key || task.idempotency_key->empty())) {
            throw std::invalid_argument(
                "idempotent task requires a non-empty idempotency key: " + task.id);
        }
        for (const auto& dependency : task.dependencies) {
            if (!tasks_.contains(dependency) && !incoming_ids.contains(dependency)) {
                throw std::invalid_argument(
                    "task " + task.id + " depends on missing task: " + dependency);
            }
        }
    }

    void insert_task(Task task) {
        std::size_t unresolved = 0;
        bool blocked = false;
        for (const auto& dependency : task.dependencies) {
            dependents_[dependency].push_back(task.id);
            const auto dependency_state = tasks_.at(dependency).state;
            if (dependency_state == TaskState::Failed ||
                dependency_state == TaskState::Blocked) {
                blocked = true;
            } else if (dependency_state != TaskState::Completed) {
                ++unresolved;
            }
        }

        const std::string task_id = task.id;
        if (blocked) {
            task.state = TaskState::Blocked;
        }
        tasks_.emplace(task_id, std::move(task));
        indegree_[task_id] = unresolved;
        if (unresolved == 0 && !blocked) {
            ready_.push(task_id);
        }
    }

    mutable std::mutex mutex_;
    std::unordered_map<std::string, Task> tasks_;
    std::unordered_map<std::string, std::size_t> indegree_;
    std::unordered_map<std::string, std::vector<std::string>> dependents_;
    std::queue<std::string> ready_;
};
