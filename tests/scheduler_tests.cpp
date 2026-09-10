#include "scheduler.hpp"
#include "durable_event_store.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <functional>
#include <iostream>
#include <filesystem>
#include <string>
#include <thread>

void expect_invalid_argument(const std::function<void()>& action) {
    bool threw = false;
    try {
        action();
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    assert(threw);
}

void test_dependency_order() {
    TaskGraph graph;
    Scheduler scheduler(graph);
    std::atomic completed_a{false};
    std::atomic dependency_violation{false};

    scheduler.start();
    scheduler.submit(Task{"A", {}, [&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        completed_a = true;
    }});
    scheduler.submit(Task{"B", {"A"}, [&] {
        if (!completed_a) {
            dependency_violation = true;
        }
    }});
    scheduler.wait_until_idle();
    scheduler.shutdown();

    assert(!dependency_violation);
}

void test_independent_tasks_run_concurrently() {
    TaskGraph graph;
    Scheduler scheduler(graph, 2);
    std::atomic active{0};
    std::atomic maximum_active{0};

    const auto work = [&] {
        const auto current = ++active;
        auto maximum = maximum_active.load();
        while (current > maximum &&
               !maximum_active.compare_exchange_weak(maximum, current)) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        --active;
    };

    scheduler.start();
    scheduler.submit_batch({Task{"A", {}, work}, Task{"B", {}, work}});
    scheduler.wait_until_idle();
    scheduler.shutdown();

    assert(maximum_active == 2);
}

void test_dynamic_batch_submission() {
    TaskGraph graph;
    Scheduler scheduler(graph, 2);
    std::atomic completed_a{false};
    std::atomic completed_b{false};
    std::atomic completed_c{false};
    std::atomic dependency_violation{false};

    scheduler.start();
    scheduler.submit(Task{"A", {}, [&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        completed_a = true;
    }});

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    scheduler.submit_batch({
        Task{"B", {"A"}, [&] {
            if (!completed_a) {
                dependency_violation = true;
            }
            completed_b = true;
        }},
        Task{"C", {"A"}, [&] {
            if (!completed_a) {
                dependency_violation = true;
            }
            completed_c = true;
        }},
        Task{"D", {"B", "C"}, [&] {
            if (!completed_b || !completed_c) {
                dependency_violation = true;
            }
        }},
    });

    scheduler.wait_until_idle();
    scheduler.shutdown();

    assert(!dependency_violation);
}

void test_mixed_submission_order() {
    TaskGraph graph;
    Scheduler scheduler(graph, 2);
    std::atomic completed_a{false};
    std::atomic completed_b{false};
    std::atomic completed_c{false};
    std::atomic completed_d{false};
    std::atomic dependency_violation{false};

    scheduler.start();
    scheduler.submit(Task{"A", {}, [&] { completed_a = true; }});
    scheduler.wait_until_idle();

    scheduler.submit(Task{"B", {"A"}, [&] {
        if (!completed_a) {
            dependency_violation = true;
        }
        completed_b = true;
    }});
    scheduler.submit_batch({
        Task{"D", {"B", "C"}, [&] {
            if (!completed_b || !completed_c) {
                dependency_violation = true;
            }
            completed_d = true;
        }},
        Task{"C", {"A"}, [&] {
            if (!completed_a) {
                dependency_violation = true;
            }
            completed_c = true;
        }},
    });

    scheduler.wait_until_idle();
    scheduler.shutdown();

    assert(completed_d);
    assert(!dependency_violation);
}

void test_submission_after_dependency_completion() {
    TaskGraph graph;
    Scheduler scheduler(graph);
    std::atomic completed_a{false};
    std::atomic completed_b{false};

    scheduler.start();
    scheduler.submit(Task{"A", {}, [&] { completed_a = true; }});
    scheduler.wait_until_idle();

    scheduler.submit(Task{"B", {"A"}, [&] {
        assert(completed_a);
        completed_b = true;
    }});
    scheduler.wait_until_idle();
    scheduler.shutdown();

    assert(completed_b);
}

void test_invalid_dependencies_are_rejected() {
    TaskGraph graph;
    expect_invalid_argument([&] {
        graph.add_task(Task{"B", {"missing"}, [] {}});
    });

    graph.add_task(Task{"A", {}, [] {}});
    expect_invalid_argument([&] {
        graph.add_task(Task{"A", {}, [] {}});
    });

    expect_invalid_argument([&] {
        graph.add_tasks({
            Task{"B", {"C"}, [] {}},
            Task{"C", {"B"}, [] {}},
        });
    });

    expect_invalid_argument([&] {
        graph.add_tasks({Task{"self", {"self"}, [] {}}});
    });

    expect_invalid_argument([&] {
        graph.add_tasks({
            Task{"X", {"Y"}, [] {}},
            Task{"Y", {"Z"}, [] {}},
            Task{"Z", {"X"}, [] {}},
        });
    });

    expect_invalid_argument([&] {
        graph.add_task(Task{"missing-key", {}, [] {}, TaskState::Waiting, 1, 0,
                            std::chrono::milliseconds(0), std::chrono::milliseconds(0),
                            std::make_shared<std::atomic_bool>(false), true});
    });
}

void test_rejected_batch_is_atomic() {
    TaskGraph graph;
    graph.add_task(Task{"A", {}, [] {}});
    const auto original_size = graph.size();

    expect_invalid_argument([&] {
        graph.add_tasks({
            Task{"B", {"A"}, [] {}},
            Task{"C", {"B", "C"}, [] {}},
        });
    });

    assert(graph.size() == original_size);
}

void test_incremental_validation_scopes_to_new_tasks() {
    TaskGraph graph;
    std::vector<Task> existing_tasks;
    constexpr std::size_t existing_count = 2000;
    existing_tasks.reserve(existing_count);
    for (std::size_t index = 0; index < existing_count; ++index) {
        existing_tasks.emplace_back("existing-" + std::to_string(index),
                                   std::vector<std::string>{}, [] {});
    }
    graph.add_tasks(std::move(existing_tasks));

    graph.add_task(Task{"new-task", {"existing-1999"}, [] {}});
    assert(graph.size() == existing_count + 1);
}

void test_failure_blocks_dependents() {
    TaskGraph graph;
    Scheduler scheduler(graph);
    std::atomic dependent_ran{false};

    scheduler.start();
    scheduler.submit(Task{"A", {}, [] {
        throw std::runtime_error("expected failure");
    }});
    scheduler.submit(Task{"B", {"A"}, [&] { dependent_ran = true; }});
    scheduler.submit(Task{"C", {"B"}, [&] { dependent_ran = true; }});

    bool reported_failure = false;
    try {
        scheduler.wait_until_idle();
    } catch (const std::runtime_error&) {
        reported_failure = true;
    }
    scheduler.shutdown();

    assert(reported_failure);
    assert(!dependent_ran);
}

void test_retry_then_success() {
    TaskGraph graph;
    Scheduler scheduler(graph);
    std::atomic attempts{0};
    std::atomic dependent_ran{false};

    scheduler.start();
    scheduler.submit(Task{"A", {}, [&] {
        if (++attempts < 3) {
            throw std::runtime_error("temporary failure");
        }
    }, TaskState::Waiting, 3, 0, std::chrono::milliseconds(1)});
    scheduler.submit(Task{"B", {"A"}, [&] { dependent_ran = true; }});
    scheduler.wait_until_idle();
    scheduler.shutdown();

    assert(attempts == 3);
    assert(dependent_ran);
}

void test_retry_limit_blocks_dependents() {
    TaskGraph graph;
    Scheduler scheduler(graph);
    std::atomic attempts{0};
    std::atomic dependent_ran{false};

    scheduler.start();
    scheduler.submit(Task{"A", {}, [&] {
        ++attempts;
        throw std::runtime_error("permanent failure");
    }, TaskState::Waiting, 2, 0, std::chrono::milliseconds(1)});
    scheduler.submit(Task{"B", {"A"}, [&] { dependent_ran = true; }});

    bool reported_failure = false;
    try {
        scheduler.wait_until_idle();
    } catch (const std::runtime_error&) {
        reported_failure = true;
    }
    scheduler.shutdown();

    assert(reported_failure);
    assert(attempts == 2);
    assert(!dependent_ran);
}

void test_events_are_emitted() {
    TaskGraph graph;
    std::atomic submitted{0};
    std::atomic started{0};
    std::atomic completed{0};
    Scheduler scheduler(graph, 1, [&](const RuntimeEvent& event) {
        if (event.type == EventType::TaskSubmitted) {
            ++submitted;
        } else if (event.type == EventType::TaskStarted) {
            ++started;
        } else if (event.type == EventType::TaskCompleted) {
            ++completed;
        }
    });

    scheduler.start();
    scheduler.submit(Task{"A", {}, [] {}});
    scheduler.wait_until_idle();
    scheduler.shutdown();

    assert(submitted == 1);
    assert(started == 1);
    assert(completed == 1);
}

void test_deadline_reports_failure() {
    TaskGraph graph;
    Scheduler scheduler(graph, 1);
    scheduler.start();
    scheduler.submit(Task{"A", {}, [] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }, TaskState::Waiting, 1, 0, std::chrono::milliseconds(0),
        std::chrono::milliseconds(1)});

    bool reported_timeout = false;
    try {
        scheduler.wait_until_idle();
    } catch (const std::runtime_error& error) {
        reported_timeout = std::string(error.what()).find("deadline") != std::string::npos;
    }
    scheduler.shutdown();
    assert(reported_timeout);
}

