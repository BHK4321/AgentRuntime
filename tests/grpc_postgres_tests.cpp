#include "grpc_runtime_service.hpp"
#include "postgres_runtime_store.hpp"
#include "scheduler.hpp"
#include "task_handler_registry.hpp"
#include "task_recovery.hpp"

#include <grpcpp/grpcpp.h>

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
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

void test_grpc_submit_persists_to_postgres() {
    const auto* url = require_database_url();
    const auto prefix = unique_prefix("submit");
    const auto first = prefix + "-a";
    const auto second = prefix + "-b";
    cleanup_prefix(url, prefix);

    PostgresRuntimeStore store(url);
    TaskHandlerRegistry handlers;
    handlers.register_handler("sleep", [](const std::string&, const TaskContext&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    });
    TaskGraph graph;
    restore_runtime_graph(graph, store, handlers);
    Scheduler scheduler(graph, 2, [&store](const RuntimeEvent& event) { store.append(event); },
                        std::chrono::seconds(5),
                        [&store](const Task& task) { store.persist_task(task); },
                        [&store](const std::vector<Task>& tasks) { store.persist_tasks(tasks); },
                        [&handlers](const Task& task) { return handlers.resolve(task); });
    scheduler.start();

    RuntimeService service(scheduler, handlers);
    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    assert(server);
    assert(port != 0);

    auto channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(port),
                                       grpc::InsecureChannelCredentials());
    auto stub = agentos::Runtime::NewStub(channel);
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
    const auto status = stub->SubmitWorkflow(&context, request, &response);
    assert(status.ok());
    assert(response.task_ids_size() == 2);

    wait_for_state(url, first, "Completed", std::chrono::seconds(5));
    wait_for_state(url, second, "Completed", std::chrono::seconds(5));

    server->Shutdown();
    scheduler.shutdown();
    cleanup_prefix(url, prefix);
}

void test_grpc_cancel_returns_graph_state() {
    const auto* url = require_database_url();
    const auto prefix = unique_prefix("cancel");
    const auto running = prefix + "-run";
    const auto waiting = prefix + "-wait";
    cleanup_prefix(url, prefix);

    PostgresRuntimeStore store(url);
    TaskHandlerRegistry handlers;
    handlers.register_handler("sleep", [](const std::string& payload, const TaskContext&) {
        std::this_thread::sleep_for(parse_sleep_duration(payload));
    });
    TaskGraph graph;
    Scheduler scheduler(graph, 1, [&store](const RuntimeEvent& event) { store.append(event); },
                        std::chrono::seconds(5),
                        [&store](const Task& task) { store.persist_task(task); },
                        [&store](const std::vector<Task>& tasks) { store.persist_tasks(tasks); },
                        [&handlers](const Task& task) { return handlers.resolve(task); });
    scheduler.start();

    RuntimeService service(scheduler, handlers);
    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    auto channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(port),
                                       grpc::InsecureChannelCredentials());
    auto stub = agentos::Runtime::NewStub(channel);

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
    assert(stub->SubmitWorkflow(&submit_context, request, &submitted).ok());

    wait_for_state(url, running, "Running", std::chrono::seconds(2));

    agentos::CancelTaskRequest cancel_wait;
    cancel_wait.set_task_id(waiting);
    agentos::TaskStatus wait_status;
    grpc::ClientContext wait_context;
    assert(stub->CancelTask(&wait_context, cancel_wait, &wait_status).ok());
    assert(wait_status.state() == "Blocked");

    agentos::CancelTaskRequest cancel_run;
    cancel_run.set_task_id(running);
    agentos::TaskStatus run_status;
    grpc::ClientContext run_context;
    assert(stub->CancelTask(&run_context, cancel_run, &run_status).ok());
    assert(run_status.state() == "Running");

    wait_for_state(url, running, "Blocked", std::chrono::seconds(3));
    wait_for_state(url, waiting, "Blocked", std::chrono::seconds(2));

    server->Shutdown();
    scheduler.shutdown();
    cleanup_prefix(url, prefix);
}

int main(int argc, char* argv[]) {
    try {
        if (argc == 1) {
            test_grpc_submit_persists_to_postgres();
            test_grpc_cancel_returns_graph_state();
            std::cout << "Passed gRPC/Postgres tests\n";
            return 0;
        }
        const std::string test_name = argv[1];
        if (test_name == "grpc_submit") {
            test_grpc_submit_persists_to_postgres();
        } else if (test_name == "grpc_cancel") {
            test_grpc_cancel_returns_graph_state();
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
