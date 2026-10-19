#include "core/empty_folders.hpp"
#include "core/thread_priority.hpp"

#include "core/path.hpp"
#include "core/text.hpp"

#include <windows.h>

#include <algorithm>
#include <thread>

namespace prism::core {

namespace {

// true when `dir` holds nothing but empty folders. the empty ones that sit inside a folder that is not empty go to `out`
bool walk(const std::wstring& dir, std::vector<std::wstring>& out, std::atomic<std::uint64_t>& seen, const std::atomic<bool>& cancel)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = ::FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) { return false; } // (cannot look inside: not known to be empty)
    bool has_file = false;
    std::vector<std::wstring> subdirs;
    do {
        const wchar_t* n = fd.cFileName;
        if (n[0] == L'.' && (n[1] == L'\0' || (n[1] == L'.' && n[2] == L'\0'))) { continue; }
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 && (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
            subdirs.emplace_back(n);
        } else {
            has_file = true; // (a file, or a link: either way something is there)
        }
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
    seen.fetch_add(1, std::memory_order_relaxed);

    std::vector<std::wstring> empties;
    bool all_empty = !has_file;
    for (const std::wstring& s : subdirs) {
        if (cancel.load(std::memory_order_relaxed)) { return false; }
        const std::wstring full = dir + L"\\" + s;
        if (walk(full, empties, seen, cancel)) { empties.push_back(full); } else { all_empty = false; }
    }
    if (all_empty) { return true; } // (the whole tree below is empty: the caller lists this folder, not what is in it)
    out.insert(out.end(), empties.begin(), empties.end());
    return false;
}

} // namespace

void EmptyFolderScan::start(const std::string& root, std::function<void()> wake)
{
    cancel();
    s_ = std::make_shared<State>();
    std::thread{[s = s_, root, wake = std::move(wake)] {
        lower_thread_priority();
        std::wstring dir = text::widen(root);
        while (!dir.empty() && dir.back() == L'\\') { dir.pop_back(); }
        std::vector<std::wstring> out;
        // (a folder inside an empty tree is dropped by walk(): what is listed are the outermost empty folders.
        // when the root itself is empty there is nothing inside to list)
        if (walk(dir, out, s->seen, s->cancel)) { out.clear(); }
        if (!s->cancel.load()) {
            std::lock_guard g{s->lock};
            for (const std::wstring& w : out) { s->found.push_back(text::narrow(w)); }
            std::ranges::sort(s->found);
        }
        s->done.store(true);
        if (wake) { wake(); }
    }}.detach();
}

void EmptyFolderScan::cancel() noexcept
{
    if (s_) { s_->cancel.store(true); }
    s_.reset();
}

std::vector<std::string> EmptyFolderScan::result() const
{
    if (!s_) { return {}; }
    std::lock_guard g{s_->lock};
    return s_->found;
}

} // namespace prism::core
