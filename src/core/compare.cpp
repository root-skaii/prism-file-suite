#include "core/compare.hpp"
#include "core/thread_priority.hpp"

#include "core/text.hpp"

#include <windows.h>

#include <algorithm>
#include <map>
#include <thread>

namespace prism::core {

namespace {

struct Seen {
    std::string   name;      // as spelled
    bool          is_dir{};
    std::uint64_t size{};
    std::uint64_t modified{};
};

[[nodiscard]] std::uint64_t pack(DWORD high, DWORD low) noexcept { return (static_cast<std::uint64_t>(high) << 32) | low; }

// the entries of one folder, keyed by lower-case name
std::map<std::string, Seen> list_dir(const std::wstring& dir, std::atomic<std::uint64_t>& seen)
{
    std::map<std::string, Seen> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = ::FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) { return out; }
    do {
        const wchar_t* n = fd.cFileName;
        if (n[0] == L'.' && (n[1] == L'\0' || (n[1] == L'.' && n[2] == L'\0'))) { continue; }
        Seen e;
        e.name     = text::narrow(n);
        e.is_dir   = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e.size     = e.is_dir ? 0 : pack(fd.nFileSizeHigh, fd.nFileSizeLow);
        e.modified = pack(fd.ftLastWriteTime.dwHighDateTime, fd.ftLastWriteTime.dwLowDateTime);
        out.emplace(text::lower_ascii(e.name), std::move(e));
        seen.fetch_add(1, std::memory_order_relaxed);
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
    return out;
}

constexpr std::uint64_t k_tolerance = 20000000ull; // 2 s: FAT stores times in two-second steps

struct Walk {
    std::atomic<std::uint64_t>&  seen;
    const std::atomic<bool>&     cancel;
    bool                         recursive;
    std::vector<DiffItem>&       out;

    void run(const std::wstring& left, const std::wstring& right, const std::string& prefix) const
    {
        const auto l = list_dir(left, seen);
        const auto r = list_dir(right, seen);
        auto li = l.begin();
        auto ri = r.begin();
        while ((li != l.end() || ri != r.end()) && !cancel.load(std::memory_order_relaxed)) {
            const bool take_left  = ri == r.end() || (li != l.end() && li->first <= ri->first);
            const bool take_right = li == l.end() || (ri != r.end() && ri->first <= li->first);
            DiffItem item;
            if (take_left && take_right) {
                const Seen& a = li->second;
                const Seen& b = ri->second;
                item.rel            = prefix + a.name;
                item.is_dir         = a.is_dir;
                item.left_size      = a.size;
                item.right_size     = b.size;
                item.left_modified  = a.modified;
                item.right_modified = b.modified;
                if (a.is_dir != b.is_dir) {
                    item.kind = DiffKind::type_differs;
                } else if (a.is_dir) {
                    item.kind = DiffKind::same; // (its contents decide, below)
                } else if (a.modified > b.modified + k_tolerance) {
                    item.kind = DiffKind::left_newer;
                } else if (b.modified > a.modified + k_tolerance) {
                    item.kind = DiffKind::right_newer;
                } else if (a.size != b.size) {
                    item.kind = DiffKind::size_differs;
                }
                out.push_back(item);
                if (a.is_dir && b.is_dir && recursive) {
                    run(left + L"\\" + text::widen(a.name), right + L"\\" + text::widen(b.name), item.rel + "\\");
                }
                ++li;
                ++ri;
            } else if (take_left) {
                const Seen& a = li->second;
                item.rel           = prefix + a.name;
                item.kind          = DiffKind::only_left;
                item.is_dir        = a.is_dir;
                item.left_size     = a.size;
                item.left_modified = a.modified;
                out.push_back(std::move(item));
                ++li;
            } else {
                const Seen& b = ri->second;
                item.rel            = prefix + b.name;
                item.kind           = DiffKind::only_right;
                item.is_dir         = b.is_dir;
                item.right_size     = b.size;
                item.right_modified = b.modified;
                out.push_back(std::move(item));
                ++ri;
            }
        }
    }
};

} // namespace

void FolderCompare::start(const std::string& left, const std::string& right, bool recursive, std::function<void()> wake)
{
    cancel();
    s_ = std::make_shared<State>();
    std::thread{[s = s_, left, right, recursive, wake = std::move(wake)] {
        lower_thread_priority();
        auto items = std::make_shared<std::vector<DiffItem>>();
        const auto trim = [](std::wstring w) { while (!w.empty() && w.back() == L'\\') { w.pop_back(); } return w; };
        Walk{s->seen, s->cancel, recursive, *items}.run(trim(text::widen(left)), trim(text::widen(right)), {});
        if (!s->cancel.load()) {
            std::lock_guard g{s->lock};
            s->items = std::move(items);
        }
        s->done.store(true);
        if (wake) { wake(); }
    }}.detach();
}

void FolderCompare::cancel() noexcept
{
    if (s_) { s_->cancel.store(true); }
    s_.reset();
}

std::shared_ptr<const std::vector<DiffItem>> FolderCompare::result() const
{
    if (!s_) { return nullptr; }
    std::lock_guard g{s_->lock};
    return s_->items;
}

} // namespace prism::core
