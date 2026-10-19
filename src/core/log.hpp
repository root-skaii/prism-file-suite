#pragma once

// a plain-text troubleshooting log (Settings > Diagnostics). off: every call costs one relaxed atomic load.
// lines: wall time, ms since start, thread, message. appended; rotated to .old past 2 MB. safe from any thread

#include <chrono>
#include <format>
#include <string>
#include <string_view>

namespace prism::core::log {

void enable(const std::string& path); // opens for appending; false-safe (a log that cannot open stays off)
void disable();
[[nodiscard]] bool enabled() noexcept;
void write(std::string_view message);

template <class... Args>
void info(std::format_string<Args...> fmt, Args&&... args)
{
    if (enabled()) { write(std::format(fmt, std::forward<Args>(args)...)); }
}

// logs "what: N ms" when it ends, if it took at least min_ms (0: always)
class Timer {
public:
    explicit Timer(std::string what, double min_ms = 0.0) : what_{std::move(what)}, min_ms_{min_ms}, t0_{std::chrono::steady_clock::now()} {}
    ~Timer() { done(); }
    Timer(const Timer&)            = delete;
    Timer& operator=(const Timer&) = delete;
    [[nodiscard]] double ms() const { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0_).count(); }
    void done()
    {
        if (finished_) { return; }
        finished_ = true;
        if (const double t = ms(); enabled() && t >= min_ms_) { write(std::format("{}: {:.1f} ms", what_, t)); }
    }

private:
    std::string                           what_;
    double                                min_ms_;
    std::chrono::steady_clock::time_point t0_;
    bool                                  finished_{};
};

} // namespace prism::core::log