void test_pending_task_can_be_cancelled() {
    TaskGraph graph;
    Scheduler scheduler(graph, 1);
    std::atomic ran{false};
    scheduler.start();
    scheduler.submit(Task{"A", {}, [] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }});
    scheduler.submit(Task{"B", {"A"}, [&] { ran = true; }});
    scheduler.cancel("B");
    scheduler.wait_until_idle();
    scheduler.shutdown();
    assert(!ran);
}

void test_worker_lease_expiry_is_reported() {
    TaskGraph graph;
    std::atomic lease_expired{false};
    std::atomic reclaimed{false};
    std::atomic started{0};
    Scheduler scheduler(graph, 1, [&](const RuntimeEvent& event) {
        if (event.type == EventType::WorkerLeaseExpired) {
            lease_expired = true;
        } else if (event.type == EventType::TaskReclaimed) {
            reclaimed = true;
        } else if (event.type == EventType::TaskStarted) {
            ++started;
        }
    }, std::chrono::milliseconds(15));
    scheduler.set_renew_lease_while_running(false);

    scheduler.start();
    scheduler.submit(Task{"A", {}, [] {
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
    }});
    scheduler.wait_until_idle();
    scheduler.shutdown();

    assert(lease_expired);
    assert(reclaimed);
    assert(started >= 2);
}

