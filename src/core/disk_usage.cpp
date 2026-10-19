#include "core/disk_usage.hpp"

#include "core/text.hpp"

#include <windows.h>

#include <algorithm>
#include <format>
#include <thread>

namespace prism::core {

namespace {

[[nodiscard]] std::uint64_t pack(DWORD high, DWORD low) noexcept { return (static_cast<std::uint64_t>(high) << 32) | low; }

struct Scan {
    std::atomic<std::uint64_t>& files;
    std::atomic<std::uint64_t>& bytes;
    const std::atomic<bool>&    cancel;
    const std::function<void()>& wake;
    mutable std::uint64_t       last_wake{};

    // the ui shows the running counts: poke it a few times a second
    void tick() const
    {
        if (!wake) { return; }
        const std::uint64_t now = ::GetTickCount64();
        if (now - last_wake >= 150) {
            last_wake = now;
            wake();
        }
    }

    // adds everything below `dir` to node. keep: the node holds children (folders and its biggest files), else only totals
    void walk(const std::wstring& dir, int depth, UsageNode& node, bool keep) const
    {
        WIN32_FIND_DATAW fd;
        HANDLE h = ::FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) { return; }
        std::vector<std::pair<std::wstring, std::uint64_t>> found; // files, only collected when children are kept
        std::vector<std::wstring>                           subdirs;
        do {
            const wchar_t* n = fd.cFileName;
            if (n[0] == L'.' && (n[1] == L'\0' || (n[1] == L'.' && n[2] == L'\0'))) { continue; }
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) { subdirs.emplace_back(n); } // (links: not followed)
            } else {
                const std::uint64_t sz = pack(fd.nFileSizeHigh, fd.nFileSizeLow);
                node.bytes += sz;
                node.files += 1;
                files.fetch_add(1, std::memory_order_relaxed);
                bytes.fetch_add(sz, std::memory_order_relaxed);
                if (keep) { found.emplace_back(n, sz); }
                if ((node.files & 0xff) == 0) { tick(); }
            }
        } while (::FindNextFileW(h, &fd) && !cancel.load(std::memory_order_relaxed));
        ::FindClose(h);

        if (keep) {
            std::ranges::sort(found, [](const auto& a, const auto& b) { return a.second > b.second; });
            std::uint64_t rest_bytes = 0, rest_files = 0;
            for (std::size_t i = 0; i < found.size(); ++i) {
                if (i < DiskUsage::keep_files && found[i].second > 0) {
                    UsageNode f;
                    f.name  = text::narrow(found[i].first);
                    f.bytes = found[i].second;
                    f.files = 1;
                    node.children.push_back(std::move(f));
                } else {
                    rest_bytes += found[i].second;
                    ++rest_files;
                }
            }
            if (rest_files > 0 && rest_bytes > 0) {
                UsageNode lump;
                lump.name   = std::format("{} smaller files", rest_files);
                lump.bytes  = rest_bytes;
                lump.files  = rest_files;
                lump.lumped = true;
                node.children.push_back(std::move(lump));
            }
        }
        for (const std::wstring& sub : subdirs) {
            if (cancel.load(std::memory_order_relaxed)) { return; }
            UsageNode child;
            child.is_dir = true;
            if (keep) { child.name = text::narrow(sub); }
            walk(dir + L"\\" + sub, depth + 1, child, keep && depth + 1 < DiskUsage::depth_limit);
            node.bytes += child.bytes;
            node.files += child.files;
            if (keep) { node.children.push_back(std::move(child)); }
        }
        if (keep) {
            std::ranges::sort(node.children, [](const UsageNode& a, const UsageNode& b) { return a.bytes > b.bytes; });
        }
    }
};

} // namespace

void DiskUsage::start(const std::string& root, std::function<void()> wake)
{
    cancel();
    s_ = std::make_shared<State>();
    std::thread{[s = s_, root, wake = std::move(wake)] {
        auto tree = std::make_shared<UsageNode>();
        tree->name   = root;
        tree->is_dir = true;
        std::wstring dir = text::widen(root);
        while (!dir.empty() && dir.back() == L'\\') { dir.pop_back(); } // (walk appends "\*": "C:\" becomes "C:")
        const Scan scan{s->files, s->bytes, s->cancel, wake};
        scan.walk(dir, 0, *tree, depth_limit > 0);
        if (!s->cancel.load()) {
            std::lock_guard g{s->lock};
            s->root = std::move(tree);
        }
        s->done.store(true);
        if (wake) { wake(); }
    }}.detach();
}

void DiskUsage::cancel() noexcept
{
    if (s_) { s_->cancel.store(true); }
    s_.reset();
}

std::shared_ptr<const UsageNode> DiskUsage::result() const
{
    if (!s_) { return nullptr; }
    std::lock_guard g{s_->lock};
    return s_->root;
}

} // namespace prism::core
