#pragma once

#include "event_store.hpp"

#ifdef AGENTOS_ENABLE_POSTGRES

#include <pqxx/pqxx>
#include <mutex>
#include <stdexcept>
#include <string>

class PostgresEventStore final : public EventStore {
public:
    explicit PostgresEventStore(const std::string& connection_string)
        : connection_string_(connection_string) {}

    void append(const RuntimeEvent& event) override {
        std::lock_guard lock(mutex_);
        pqxx::connection connection(connection_string_);
        pqxx::work transaction(connection);
        transaction.exec_params(
            "INSERT INTO runtime_events "
            "(event_type, task_id, worker_id, idempotency_key) "
            "VALUES ($1, $2, $3, $4)",
            event_type_name(event.type), event.task_id, event.worker_id,
            event.idempotency_key.value_or("")
        );
        transaction.commit();
    }

private:
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
        }
        throw std::invalid_argument("unknown event type");
    }

    std::string connection_string_;
    std::mutex mutex_;
};

#endif