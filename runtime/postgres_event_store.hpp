#pragma once

#include "event_store.hpp"
#include "postgres_connection_pool.hpp"

#ifdef AGENTOS_ENABLE_POSTGRES

#include <pqxx/pqxx>
#include <cstddef>
#include <stdexcept>
#include <string>

class PostgresEventStore final : public EventStore {
public:
    explicit PostgresEventStore(const std::string& connection_string,
                                std::size_t pool_size = 8)
        : pool_(connection_string, pool_size) {}

    void append(const RuntimeEvent& event) override {
        auto connection = pool_.acquire();
        pqxx::work transaction(connection.connection());
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

    PostgresConnectionPool pool_;
};

#endif
