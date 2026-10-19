#include "core/folder_sizes.hpp"

#include "core/path.hpp"
#include "core/text.hpp"

#include <windows.h>

#include <vector>

namespace prism::core {

unsigned long long measure_folder(const std::string& dir, const std::stop_token& stop)
{
    unsigned long long      total = 0;
    std::vector<std::wstring> todo{text::widen(dir)};
    while (!todo.empty() && !stop.stop_requested()) {
        const std::wstring cur = std::move(todo.back());
        todo.pop_back();
        WIN32_FIND_DATAW fd{};
        const HANDLE     h = ::FindFirstFileExW((cur + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) { continue; }
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == 0 || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0))) { continue; }
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { continue; } // junctions would count things twice
                todo.push_back(cur + L"\\" + fd.cFileName);
            } else {
                total += (static_cast<unsigned long long>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            }
        } while (!stop.stop_requested() && ::FindNextFileW(h, &fd));
        ::FindClose(h);
    }
    return total;
}

FolderSizes::FolderSizes(std::function<void()> wake) : wake_{std::move(wake)}, thread_{[this](std::stop_token st) { run(st); }} {}

FolderSizes::~FolderSizes()
{
    thread_.request_stop();
    cv_.notify_all();
}

std::optional<unsigned long long> FolderSizes::lookup(const std::string& path)
{
    const std::lock_guard lock{mutex_};
    Entry& e = entries_[path];
    const bool stale = e.have && std::chrono::steady_clock::now() - e.at > max_age;
    if ((!e.have || stale) && !e.queued && queue_.size() < 256) {
        e.queued = true;
        queue_.push_back(path);
        cv_.notify_one();
    }
    if (e.have) { return e.bytes; }
    return std::nullopt;
}

void FolderSizes::run(std::stop_token st)
{
    while (!st.stop_requested()) {
        std::string path;
        {
            std::unique_lock lock{mutex_};
            if (!cv_.wait(lock, st, [this] { return !queue_.empty(); })) { return; }
            path = std::move(queue_.front());
            queue_.pop_front();
        }
        const unsigned long long bytes = measure_folder(path, st);
        if (st.stop_requested()) { return; }
        {
            const std::lock_guard lock{mutex_};
            Entry& e = entries_[path];
            e.bytes  = bytes;
            e.have   = true;
            e.queued = false;
            e.at     = std::chrono::steady_clock::now();
        }
        if (wake_) { wake_(); }
    }
}

} // namespace prism::core
