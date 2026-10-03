#include "script_task_handlers.hpp"
#include <iostream>

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("agentos-script-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TaskHandlerRegistry handlers;
    script_tasks::register_handlers(handlers, root);
    Task task;
    task.id = "test-script";
    task.type = "python_script";
    task.payload_json = R"({"input_path":"input.txt","output_path":"output/result.txt","code":"pass"})";
    auto work = handlers.resolve(task);
    std::filesystem::create_directories(root / ".script-jobs");
    auto simulate_worker = [&](bool succeed) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            for (const auto& item : std::filesystem::directory_iterator(root / ".script-jobs")) {
                if (!std::filesystem::exists(item.path() / "request.json")) continue;
                if (std::filesystem::exists(item.path() / "status") || std::filesystem::exists(item.path() / "cancel")) continue;
                file_tasks::write_file(item.path() / "result.txt", "processed", "test");
                file_tasks::write_file(item.path() / "report.json", succeed ? R"({"ok":true})" : R"({"ok":false,"error":"bad script"})", "test");
                file_tasks::write_file(item.path() / "status", succeed ? "ok" : "error", "test");
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    };
    std::thread worker(simulate_worker, true);
    TaskContext context;
    context.task_id = task.id;
    context.cancellation = std::make_shared<std::atomic_bool>(false);
    work(context);
    worker.join();
    if (file_tasks::read_file(root / "output/result.txt") != "processed") return 1;
    context.cancellation->store(true);
    work(context); // A cancelled task exits without waiting for a worker.
    context.cancellation->store(false);
    task.payload_json = R"({"input_path":"input.txt","output_path":"output/failed.txt","code":"bad code"})";
    std::thread failed_worker(simulate_worker, false);
    bool failed = false;
    try { handlers.resolve(task)(context); }
    catch (const std::runtime_error&) { failed = true; }
    failed_worker.join();
    if (!failed || std::filesystem::exists(root / "output/failed.txt") ||
        !std::filesystem::exists(root / "output/failed.txt.report.json")) return 1;
    std::filesystem::remove_all(root);
    std::cout << "Script bridge passed\n";
}
