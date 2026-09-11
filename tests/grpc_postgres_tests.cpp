#include "grpc_runtime_service.hpp"
#include "postgres_runtime_store.hpp"
#include "scheduler.hpp"
#include "task_handler_registry.hpp"
#include "task_recovery.hpp"

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include <pqxx/pqxx>

#include "agentos.grpc.pb.h"

const char* require_database_url() {
    const auto* url = std::getenv("AGENTOS_DATABASE_URL");
    if (url == nullptr || *url == '\0') {
        throw std::runtime_error("AGENTOS_DATABASE_URL is required for gRPC/Postgres tests");
    }
    return url;
}

std::string unique_prefix(const char* label) {
    return std::string("grpc-") + label + "-" +
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
}

void cleanup_prefix(const std::string& url, const std::string& prefix) {
    pqxx::connection connection(url);
    pqxx::work transaction(connection);
    const auto like = transaction.quote(prefix + "%");
    transaction.exec("DELETE FROM executions WHERE task_id LIKE " + like);
    transaction.exec(
        "DELETE FROM task_dependencies WHERE task_id LIKE " + like +
        " OR dependency_id LIKE " + like);
    transaction.exec("DELETE FROM runtime_events WHERE task_id LIKE " + like);
    transaction.exec("DELETE FROM tasks WHERE id LIKE " + like);
    transaction.commit();
}

std::string task_state(const std::string& url, const std::string& task_id) {
    pqxx::connection connection(url);
    pqxx::read_transaction transaction(connection);
    const auto row = transaction.exec_params("SELECT state FROM tasks WHERE id = $1", task_id);
    if (row.empty()) {
        return {};
    }
    return row[0][0].as<std::string>();
}

int count_events(const std::string& url, const std::string& task_id, const std::string& event_type) {
    pqxx::connection connection(url);
    pqxx::read_transaction transaction(connection);
    const auto row = transaction.exec_params(
        "SELECT COUNT(*) FROM runtime_events WHERE task_id = $1 AND event_type = $2",
        task_id, event_type);
    return row[0][0].as<int>();
}

int count_executions(const std::string& url, const std::string& task_id, const std::string& status) {
    pqxx::connection connection(url);
    pqxx::read_transaction transaction(connection);
    const auto row = transaction.exec_params(
        "SELECT COUNT(*) FROM executions WHERE task_id = $1 AND status = $2",
        task_id, status);
    return row[0][0].as<int>();
}

void wait_for_event(const std::string& url, const std::string& task_id,
                    const std::string& event_type, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (count_events(url, task_id, event_type) >= 1) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    throw std::runtime_error("timed out waiting for " + event_type + " on " + task_id +
                             ", state=" + task_state(url, task_id));
}

void wait_for_state(const std::string& url, const std::string& task_id,
                    const std::string& expected, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (task_state(url, task_id) == expected) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    throw std::runtime_error("timed out waiting for " + task_id + " to become " + expected +
                             ", last state=" + task_state(url, task_id));
}

struct Harness {
    struct Config {
        std::chrono::milliseconds lease_timeout = std::chrono::seconds(5);
        bool restore = false;
        std::chrono::milliseconds sleep_for{-1};
        bool hold_until_cancel = false;
        bool renew_leases = true;
        bool sleep_only_first_attempt = false;
        std::size_t worker_count = 2;
    };

