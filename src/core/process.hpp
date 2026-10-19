#pragma once

// runs a console program without a window and collects its output (stdout and stderr together). blocking: only call it
// from worker threads

#include <atomic>
#include <string>

namespace prism::core {

struct ProcessResult {
    bool        started{};  // false: the program could not be launched (not installed ...)
    bool        timed_out{};
    unsigned    exit_code{};
    std::string output;
};

// `command_line` is the full line ("git status ..."), `cwd` may be empty. The program is killed after `timeout_ms`, or as
// soon as `quit` (when given) turns true
[[nodiscard]] ProcessResult run_process(const std::wstring& command_line, const std::wstring& cwd, unsigned timeout_ms, const std::atomic<bool>* quit = nullptr);

} // namespace prism::core