void test_lease_renewal_prevents_false_reclaim() {
    TaskGraph graph;
    std::atomic reclaimed{false};
    std::atomic started{0};
    Scheduler scheduler(graph, 1, [&](const RuntimeEvent& event) {
        if (event.type == EventType::TaskReclaimed) {
            reclaimed = true;
        } else if (event.type == EventType::TaskStarted) {
            ++started;
        }
    }, std::chrono::milliseconds(15));

    scheduler.start();
    scheduler.submit(Task{"A", {}, [] {
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
    }});
    scheduler.wait_until_idle();
    scheduler.shutdown();

    assert(!reclaimed);
    assert(started == 1);
}

void test_cancel_reports_actual_state() {
    TaskGraph graph;
    Scheduler scheduler(graph, 1);
    scheduler.start();
    scheduler.submit(Task{"A", {}, [] {
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
    }});
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    scheduler.submit(Task{"B", {"A"}, [] {}});
    const auto waiting_state = scheduler.cancel("B");
    assert(waiting_state == std::string("Blocked"));
    const auto running_state = scheduler.cancel("A");
    assert(running_state == std::string("Running"));
    scheduler.wait_until_idle();
    scheduler.shutdown();
    assert(graph.state_of("A") == TaskState::Blocked);
    assert(graph.state_of("B") == TaskState::Blocked);
}

