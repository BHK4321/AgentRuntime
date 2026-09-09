#pragma once

#include "event.hpp"
#include "event_store.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

enum class PersistedEventType {
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

struct PersistedEvent {
    std::uint64_t sequence;
    std::chrono::system_clock::time_point timestamp;
    RuntimeEvent event;
};

class DurableEventStore : public EventStore {
public:
    explicit DurableEventStore(std::filesystem::path path) : path_(std::move(path)) {
        if (path_.has_parent_path()) {
            std::filesystem::create_directories(path_.parent_path());
        }
        load_last_sequence();
    }

    void append(const RuntimeEvent& event) override {
        std::lock_guard lock(mutex_);
        std::ofstream output(path_, std::ios::app);
        if (!output) {
            throw std::runtime_error("could not open event store: " + path_.string());
        }

        const auto sequence = ++last_sequence_;
        const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        output << sequence << ' ' << timestamp << ' ' << event_type_name(event.type) << ' '
               << std::quoted(event.task_id) << ' ' << std::quoted(event.worker_id) << ' '
               << (event.idempotency_key.has_value() ? 1 : 0) << ' '
               << std::quoted(event.idempotency_key.value_or("")) << '\n';
        output.flush();
        if (!output) {
            throw std::runtime_error("could not write event store: " + path_.string());
        }
    }

    std::vector<PersistedEvent> replay() const {
        std::lock_guard lock(mutex_);
        std::ifstream input(path_);
        if (!input) {
            return {};
        }

        std::vector<PersistedEvent> events;
        std::uint64_t sequence;
        long long timestamp_ms;
        std::string type;
        std::string task_id;
        std::string worker_id;
        int has_key;
        std::string key;
        while (input >> sequence >> timestamp_ms >> type >> std::quoted(task_id)
                     >> std::quoted(worker_id) >> has_key >> std::quoted(key)) {
            events.push_back(PersistedEvent{
                sequence,
                std::chrono::system_clock::time_point(std::chrono::milliseconds(timestamp_ms)),
                RuntimeEvent{event_type_from_name(type), task_id, worker_id,
                             has_key ? std::optional<std::string>(key) : std::nullopt},
            });
        }
        return events;
    }

    std::uint64_t last_sequence() const {
        std::lock_guard lock(mutex_);
        return last_sequence_;
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

    static EventType event_type_from_name(const std::string& name) {
        static const std::vector<std::pair<const char*, EventType>> names{
            {"TaskSubmitted", EventType::TaskSubmitted},
            {"TaskStarted", EventType::TaskStarted},
            {"TaskRetryScheduled", EventType::TaskRetryScheduled},
            {"TaskCompleted", EventType::TaskCompleted},
            {"TaskFailed", EventType::TaskFailed},
            {"TaskBlocked", EventType::TaskBlocked},
            {"TaskCancelled", EventType::TaskCancelled},
            {"TaskTimedOut", EventType::TaskTimedOut},
            {"WorkerHeartbeat", EventType::WorkerHeartbeat},
            {"WorkerLeaseExpired", EventType::WorkerLeaseExpired},
        };
        for (const auto& [event_name, type] : names) {
            if (name == event_name) {
                return type;
            }
        }
        throw std::invalid_argument("unknown event type: " + name);
    }

    void load_last_sequence() {
        const auto events = replay();
        if (!events.empty()) {
            last_sequence_ = events.back().sequence;
        }
    }

    std::filesystem::path path_;
    mutable std::mutex mutex_;
    std::uint64_t last_sequence_ = 0;
};
