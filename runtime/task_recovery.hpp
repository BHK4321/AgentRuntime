#pragma once

#include "postgres_runtime_store.hpp"
#include "task.hpp"
#include "task_graph.hpp"
#include "task_handler_registry.hpp"

#ifdef AGENTOS_ENABLE_POSTGRES

inline TaskState task_state_from_name(const std::string& name) {
    if (name == "Completed") {
        return TaskState::Completed;
    }
    if (name == "Failed") {
        return TaskState::Failed;
    }
    if (name == "Blocked") {
        return TaskState::Blocked;
    }
    if (name == "Ready") {
        return TaskState::Ready;
    }
    if (name == "Running") {
        return TaskState::Waiting;
    }
    return TaskState::Waiting;
}

inline std::vector<Task> load_recovered_tasks(
    PostgresRuntimeStore& store, TaskHandlerRegistry& handlers) {
    store.recover_stale_executions();
    std::vector<Task> recovered_tasks;
    for (const auto& persisted : store.load_recoverable_tasks()) {
        Task task;
        task.id = persisted.id;
        task.type = persisted.type;
        task.payload_json = persisted.payload_json;
        task.dependencies = persisted.dependencies;
        task.state = task_state_from_name(persisted.state);
        task.max_attempts = persisted.max_attempts;
        task.attempts = persisted.attempts;
        task.retry_delay = persisted.retry_delay;
        task.deadline = persisted.deadline;
        task.idempotent = persisted.idempotent;
        task.idempotency_key = persisted.idempotency_key;
        if (task.type != "cpp_callback") {
            task.work = handlers.resolve(task);
        }
        recovered_tasks.push_back(std::move(task));
    }
    return recovered_tasks;
}

inline void restore_runtime_graph(
    TaskGraph& graph, PostgresRuntimeStore& store, TaskHandlerRegistry& handlers) {
    graph.restore_tasks(load_recovered_tasks(store, handlers));
}

#endif