    explicit Harness(const char* url, Config config = {})
        : store(url) {
        if (config.hold_until_cancel) {
            handlers.register_handler("sleep", [](const std::string&, const TaskContext& context) {
                while (context.cancellation && !context.cancellation->load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            });
        } else if (config.sleep_for.count() >= 0) {
            auto runs = std::make_shared<std::atomic<int>>(0);
            const auto once = config.sleep_only_first_attempt;
            const auto sleep_for = config.sleep_for;
            handlers.register_handler("sleep", [sleep_for, once, runs](
                                                  const std::string&, const TaskContext&) {
                if (!once || ++(*runs) == 1) {
                    std::this_thread::sleep_for(sleep_for);
                }
            });
        } else {
            handlers.register_handler("sleep", [](const std::string& payload, const TaskContext&) {
                std::this_thread::sleep_for(parse_sleep_duration(payload));
            });
        }
        if (config.restore) {
            restore_runtime_graph(graph, store, handlers);
        }
        scheduler = std::make_unique<Scheduler>(
            graph, config.worker_count,
            [this](const RuntimeEvent& event) { store.append(event); },
            config.lease_timeout,
            [this](const Task& task) { store.persist_task(task); },
            [this](const std::vector<Task>& tasks) { store.persist_tasks(tasks); },
            [this](const Task& task) { return handlers.resolve(task); });
        scheduler->set_renew_lease_while_running(config.renew_leases);
        scheduler->start();
        service = std::make_unique<RuntimeService>(*scheduler, handlers);
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(service.get());
        server = builder.BuildAndStart();
        if (!server || port == 0) {
            throw std::runtime_error("failed to start test gRPC server");
        }
        stub = agentos::Runtime::NewStub(
            grpc::CreateChannel("127.0.0.1:" + std::to_string(port),
                                grpc::InsecureChannelCredentials()));
    }

    void stop() {
        if (server) {
            server->Shutdown();
            server->Wait();
            server.reset();
        }
        if (scheduler) {
            scheduler->shutdown();
        }
    }

    ~Harness() {
        try {
            stop();
        } catch (...) {
        }
    }

    PostgresRuntimeStore store;
    TaskHandlerRegistry handlers;
    TaskGraph graph;
    std::unique_ptr<Scheduler> scheduler;
    std::unique_ptr<RuntimeService> service;
    grpc::ServerBuilder builder;
    int port = 0;
    std::unique_ptr<grpc::Server> server;
    std::unique_ptr<agentos::Runtime::Stub> stub;
};

void submit_sleep(agentos::Runtime::Stub& stub, const std::string& task_id, int seconds) {
    agentos::SubmitWorkflowRequest request;
    auto* task = request.add_tasks();
    task->set_id(task_id);
    task->set_type("sleep");
    task->set_payload_json("{\"seconds\":" + std::to_string(seconds) + "}");
    agentos::SubmitWorkflowResponse response;
    grpc::ClientContext context;
    const auto status = stub.SubmitWorkflow(&context, request, &response);
    if (!status.ok()) {
        throw std::runtime_error("SubmitWorkflow failed: " + status.error_message());
    }
}

void test_grpc_submit_persists_to_postgres() {
    const auto* url = require_database_url();
    const auto prefix = unique_prefix("submit");
    const auto first = prefix + "-a";
    const auto second = prefix + "-b";
    cleanup_prefix(url, prefix);

    Harness harness(url);
    agentos::SubmitWorkflowRequest request;
    auto* task_a = request.add_tasks();
    task_a->set_id(first);
    task_a->set_type("sleep");
    task_a->set_payload_json("{\"seconds\":0}");
    auto* task_b = request.add_tasks();
    task_b->set_id(second);
    task_b->set_type("sleep");
    task_b->set_payload_json("{\"seconds\":0}");
    task_b->add_dependencies(first);
    agentos::SubmitWorkflowResponse response;
    grpc::ClientContext context;
    const auto status = harness.stub->SubmitWorkflow(&context, request, &response);
    if (!status.ok()) {
        throw std::runtime_error("SubmitWorkflow failed: " + status.error_message());
    }
    if (response.task_ids_size() != 2) {
        throw std::runtime_error("expected 2 submitted task ids");
    }

    wait_for_state(url, first, "Completed", std::chrono::seconds(5));
    wait_for_state(url, second, "Completed", std::chrono::seconds(5));
    cleanup_prefix(url, prefix);
}

void test_grpc_cancel_returns_graph_state() {
    const auto* url = require_database_url();
    const auto prefix = unique_prefix("cancel");
    const auto running = prefix + "-run";
    const auto waiting = prefix + "-wait";
    cleanup_prefix(url, prefix);

    Harness::Config cancel_config;
    cancel_config.hold_until_cancel = true;
    Harness harness(url, cancel_config);
    agentos::SubmitWorkflowRequest request;
    auto* task_a = request.add_tasks();
    task_a->set_id(running);
    task_a->set_type("sleep");
    task_a->set_payload_json("{\"seconds\":1}");
    auto* task_b = request.add_tasks();
    task_b->set_id(waiting);
    task_b->set_type("sleep");
    task_b->set_payload_json("{\"seconds\":0}");
    task_b->add_dependencies(running);
    agentos::SubmitWorkflowResponse submitted;
    grpc::ClientContext submit_context;
    const auto submitted_status = harness.stub->SubmitWorkflow(&submit_context, request, &submitted);
    if (!submitted_status.ok()) {
        throw std::runtime_error("SubmitWorkflow failed: " + submitted_status.error_message());
    }

    wait_for_event(url, running, "TaskStarted", std::chrono::seconds(3));

    agentos::CancelTaskRequest cancel_wait;
    cancel_wait.set_task_id(waiting);
    agentos::TaskStatus wait_status;
    grpc::ClientContext wait_context;
    const auto wait_rpc = harness.stub->CancelTask(&wait_context, cancel_wait, &wait_status);
    if (!wait_rpc.ok()) {
        throw std::runtime_error("CancelTask(waiting) failed: " + wait_rpc.error_message());
    }
    if (wait_status.state() != "Blocked") {
        throw std::runtime_error("expected waiting cancel state Blocked, got " + wait_status.state());
    }

    agentos::CancelTaskRequest cancel_run;
    cancel_run.set_task_id(running);
    agentos::TaskStatus run_status;
    grpc::ClientContext run_context;
    const auto run_rpc = harness.stub->CancelTask(&run_context, cancel_run, &run_status);
    if (!run_rpc.ok()) {
        throw std::runtime_error("CancelTask(running) failed: " + run_rpc.error_message());
    }
    if (run_status.state() != "Running") {
        throw std::runtime_error("expected running cancel state Running, got " + run_status.state());
    }

    wait_for_state(url, running, "Blocked", std::chrono::seconds(3));
    wait_for_state(url, waiting, "Blocked", std::chrono::seconds(2));
    cleanup_prefix(url, prefix);
}

void test_grpc_recovery_resumes_persisted_work() {
    const auto* url = require_database_url();
    const auto prefix = unique_prefix("recovery");
    const auto task_id = prefix + "-a";
    cleanup_prefix(url, prefix);

    {
        PostgresRuntimeStore store(url);
        Task task;
        task.id = task_id;
        task.type = "sleep";
        task.payload_json = "{\"seconds\":0}";
        store.persist_task(task);
        pqxx::connection connection(url);
        pqxx::work transaction(connection);
        transaction.exec_params(
            "INSERT INTO workers (id, status) VALUES ('dead-worker', 'LOST') "
            "ON CONFLICT (id) DO UPDATE SET status = 'LOST', updated_at = now()");
        transaction.exec_params(
            "UPDATE tasks SET state = 'Running', attempts = 1, updated_at = now() WHERE id = $1",
            task_id);
        transaction.exec_params(
            "INSERT INTO executions (task_id, worker_id, attempt, status, lease_expires_at) "
            "VALUES ($1, 'dead-worker', 1, 'RUNNING', now() + interval '30 seconds')",
            task_id);
        transaction.commit();
    }

    Harness::Config recovery_config;
    recovery_config.restore = true;
    Harness harness(url, recovery_config);
    wait_for_state(url, task_id, "Completed", std::chrono::seconds(5));
    if (count_executions(url, task_id, "ABANDONED") < 1) {
        throw std::runtime_error("expected abandoned execution after recovery");
    }
    if (count_executions(url, task_id, "COMPLETED") < 1) {
        throw std::runtime_error("expected completed execution after recovery");
    }
    cleanup_prefix(url, prefix);
}

void test_grpc_lease_steal_requeues_work() {
    const auto* url = require_database_url();
    const auto prefix = unique_prefix("lease");
    const auto task_id = prefix + "-a";
    cleanup_prefix(url, prefix);

    // First attempt sleeps past the lease. Later attempts return immediately so
    // the stolen run can complete; with renewal off, a long second sleep would
    // look dead and be reclaimed forever.
    Harness::Config lease_config;
    lease_config.lease_timeout = std::chrono::milliseconds(80);
    lease_config.renew_leases = false;
    lease_config.sleep_for = std::chrono::milliseconds(400);
    lease_config.sleep_only_first_attempt = true;
    lease_config.worker_count = 1;
    Harness harness(url, lease_config);
    harness.scheduler->set_renew_lease_while_running(false);

    submit_sleep(*harness.stub, task_id, 0);
    wait_for_event(url, task_id, "TaskReclaimed", std::chrono::seconds(5));
    wait_for_state(url, task_id, "Completed", std::chrono::seconds(5));
    if (count_events(url, task_id, "TaskStarted") < 2) {
        throw std::runtime_error("lease steal did not start a second attempt");
    }
    if (count_executions(url, task_id, "ABANDONED") < 1) {
        throw std::runtime_error("expected abandoned execution after lease steal");
    }
    if (count_executions(url, task_id, "COMPLETED") < 1) {
        throw std::runtime_error("expected completed execution after lease steal");
    }
    cleanup_prefix(url, prefix);
}

int main(int argc, char* argv[]) {
    try {
        if (argc == 1) {
            test_grpc_submit_persists_to_postgres();
            test_grpc_cancel_returns_graph_state();
            test_grpc_recovery_resumes_persisted_work();
            test_grpc_lease_steal_requeues_work();
            std::cout << "Passed gRPC/Postgres tests\n";
            return 0;
        }
        const std::string test_name = argv[1];
        if (test_name == "grpc_submit") {
            test_grpc_submit_persists_to_postgres();
        } else if (test_name == "grpc_cancel") {
            test_grpc_cancel_returns_graph_state();
        } else if (test_name == "grpc_recovery") {
            test_grpc_recovery_resumes_persisted_work();
        } else if (test_name == "grpc_lease_steal") {
            test_grpc_lease_steal_requeues_work();
        } else {
            std::cerr << "Unknown test: " << test_name << '\n';
            return 1;
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "Passed gRPC/Postgres tests\n";
    return 0;
}
