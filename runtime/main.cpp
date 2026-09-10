#include "scheduler.hpp"

#ifdef AGENTOS_ENABLE_POSTGRES
#include "postgres_runtime_store.hpp"
#include "task_recovery.hpp"
#endif
#include "task_handler_registry.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

int main() {
    TaskGraph graph;
    TaskHandlerRegistry handlers;
    bool recovered_runtime = false;
    handlers.register_handler("sleep", [](const std::string& payload, const TaskContext& context) {
        std::cout << context.task_id << " running via registered handler\n";
        std::this_thread::sleep_for(parse_sleep_duration(payload));
    });
#ifdef AGENTOS_ENABLE_POSTGRES
    std::unique_ptr<PostgresRuntimeStore> postgres_store;
    Scheduler::EventSink event_sink;
    Scheduler::TaskSink task_sink;
    Scheduler::TaskBatchSink task_batch_sink;
    if (const auto* connection_string = std::getenv("AGENTOS_DATABASE_URL");
        connection_string != nullptr && *connection_string != '\0') {
        postgres_store = std::make_unique<PostgresRuntimeStore>(connection_string);
        restore_runtime_graph(graph, *postgres_store, handlers);
        recovered_runtime = graph.pending_size() != 0;
        task_sink = [&postgres_store](const Task& task) {
            postgres_store->persist_task(task);
        };
        task_batch_sink = [&postgres_store](const std::vector<Task>& tasks) {
            postgres_store->persist_tasks(tasks);
        };
        event_sink = [&postgres_store](const RuntimeEvent& event) {
            try {
                postgres_store->append(event);
            } catch (const std::exception& error) {
                std::cerr << "PostgreSQL event write failed: " << error.what() << '\n';
            }
        };
        std::cout << "PostgreSQL event storage enabled\n";
    }
#endif
    Scheduler::TaskResolver task_resolver = [&handlers](const Task& task) {
        return handlers.create_work(task);
    };
#ifdef AGENTOS_ENABLE_POSTGRES
    Scheduler scheduler(graph, 2, std::move(event_sink), {}, std::move(task_sink),
                        std::move(task_batch_sink), std::move(task_resolver));
#else
    Scheduler scheduler(graph, 2, {}, {}, {}, {}, std::move(task_resolver));
#endif
    scheduler.start();

    if (recovered_runtime) {
        scheduler.wait_until_idle();
        scheduler.shutdown();
        std::cout << "Recovered tasks completed\n";
        return 0;
    }

    scheduler.submit(Task{"A", {}, [] {
        std::cout << "A running\n";
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::cout << "A completed\n";
    }});
    scheduler.submit(Task{"B", {"A"}, [] {
        std::cout << "B running\n";
        std::this_thread::sleep_for(std::chrono::seconds(3));
        std::cout << "B completed\n";
    }});

    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    scheduler.submit_batch({
        Task{"C", {"A"}, [] {
            std::cout << "C running\n";
            std::this_thread::sleep_for(std::chrono::seconds(3));
            std::cout << "C completed\n";
        }},
        Task{"D", {"B", "C"}, [] {
            std::cout << "D running\n";
            std::this_thread::sleep_for(std::chrono::seconds(1));
            std::cout << "D completed\n";
        }},
    });
    Task registered_task{"E", {"D"}, Task::Work{}};
    registered_task.type = "sleep";
    registered_task.payload_json = "{\"seconds\":1}";
    scheduler.submit(std::move(registered_task));

    scheduler.wait_until_idle();
    scheduler.shutdown();
    std::cout << "All tasks completed\n";
    return 0;
}
