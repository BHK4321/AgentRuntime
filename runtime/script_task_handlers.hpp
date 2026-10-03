#pragma once

#include "file_task_handlers.hpp"
#include <chrono>
#include <cstdlib>
#include <random>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace script_tasks {
#ifdef _WIN32
inline std::wstring widen(const std::string& value) {
    const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                          static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) throw std::runtime_error("invalid UTF-8 process argument");
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), size);
    return result;
}

inline std::wstring quote_windows_argument(const std::wstring& value) {
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const auto character : value) {
        if (character == L'\\') { ++slashes; continue; }
        if (character == L'"') {
            result.append(slashes * 2 + 1, L'\\');
            result.push_back(character);
        } else {
            result.append(slashes, L'\\');
            result.push_back(character);
        }
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}
#endif

inline void run_python(const std::filesystem::path& runner,
                       const std::filesystem::path& request,
                       const std::filesystem::path& job,
                       const TaskContext& context,
                       const std::chrono::steady_clock::time_point deadline) {
    const auto* configured = std::getenv("AGENTOS_PYTHON_EXECUTABLE");
#ifdef _WIN32
    const std::string executable = configured && *configured ? configured : "python";
    auto command = quote_windows_argument(widen(executable)) + L" -I " +
                   quote_windows_argument(runner.wstring()) + L" " +
                   quote_windows_argument(request.wstring()) + L" " +
                   quote_windows_argument(job.wstring());
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    HANDLE job_handle = CreateJobObjectW(nullptr, nullptr);
    if (!job_handle) throw std::runtime_error("could not create script process job");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job_handle, JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits))) {
        CloseHandle(job_handle);
        throw std::runtime_error("could not configure Python process cleanup");
    }
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP,
                        nullptr, job.parent_path().c_str(), &startup, &process)) {
        CloseHandle(job_handle);
        throw std::runtime_error("could not start Python; set AGENTOS_PYTHON_EXECUTABLE");
    }
    if (!AssignProcessToJobObject(job_handle, process.hProcess)) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 5000);
        CloseHandle(process.hThread); CloseHandle(process.hProcess); CloseHandle(job_handle);
        throw std::runtime_error("could not track Python child processes");
    }
    ResumeThread(process.hThread);
    CloseHandle(process.hThread);
    while (true) {
        const auto wait = WaitForSingleObject(process.hProcess, 100);
        if (wait == WAIT_OBJECT_0) break;
        if (wait == WAIT_FAILED) {
            TerminateJobObject(job_handle, 1);
            CloseHandle(process.hProcess); CloseHandle(job_handle);
            throw std::runtime_error("failed waiting for Python script runner");
        }
        if ((context.cancellation && context.cancellation->load()) ||
            std::chrono::steady_clock::now() >= deadline) {
            TerminateJobObject(job_handle, 1);
            WaitForSingleObject(process.hProcess, 5000);
            CloseHandle(process.hProcess); CloseHandle(job_handle);
            if (context.cancellation && context.cancellation->load()) return;
            throw std::runtime_error("Python script exceeded the 45-second runtime limit");
        }
    }
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hProcess); CloseHandle(job_handle);
    if (context.cancellation && context.cancellation->load()) return;
    (void)exit_code; // The runner writes its bounded failure report before exiting.
#else
    const std::string executable = configured && *configured ? configured : "python3";
    const auto pid = fork();
    if (pid < 0) throw std::runtime_error("could not start Python script runner");
    if (pid == 0) {
        setpgid(0, 0);
        if (chdir(job.c_str()) != 0) _exit(126);
        execlp(executable.c_str(), executable.c_str(), "-I", runner.c_str(),
               request.c_str(), job.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    setpgid(pid, pid);
    while (true) {
        int status = 0;
        const auto result = waitpid(pid, &status, WNOHANG);
        if (result == pid) {
            if (context.cancellation && context.cancellation->load()) return;
            return;
        }
        if (result < 0 && errno != EINTR) throw std::runtime_error("failed waiting for Python script runner");
        if ((context.cancellation && context.cancellation->load()) ||
            std::chrono::steady_clock::now() >= deadline) {
            kill(-pid, SIGKILL);
            waitpid(pid, nullptr, 0);
            if (context.cancellation && context.cancellation->load()) return;
            throw std::runtime_error("Python script exceeded the 45-second runtime limit");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
#endif
}

inline void register_handlers(TaskHandlerRegistry& handlers,
                              std::filesystem::path root = file_tasks::work_root()) {
    handlers.register_handler("python_script", [root](const std::string& payload, const TaskContext& context) {
        const auto output_relative = file_tasks::json_string_field(payload, "output_path");
        const auto output = file_tasks::confined_path(root, output_relative);
        const auto report = file_tasks::confined_path(root, output_relative + ".report.json");
        auto runner = std::filesystem::current_path() / "script_worker" / "direct_runner.py";
        if (const auto* configured = std::getenv("AGENTOS_SCRIPT_RUNNER"); configured && *configured)
            runner = configured;
        runner = std::filesystem::absolute(runner).lexically_normal();
        if (!std::filesystem::is_regular_file(runner))
            throw std::runtime_error("Python script runner missing: set AGENTOS_SCRIPT_RUNNER");

        std::random_device random;
        const auto job_id = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                            "-" + std::to_string(random()) + "-" + std::to_string(random());
        const auto job = file_tasks::confined_path(root, ".script-jobs/" + job_id);
        std::filesystem::create_directories(job);
        const auto request = job / "request.json";
        file_tasks::write_file(request, payload, context.task_id);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
        run_python(runner, request, job, context, deadline);
        if (context.cancellation && context.cancellation->load()) return;
        if (!std::filesystem::exists(job / "status"))
            throw std::runtime_error("Python script runner returned no result report");
        file_tasks::write_file(report, file_tasks::read_file(job / "report.json"), context.task_id);
        if (file_tasks::read_file(job / "status") != "ok")
            throw std::runtime_error("Python script failed; inspect its task result report");
        file_tasks::write_file(output, file_tasks::read_file(job / "result.txt"), context.task_id);
    });
}
} // namespace script_tasks
