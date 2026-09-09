#pragma once

#include <functional>
#include <chrono>
#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

enum class TaskState {
    Waiting,
    Ready,
    Running,
    Completed,
    Failed,
    Blocked,
};

struct TaskContext {
    std::string task_id;
    std::size_t attempt = 0;
    std::optional<std::string> idempotency_key;
    std::shared_ptr<std::atomic_bool> cancellation;
};

struct PersistedTask {
    std::string id;
    std::string type;
    std::string payload_json;
    std::string state;
    std::vector<std::string> dependencies;
    std::size_t max_attempts = 1;
    std::size_t attempts = 0;
    std::chrono::milliseconds retry_delay{0};
    std::chrono::milliseconds deadline{0};
    bool idempotent = false;
    std::optional<std::string> idempotency_key;
};

struct Task {
    class Work {
    public:
        Work() = default;

        template <typename Callable>
        Work(Callable callable) {
            if constexpr (std::is_invocable_v<Callable, const TaskContext&>) {
                callback_ = std::move(callable);
            } else {
                callback_ = [callable = std::move(callable)](const TaskContext&) mutable {
                    callable();
                };
            }
        }

        void operator()(const TaskContext& context) const {
            if (callback_) {
                callback_(context);
            }
        }

        explicit operator bool() const {
            return static_cast<bool>(callback_);
        }

    private:
        std::function<void(const TaskContext&)> callback_;
    };

    std::string id;
    std::vector<std::string> dependencies;
    Work work;
    TaskState state = TaskState::Waiting;
    std::size_t max_attempts = 1;
    std::size_t attempts = 0;
    std::chrono::milliseconds retry_delay{0};
    std::chrono::milliseconds deadline{0};
    std::shared_ptr<std::atomic_bool> cancellation = std::make_shared<std::atomic_bool>(false);
    bool idempotent = false;
    std::optional<std::string> idempotency_key;
    std::string type = "cpp_callback";
    std::string payload_json = "{}";
};
