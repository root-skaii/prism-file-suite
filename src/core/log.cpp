#include "core/log.hpp"

#include "core/text.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fcntl.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#endif

#include <atomic>
#include <mutex>

namespace prism::core::log {

namespace {

std::atomic<bool> g_on{false};
std::mutex        g_lock;
#ifdef _WIN32
HANDLE            g_file = INVALID_HANDLE_VALUE;
ULONGLONG         g_start = ::GetTickCount64();
constexpr ULONGLONG max_bytes = 2ull * 1024 * 1024;

void put(std::string_view s)
{
    DWORD wrote = 0;
    ::WriteFile(g_file, s.data(), static_cast<DWORD>(s.size()), &wrote, nullptr);
}
#else
int g_fd = -1;
const auto g_start = std::chrono::steady_clock::now();
constexpr std::uint64_t max_bytes = 2ull * 1024 * 1024;

void put(std::string_view s)
{
    if (g_fd >= 0) { (void)::write(g_fd, s.data(), s.size()); }
}
#endif

} // namespace

void enable(const std::string& path)
{
    std::lock_guard g{g_lock};
#ifdef _WIN32
    if (g_file != INVALID_HANDLE_VALUE) { return; }
    const std::wstring w = text::widen(path);
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (::GetFileAttributesExW(w.c_str(), GetFileExInfoStandard, &a) != FALSE && ((static_cast<ULONGLONG>(a.nFileSizeHigh) << 32) | a.nFileSizeLow) > max_bytes) {
        ::MoveFileExW(w.c_str(), (w + L".old").c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    g_file = ::CreateFileW(w.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_file == INVALID_HANDLE_VALUE) { return; }
    g_on.store(true);
    put("\r\n");
#else
    if (g_fd >= 0) { return; }
    struct stat st{};
    if (::stat(path.c_str(), &st) == 0 && static_cast<std::uint64_t>(st.st_size) > max_bytes) {
        (void)::rename(path.c_str(), (path + ".old").c_str());
    }
    g_fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (g_fd < 0) { return; }
    g_on.store(true);
    put("\n");
#endif
}

void disable()
{
    std::lock_guard g{g_lock};
    g_on.store(false);
#ifdef _WIN32
    if (g_file != INVALID_HANDLE_VALUE) {
        ::CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
#else
    if (g_fd >= 0) {
        ::close(g_fd);
        g_fd = -1;
    }
#endif
}

bool enabled() noexcept { return g_on.load(std::memory_order_relaxed); }

void write(std::string_view message)
{
    if (!enabled()) { return; }
#ifdef _WIN32
    SYSTEMTIME st;
    ::GetLocalTime(&st);
    const std::string line = std::format("{:02}:{:02}:{:02}.{:03} +{:>6}ms [{:>5}] {}\r\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                                         ::GetTickCount64() - g_start, ::GetCurrentThreadId(), message);
    std::lock_guard g{g_lock};
    if (g_file != INVALID_HANDLE_VALUE) { put(line); }
#else
    const auto now = std::chrono::system_clock::now();
    const std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    ::localtime_r(&tt, &tm);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_start).count();
    const auto tid = std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xffffu;
    const std::string line = std::format("{:02}:{:02}:{:02}.{:03} +{:>6}ms [{:>5}] {}\n", tm.tm_hour, tm.tm_min, tm.tm_sec, ms, elapsed, tid, message);
    std::lock_guard g{g_lock};
    if (g_fd >= 0) { put(line); }
#endif
}

} // namespace prism::core::log
