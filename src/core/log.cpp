#include "core/log.hpp"

#include "core/text.hpp"

#include <windows.h>

#include <atomic>
#include <mutex>

namespace prism::core::log {

namespace {

std::atomic<bool> g_on{false};
std::mutex        g_lock;
HANDLE            g_file = INVALID_HANDLE_VALUE;
ULONGLONG         g_start = ::GetTickCount64();
constexpr ULONGLONG max_bytes = 2ull * 1024 * 1024;

void put(std::string_view s)
{
    DWORD wrote = 0;
    ::WriteFile(g_file, s.data(), static_cast<DWORD>(s.size()), &wrote, nullptr);
}

} // namespace

void enable(const std::string& path)
{
    std::lock_guard g{g_lock};
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
}

void disable()
{
    std::lock_guard g{g_lock};
    g_on.store(false);
    if (g_file != INVALID_HANDLE_VALUE) {
        ::CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
}

bool enabled() noexcept { return g_on.load(std::memory_order_relaxed); }

void write(std::string_view message)
{
    if (!enabled()) { return; }
    SYSTEMTIME st;
    ::GetLocalTime(&st);
    const std::string line = std::format("{:02}:{:02}:{:02}.{:03} +{:>6}ms [{:>5}] {}\r\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                                         ::GetTickCount64() - g_start, ::GetCurrentThreadId(), message);
    std::lock_guard g{g_lock};
    if (g_file != INVALID_HANDLE_VALUE) { put(line); }
}

} // namespace prism::core::log
