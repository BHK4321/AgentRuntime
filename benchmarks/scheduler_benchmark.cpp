#include "scheduler.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

struct ParallelMetrics {
    double elapsed_ms;
    std::size_t maximum_parallelism;
};

ParallelMetrics benchmark_parallel(std::size_t worker_count, std::size_t task_count) {
    TaskGraph graph;
    Scheduler scheduler(graph, worker_count);
    std::atomic<std::size_t> active{0};
    std::atomic<std::size_t> maximum_parallelism{0};

    scheduler.start();
    for (std::size_t index = 0; index < task_count; ++index) {
        scheduler.submit(Task{"task-" + std::to_string(index), {}, [&] {
            const auto current = active.fetch_add(1) + 1;
            auto maximum = maximum_parallelism.load();
            while (current > maximum &&
                   !maximum_parallelism.compare_exchange_weak(maximum, current)) {
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            active.fetch_sub(1);
        }});
    }

    const auto started = std::chrono::steady_clock::now();
    scheduler.wait_until_idle();
    const auto finished = std::chrono::steady_clock::now();
    scheduler.shutdown();

    return {
        std::chrono::duration<double, std::milli>(finished - started).count(),
        maximum_parallelism.load(),
    };
}

double benchmark_dynamic_submission(std::size_t task_count) {
    TaskGraph graph;
    Scheduler scheduler(graph, 4);
    scheduler.start();
    const auto started = std::chrono::steady_clock::now();

    for (std::size_t index = 0; index < task_count; ++index) {
        scheduler.submit(Task{"dynamic-" + std::to_string(index), {}, [] {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }});
    }

    scheduler.wait_until_idle();
    const auto finished = std::chrono::steady_clock::now();
    scheduler.shutdown();
    return std::chrono::duration<double, std::milli>(finished - started).count();
}

double benchmark_batch_submission(std::size_t task_count) {
    TaskGraph graph;
    Scheduler scheduler(graph, 16);
    std::vector<Task> tasks;
    tasks.reserve(task_count);
    for (std::size_t index = 0; index < task_count; ++index) {
        tasks.emplace_back("batch-" + std::to_string(index),
                           std::vector<std::string>{}, [] {
                               std::this_thread::sleep_for(std::chrono::milliseconds(1));
                           });
    }

    scheduler.start();
    const auto started = std::chrono::steady_clock::now();
    scheduler.submit_batch(std::move(tasks));
    scheduler.wait_until_idle();
    const auto finished = std::chrono::steady_clock::now();
    scheduler.shutdown();
    return std::chrono::duration<double, std::milli>(finished - started).count();
}

double benchmark_retry() {
    TaskGraph graph;
    Scheduler scheduler(graph, 2);
    std::atomic<std::size_t> attempts{0};
    scheduler.start();

    Task task{"retry", {}, [] {}};
    task.max_attempts = 4;
    task.retry_delay = std::chrono::milliseconds(2);
    task.work = [&](const TaskContext&) {
        if (attempts.fetch_add(1) < 3) {
            throw std::runtime_error("benchmark retry");
        }
    };

    const auto started = std::chrono::steady_clock::now();
    scheduler.submit(std::move(task));
    scheduler.wait_until_idle();
    const auto finished = std::chrono::steady_clock::now();
    scheduler.shutdown();
    return std::chrono::duration<double, std::milli>(finished - started).count();
}

} // namespace

int main() {
    constexpr std::size_t task_count = 5000;
    const std::vector<std::size_t> worker_counts{1, 2, 4, 8, 16};
    const auto output_directory = std::filesystem::path("benchmarks") / "results";
    std::filesystem::create_directories(output_directory);
    const auto output_path = output_directory / "scheduler_benchmark.csv";
    std::ofstream output(output_path);
    if (!output) {
        std::cerr << "Could not open benchmark output: " << output_path << '\n';
        return 1;
    }

    output << "benchmark,workers,tasks,elapsed_ms,tasks_per_second,maximum_parallelism\n";
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "benchmark,workers,tasks,elapsed_ms,tasks_per_second,maximum_parallelism\n";

    for (const auto worker_count : worker_counts) {
        const auto metrics = benchmark_parallel(worker_count, task_count);
        const auto tasks_per_second = task_count / (metrics.elapsed_ms / 1000.0);
        output << "parallel," << worker_count << ',' << task_count << ','
               << metrics.elapsed_ms << ',' << tasks_per_second << ','
               << metrics.maximum_parallelism << '\n';
        std::cout << "parallel," << worker_count << ',' << task_count << ','
                  << metrics.elapsed_ms << ',' << tasks_per_second << ','
                  << metrics.maximum_parallelism << '\n';
    }

    const auto dynamic_ms = benchmark_dynamic_submission(task_count);
    output << "dynamic,4," << task_count << ',' << dynamic_ms << ','
           << task_count / (dynamic_ms / 1000.0) << ",\n";
    std::cout << "dynamic,4," << task_count << ',' << dynamic_ms << ','
              << task_count / (dynamic_ms / 1000.0) << ",\n";

        const auto batch_ms = benchmark_batch_submission(task_count);
        output << "batch,16," << task_count << ',' << batch_ms << ','
            << task_count / (batch_ms / 1000.0) << ",\n";
        std::cout << "batch,16," << task_count << ',' << batch_ms << ','
            << task_count / (batch_ms / 1000.0) << ",\n";

    const auto retry_ms = benchmark_retry();
    output << "retry,2,1," << retry_ms << ",,\n";
    std::cout << "retry,2,1," << retry_ms << ",,\n";

    std::cout << "Results written to " << output_path << '\n';
}
