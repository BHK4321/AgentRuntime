#include "agentos.grpc.pb.h"
#include "postgres_runtime_store.hpp"
#include "scheduler.hpp"
#include "task_handler_registry.hpp"

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

class RuntimeService final : public agentos::Runtime::Service {
public:
    RuntimeService(Scheduler& scheduler, TaskHandlerRegistry& handlers)
        : scheduler_(scheduler), handlers_(handlers) {}

    grpc::Status SubmitWorkflow(
        grpc::ServerContext*, const agentos::SubmitWorkflowRequest* request,
        agentos::SubmitWorkflowResponse* response) override {
        try {
            std::vector<Task> tasks;
            tasks.reserve(request->tasks_size());
            for (const auto& definition : request->tasks()) {
                Task task;
                task.id = definition.id();
                task.type = definition.type();
                task.payload_json = definition.payload_json();
                task.dependencies.assign(definition.dependencies().begin(), definition.dependencies().end());
                task.max_attempts = definition.max_attempts() == 0 ? 1 : definition.max_attempts();
                task.retry_delay = std::chrono::milliseconds(definition.retry_delay_ms());
                task.deadline = std::chrono::milliseconds(definition.deadline_ms());
                task.idempotent = definition.idempotent();
                if (!definition.idempotency_key().empty()) {
                    task.idempotency_key = definition.idempotency_key();
                }
                if (task.type != "cpp_callback") {
                    task.work = handlers_.resolve(task);
                }
                tasks.push_back(std::move(task));
            }
            scheduler_.submit_batch(std::move(tasks));
            for (const auto& definition : request->tasks()) {
                response->add_task_ids(definition.id());
            }
            return grpc::Status::OK;
        } catch (const std::exception& error) {
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, error.what());
        }
    }

    grpc::Status CancelTask(
        grpc::ServerContext*, const agentos::CancelTaskRequest* request,
        agentos::TaskStatus* response) override {
        try {
            scheduler_.cancel(request->task_id());
            response->set_task_id(request->task_id());
            response->set_state("Blocked");
            return grpc::Status::OK;
        } catch (const std::exception& error) {
            return grpc::Status(grpc::StatusCode::NOT_FOUND, error.what());
        }
    }

private:
    Scheduler& scheduler_;
    TaskHandlerRegistry& handlers_;
};

int main() {
    const auto* connection_string = std::getenv("AGENTOS_DATABASE_URL");
    if (connection_string == nullptr || *connection_string == '\0') {
        std::cerr << "AGENTOS_DATABASE_URL is required\n";
        return 1;
    }

    PostgresRuntimeStore store(connection_string);
    TaskHandlerRegistry handlers;
    handlers.register_handler("sleep", [](const std::string& payload, const TaskContext&) {
        std::this_thread::sleep_for(parse_sleep_duration(payload));
    });

    TaskGraph graph;
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
    Scheduler scheduler(graph, 4, std::move(event_sink), {},
                        std::move(task_sink), std::move(task_batch_sink),
                        std::move(resolver));
    scheduler.start();

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
