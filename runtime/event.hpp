#pragma once

#include <chrono>
#include <optional>
#include <string>

enum class EventType {
    TaskSubmitted,
    TaskStarted,
    TaskRetryScheduled,
    TaskCompleted,
    TaskFailed,
    TaskBlocked,
    TaskCancelled,
    TaskTimedOut,
    WorkerHeartbeat,
    WorkerLeaseExpired,
};

struct RuntimeEvent {
    EventType type;
    std::string task_id;
    std::string worker_id;
    std::optional<std::string> idempotency_key;
    std::chrono::steady_clock::time_point timestamp = std::chrono::steady_clock::now();
};
