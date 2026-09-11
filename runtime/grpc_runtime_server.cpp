#include "grpc_runtime_service.hpp"
#include "postgres_runtime_store.hpp"
#include "scheduler.hpp"
#include "task_handler_registry.hpp"
#include "task_recovery.hpp"

#include <grpcpp/grpcpp.h>

#include <cstdlib>
#include <iostream>
#include <thread>

int main() {
    const auto* connection_string = std::getenv("AGENTOS_DATABASE_URL");
    if (connection_string == nullptr || *connection_string == '\0') {
        std::cerr << "AGENTOS_DATABASE_URL is required\n";
        return 1;
    }

    PostgresRuntimeStore store(connection_string, 4);
    TaskHandlerRegistry handlers;
    handlers.register_handler("sleep", [](const std::string& payload, const TaskContext&) {
        std::this_thread::sleep_for(parse_sleep_duration(payload));
    });

    TaskGraph graph;
    restore_runtime_graph(graph, store, handlers);
    Scheduler::EventSink event_sink = [&store](const RuntimeEvent& event) {
        store.append(event);
    };
    Scheduler::TaskSink task_sink = [&store](const Task& task) {
        store.persist_task(task);
    };
    Scheduler::TaskBatchSink task_batch_sink = [&store](const std::vector<Task>& tasks) {
        store.persist_tasks(tasks);
    };
    Scheduler::TaskResolver resolver = [&handlers](const Task& task) {
        return handlers.resolve(task);
    };
    Scheduler scheduler(graph, 4, std::move(event_sink),
                        std::chrono::seconds(5),
                        std::move(task_sink), std::move(task_batch_sink),
                        std::move(resolver));
    scheduler.start();
    if (graph.pending_size() != 0) {
        std::cout << "Recovered " << graph.pending_size()
                  << " unfinished task(s) from PostgreSQL\n";
    }

    RuntimeService service(scheduler, handlers);
    grpc::ServerBuilder builder;
    builder.AddListeningPort("0.0.0.0:50051", grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    if (!server) {
        std::cerr << "failed to start gRPC server\n";
        return 1;
    }
    std::cout << "AgentOS gRPC runtime listening on 0.0.0.0:50051\n";
    server->Wait();
    scheduler.shutdown();
    return 0;
}
