#pragma once

#include "event_store.hpp"
#include "task.hpp"

#ifdef AGENTOS_ENABLE_POSTGRES

#include <pqxx/pqxx>
#include <mutex>
#include <string>

class PostgresRuntimeStore final : public EventStore {
public:
    explicit PostgresRuntimeStore(std::string connection_string)
        : connection_string_(std::move(connection_string)) {}

    void persist_task(const Task& task) {
        persist_tasks(std::vector<Task>{task});
    }

    void persist_tasks(const std::vector<Task>& tasks) {
        std::lock_guard lock(mutex_);
        pqxx::connection connection(connection_string_);
        pqxx::work transaction(connection);
        for (const auto& task : tasks) {
            transaction.exec_params(
                "INSERT INTO tasks (id, type, payload_json, state, max_attempts, "
                "retry_delay_ms, deadline_ms, idempotent, idempotency_key) "
                "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9) "
                "ON CONFLICT (id) DO NOTHING",
                task.id, task.type, task.payload_json, task_state_name(task.state),
                task.max_attempts, task.retry_delay.count(), task.deadline.count(),
                task.idempotent, task.idempotency_key.value_or("")
            );
            for (const auto& dependency : task.dependencies) {
                transaction.exec_params(
                    "INSERT INTO task_dependencies (task_id, dependency_id) VALUES ($1, $2) "
                    "ON CONFLICT DO NOTHING", task.id, dependency);
            }
        }
        transaction.commit();
    }

    void recover_stale_executions() {
        std::lock_guard lock(mutex_);
        pqxx::connection connection(connection_string_);
        pqxx::work transaction(connection);
        // Single-process restart: every RUNNING row belonged to dead workers.
        transaction.exec(
            "WITH stale AS ("
            " UPDATE executions SET status = 'ABANDONED', finished_at = now(), "
            " lease_expires_at = NULL WHERE status = 'RUNNING' "
            " RETURNING task_id, worker_id"
            "), reset AS ("
            " UPDATE tasks SET state = 'Waiting', updated_at = now() "
            " WHERE id IN (SELECT task_id FROM stale) AND state = 'Running' RETURNING id"
            ") "
            "INSERT INTO runtime_events (event_type, task_id, worker_id) "
            "SELECT 'WorkerLeaseExpired', stale.task_id, COALESCE(stale.worker_id, '') "
            "FROM stale"
        );
        transaction.exec(
            "UPDATE workers SET status = 'LOST', updated_at = now() "
            "WHERE last_heartbeat < now() - interval '5 seconds'"
        );
        transaction.commit();
    }

    std::vector<PersistedTask> load_recoverable_tasks() {
        std::lock_guard lock(mutex_);
        pqxx::connection connection(connection_string_);
        pqxx::read_transaction transaction(connection);
        const auto rows = transaction.exec(
            "WITH RECURSIVE recover(id) AS ("
            " SELECT id FROM tasks WHERE type <> 'cpp_callback' "
            " AND state NOT IN ('Completed', 'Failed', 'Blocked')"
            " UNION"
            " SELECT d.dependency_id FROM task_dependencies d JOIN recover r ON d.task_id = r.id"
            ") SELECT t.id, t.type, t.payload_json, t.state, t.max_attempts, t.attempts, "
            "t.retry_delay_ms, t.deadline_ms, t.idempotent, t.idempotency_key "
            "FROM tasks t JOIN recover r ON r.id = t.id "
            "ORDER BY t.created_at, t.id");
        std::vector<PersistedTask> result;
        for (const auto& row : rows) {
            PersistedTask task;
            task.id = row[0].as<std::string>();
            task.type = row[1].as<std::string>();
            task.payload_json = row[2].as<std::string>();
            task.state = row[3].as<std::string>();
            task.max_attempts = row[4].as<std::size_t>();
            task.attempts = row[5].as<std::size_t>();
            task.retry_delay = std::chrono::milliseconds(row[6].as<long long>());
            task.deadline = std::chrono::milliseconds(row[7].as<long long>());
            task.idempotent = row[8].as<bool>();
            if (!row[9].is_null() && !row[9].as<std::string>().empty()) {
                task.idempotency_key = row[9].as<std::string>();
            }
            const auto dependencies = transaction.exec_params(
                "SELECT dependency_id FROM task_dependencies WHERE task_id = $1 ORDER BY dependency_id",
                task.id);
            for (const auto& dependency : dependencies) {
                task.dependencies.push_back(dependency[0].as<std::string>());
            }
            result.push_back(std::move(task));
        }
        return result;
    }