void test_optional_idempotency_key_is_reused_in_events() {
    TaskGraph graph;
    std::atomic attempts{0};
    std::vector<std::optional<std::string>> observed_keys;
    std::mutex keys_mutex;
    Scheduler scheduler(graph, 1, [&](const RuntimeEvent& event) {
        if (event.type == EventType::TaskStarted && event.task_id == "A") {
            std::lock_guard lock(keys_mutex);
            observed_keys.push_back(event.idempotency_key);
        }
    });

    scheduler.start();
    scheduler.submit(Task{"A", {}, [&] {
        if (++attempts == 1) {
            throw std::runtime_error("retry once");
        }
    }, TaskState::Waiting, 2, 0, std::chrono::milliseconds(1),
        std::chrono::milliseconds(0), std::make_shared<std::atomic_bool>(false), true,
        std::string("order-42")});
    scheduler.wait_until_idle();
    scheduler.shutdown();

    assert(attempts == 2);
    assert(observed_keys.size() == 2);
    assert(observed_keys[0] == std::optional<std::string>("order-42"));
    assert(observed_keys[1] == std::optional<std::string>("order-42"));
}

void test_context_work_receives_runtime_context() {
    TaskGraph graph;
    Scheduler scheduler(graph, 1);
    std::atomic attempts{0};
    std::vector<std::size_t> observed_attempts;
    std::mutex attempts_mutex;

    Task task{"A", {}, [] {}};
    task.max_attempts = 2;
    task.retry_delay = std::chrono::milliseconds(1);
    task.idempotent = true;
    task.idempotency_key = "payment-7";
    task.work = Task::Work{[&](const TaskContext& context) {
        {
            std::lock_guard lock(attempts_mutex);
            observed_attempts.push_back(context.attempt);
        }
        assert(context.task_id == "A");
        assert(context.idempotency_key == std::optional<std::string>("payment-7"));
        assert(context.cancellation);
        if (++attempts == 1) {
            throw std::runtime_error("retry once");
        }
    }};

    scheduler.start();
    scheduler.submit(std::move(task));
    scheduler.wait_until_idle();
    scheduler.shutdown();

    assert(observed_attempts.size() == 2);
    assert(observed_attempts[0] == 1);
    assert(observed_attempts[1] == 2);
}

void test_durable_event_store_replays_after_reopen() {
    const auto path = std::filesystem::path("build") / "test-events.log";
    std::filesystem::remove(path);

    {
        DurableEventStore store(path);
        store.append(RuntimeEvent{EventType::TaskSubmitted, "A", "", "key-1"});
        store.append(RuntimeEvent{EventType::TaskCompleted, "A", "worker-1"});
        assert(store.last_sequence() == 2);
    }

    DurableEventStore reopened(path);
    const auto events = reopened.replay();
    assert(events.size() == 2);
    assert(events[0].sequence == 1);
    assert(events[0].event.type == EventType::TaskSubmitted);
    assert(events[0].event.idempotency_key == std::optional<std::string>("key-1"));
    assert(events[1].sequence == 2);
    assert(events[1].event.type == EventType::TaskCompleted);

    std::filesystem::remove(path);
}

#ifdef AGENTOS_ENABLE_POSTGRES
#include "postgres_runtime_store.hpp"
#include "task_recovery.hpp"
#include "task_handler_registry.hpp"

#include <cstdlib>
#include <pqxx/pqxx>

const char* postgres_test_url() {
    return std::getenv("AGENTOS_DATABASE_URL");
}

void cleanup_postgres_prefix(pqxx::connection& connection, const std::string& prefix) {
    pqxx::work transaction(connection);
    transaction.exec(
        "DELETE FROM executions WHERE task_id LIKE " + transaction.quote(prefix + "%"));
    transaction.exec(
        "DELETE FROM task_dependencies WHERE task_id LIKE " + transaction.quote(prefix + "%") +
        " OR dependency_id LIKE " + transaction.quote(prefix + "%"));
    transaction.exec(
        "DELETE FROM runtime_events WHERE task_id LIKE " + transaction.quote(prefix + "%"));
    transaction.exec("DELETE FROM tasks WHERE id LIKE " + transaction.quote(prefix + "%"));
    transaction.commit();
}

