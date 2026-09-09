#pragma once

#include "task.hpp"

#include <chrono>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>

inline std::chrono::milliseconds parse_sleep_duration(const std::string& payload_json) {
    const auto key = payload_json.find("\"seconds\"");
    if (key == std::string::npos) {
        throw std::invalid_argument("sleep payload requires a seconds field");
    }
    const auto colon = payload_json.find(':', key);
    if (colon == std::string::npos) {
        throw std::invalid_argument("sleep payload has invalid seconds field");
    }
    const auto start = payload_json.find_first_not_of(" \t\r\n", colon + 1);
    const auto end = payload_json.find_first_not_of("0123456789", start);
    if (start == std::string::npos || end == start) {
        throw std::invalid_argument("sleep seconds must be a non-negative integer");
    }
    return std::chrono::milliseconds(std::stoll(payload_json.substr(start, end - start)) * 1000);
}

class TaskHandlerRegistry {
public:
    using Handler = std::function<void(const std::string&, const TaskContext&)>;

    void register_handler(std::string type, Handler handler) {
        if (type.empty() || !handler) {
            throw std::invalid_argument("task handler requires a type and callable");
        }
        std::lock_guard lock(mutex_);
        handlers_[std::move(type)] = std::move(handler);
    }

    Task::Work create_work(const Task& task) const {
        std::lock_guard lock(mutex_);
        const auto handler = handlers_.find(task.type);
        if (handler == handlers_.end()) {
            throw std::invalid_argument("no task handler registered for type: " + task.type);
        }
        const auto payload = task.payload_json;
        const auto callback = handler->second;
        return Task::Work([payload, callback](const TaskContext& context) {
            callback(payload, context);
        });
    }

    bool contains(const std::string& type) const {
        std::lock_guard lock(mutex_);
        return handlers_.contains(type);
    }

    Task::Work resolve(const Task& task) const {
        return create_work(task);
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Handler> handlers_;
};