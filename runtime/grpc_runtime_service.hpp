#pragma once

#include "agentos.grpc.pb.h"
#include "scheduler.hpp"
#include "task_handler_registry.hpp"

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <string>
#include <vector>

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
                task.dependencies.assign(
                    definition.dependencies().begin(), definition.dependencies().end());
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
            const auto state = scheduler_.cancel(request->task_id());
            response->set_task_id(request->task_id());
            response->set_state(state);
            return grpc::Status::OK;
        } catch (const std::exception& error) {
            return grpc::Status(grpc::StatusCode::NOT_FOUND, error.what());
        }
    }

private:
    Scheduler& scheduler_;
    TaskHandlerRegistry& handlers_;
};