void test_postgres_recovery_resumes_sleep_task() {
    const auto* url = postgres_test_url();
    if (url == nullptr || *url == '\0') {
        std::cout << "Skipped postgres_recovery (AGENTOS_DATABASE_URL unset)\n";
        return;
    }

    const auto prefix = "gtest-recovery-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto task_id = prefix + "-a";
    pqxx::connection connection(url);
    cleanup_postgres_prefix(connection, prefix);

    PostgresRuntimeStore store(url);
    Task task;
    task.id = task_id;
    task.type = "sleep";
    task.payload_json = "{\"seconds\":0}";
    store.persist_task(task);

    TaskGraph graph;
    TaskHandlerRegistry handlers;
    handlers.register_handler("sleep", [](const std::string&, const TaskContext&) {});
    restore_runtime_graph(graph, store, handlers);
    Scheduler scheduler(graph, 1, [&store](const RuntimeEvent& event) {
        store.append(event);
    }, std::chrono::seconds(5), {}, {}, [&handlers](const Task& recovered) {
        return handlers.resolve(recovered);
    });
    scheduler.start();
    scheduler.wait_until_idle();
    scheduler.shutdown();

    pqxx::read_transaction read(connection);
    const auto row = read.exec_params("SELECT state FROM tasks WHERE id = $1", task_id);
    assert(!row.empty());
    assert(row[0][0].as<std::string>() == "Completed");
    cleanup_postgres_prefix(connection, prefix);
}
#endif

int main(int argc, char* argv[]) {
    if (argc == 1) {
        test_dependency_order();
        test_independent_tasks_run_concurrently();
        test_dynamic_batch_submission();
        test_mixed_submission_order();
        test_submission_after_dependency_completion();
        test_invalid_dependencies_are_rejected();
        test_rejected_batch_is_atomic();
        test_incremental_validation_scopes_to_new_tasks();
        test_failure_blocks_dependents();
        test_retry_then_success();
        test_retry_limit_blocks_dependents();
        test_events_are_emitted();
        test_deadline_reports_failure();
        test_pending_task_can_be_cancelled();
        test_worker_lease_expiry_is_reported();
        test_lease_renewal_prevents_false_reclaim();
        test_cancel_reports_actual_state();
        test_optional_idempotency_key_is_reused_in_events();
        test_context_work_receives_runtime_context();
        test_durable_event_store_replays_after_reopen();
#ifdef AGENTOS_ENABLE_POSTGRES
        test_postgres_recovery_resumes_sleep_task();
#endif
        std::cout << "All AgentOS tests passed\n";
        return 0;
    }

    const std::string test_name = argv[1];
    if (test_name == "dependency_order") {
        test_dependency_order();
    } else if (test_name == "concurrent_tasks") {
        test_independent_tasks_run_concurrently();
    } else if (test_name == "dynamic_submission") {
        test_dynamic_batch_submission();
    } else if (test_name == "mixed_submission") {
        test_mixed_submission_order();
    } else if (test_name == "completed_dependency") {
        test_submission_after_dependency_completion();
    } else if (test_name == "validation") {
        test_invalid_dependencies_are_rejected();
    } else if (test_name == "atomic_rejection") {
        test_rejected_batch_is_atomic();
    } else if (test_name == "incremental_validation") {
        test_incremental_validation_scopes_to_new_tasks();
    } else if (test_name == "failure_propagation") {
        test_failure_blocks_dependents();
    } else if (test_name == "retry_success") {
        test_retry_then_success();
    } else if (test_name == "retry_exhausted") {
        test_retry_limit_blocks_dependents();
    } else if (test_name == "events") {
        test_events_are_emitted();
    } else if (test_name == "deadline") {
        test_deadline_reports_failure();
    } else if (test_name == "cancellation") {
        test_pending_task_can_be_cancelled();
    } else if (test_name == "lease_expiry") {
        test_worker_lease_expiry_is_reported();
    } else if (test_name == "lease_reclaim") {
        test_worker_lease_expiry_is_reported();
    } else if (test_name == "lease_renewal") {
        test_lease_renewal_prevents_false_reclaim();
    } else if (test_name == "cancel_state") {
        test_cancel_reports_actual_state();
    } else if (test_name == "idempotency") {
        test_optional_idempotency_key_is_reused_in_events();
    } else if (test_name == "task_context") {
        test_context_work_receives_runtime_context();
    } else if (test_name == "durable_events") {
        test_durable_event_store_replays_after_reopen();
#ifdef AGENTOS_ENABLE_POSTGRES
    } else if (test_name == "postgres_recovery") {
        test_postgres_recovery_resumes_sleep_task();
#endif
    } else {
        std::cerr << "Unknown test: " << test_name << '\n';
        return 1;
    }

    std::cout << "Passed: " << test_name << '\n';
}