    void append(const RuntimeEvent& event) override {
        std::lock_guard lock(mutex_);
        pqxx::connection connection(connection_string_);
        pqxx::work transaction(connection);
        if (!event.worker_id.empty()) {
            transaction.exec_params(
                "INSERT INTO workers (id) VALUES ($1) ON CONFLICT (id) DO UPDATE SET "
                "status = 'ACTIVE', last_heartbeat = now(), updated_at = now()",
                event.worker_id);
            if (event.type == EventType::WorkerHeartbeat) {
                transaction.exec_params(
                    "UPDATE executions SET lease_expires_at = now() + interval '5 seconds' "
                    "WHERE worker_id = $1 AND status = 'RUNNING'",
                    event.worker_id);
                transaction.commit();
                return;
            }
        }
        transaction.exec_params(
            "INSERT INTO runtime_events "
            "(event_type, task_id, worker_id, idempotency_key) "
            "VALUES ($1, $2, $3, $4)",
            event_type_name(event.type), event.task_id, event.worker_id,
            event.idempotency_key.value_or("")
        );
        if (!event.task_id.empty()) {
            const auto state = state_for_event(event.type);
            if (event.type == EventType::TaskCancelled) {
                // Waiting/Ready cancels persist Blocked via TaskBlocked.
                // Running cancels stay Running until the worker observes the flag.
            } else if (state != nullptr) {
                transaction.exec_params(
                    "UPDATE tasks SET state = $1, updated_at = now() WHERE id = $2",
                    state, event.task_id);
            }
            if (event.type == EventType::TaskStarted) {
                if (!event.worker_id.empty()) {
                    transaction.exec_params(
                        "INSERT INTO workers (id) VALUES ($1) ON CONFLICT (id) DO UPDATE SET "
                        "status = 'ACTIVE', last_heartbeat = now(), updated_at = now()",
                        event.worker_id);
                }
                transaction.exec_params(
                    "INSERT INTO executions (task_id, worker_id, attempt, status, "
                    "lease_expires_at) SELECT id, $2, attempts + 1, 'RUNNING', "
                    "now() + interval '5 seconds' FROM tasks WHERE id = $1",
                    event.task_id, event.worker_id);
                transaction.exec_params(
                    "UPDATE tasks SET attempts = attempts + 1, updated_at = now() "
                    "WHERE id = $1", event.task_id);
            }
            if (event.type == EventType::TaskReclaimed) {
                // Fence by worker: a late WorkerLeaseExpired must not abandon the
                // replacement attempt that already holds a new RUNNING row.
                transaction.exec_params(
                    "UPDATE executions SET status = 'ABANDONED', finished_at = now(), "
                    "lease_expires_at = NULL WHERE task_id = $1 AND status = 'RUNNING' "
                    "AND worker_id = $2",
                    event.task_id, event.worker_id);
                transaction.exec_params(
                    "INSERT INTO executions (task_id, worker_id, attempt, status, finished_at) "
                    "SELECT $1, $2, 0, 'ABANDONED', now() WHERE NOT EXISTS ("
                    " SELECT 1 FROM executions WHERE task_id = $1 AND status = 'ABANDONED')",
                    event.task_id, event.worker_id);
                transaction.exec_params(
                    "UPDATE tasks SET state = 'Waiting', updated_at = now() WHERE id = $1 "
                    "AND state = 'Running'",
                    event.task_id);
            }
            if (event.type == EventType::TaskCompleted ||
                event.type == EventType::TaskFailed ||
                event.type == EventType::TaskTimedOut) {
                transaction.exec_params(
                    "UPDATE executions SET status = $1, finished_at = now(), "
                    "lease_expires_at = NULL WHERE task_id = $2 AND status = 'RUNNING'",
                    event.type == EventType::TaskCompleted ? "COMPLETED" : "FAILED",
                    event.task_id);
            }
        }
        if (event.type == EventType::WorkerLeaseExpired && !event.worker_id.empty()) {
            transaction.exec_params(
                "UPDATE workers SET status = 'LOST', updated_at = now() WHERE id = $1",
                event.worker_id);
        }
        transaction.commit();
    }

private:
    static const char* task_state_name(TaskState state) {
        switch (state) {
        case TaskState::Waiting: return "Waiting";
        case TaskState::Ready: return "Ready";
        case TaskState::Running: return "Running";
        case TaskState::Completed: return "Completed";
        case TaskState::Failed: return "Failed";
        case TaskState::Blocked: return "Blocked";
        }
        return "Waiting";
    }

    static const char* state_for_event(EventType type) {
        switch (type) {
        case EventType::TaskStarted: return "Running";
        case EventType::TaskRetryScheduled: return "Waiting";
        case EventType::TaskCompleted: return "Completed";
        case EventType::TaskFailed: return "Failed";
        case EventType::TaskBlocked: return "Blocked";
        case EventType::TaskCancelled: return nullptr;
        case EventType::TaskReclaimed: return "Waiting";
        case EventType::TaskTimedOut: return "Failed";
        default: return nullptr;
        }
    }

    static const char* event_type_name(EventType type) {
        switch (type) {
        case EventType::TaskSubmitted: return "TaskSubmitted";
        case EventType::TaskStarted: return "TaskStarted";
        case EventType::TaskRetryScheduled: return "TaskRetryScheduled";
        case EventType::TaskCompleted: return "TaskCompleted";
        case EventType::TaskFailed: return "TaskFailed";
        case EventType::TaskBlocked: return "TaskBlocked";
        case EventType::TaskCancelled: return "TaskCancelled";
        case EventType::TaskTimedOut: return "TaskTimedOut";
        case EventType::WorkerHeartbeat: return "WorkerHeartbeat";
        case EventType::WorkerLeaseExpired: return "WorkerLeaseExpired";
        case EventType::TaskReclaimed: return "TaskReclaimed";
        }
        return "Unknown";
    }

    std::string connection_string_;
    std::mutex mutex_;
};

#endif