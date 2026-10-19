#include "core/file_index.hpp"

#include "core/text.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>

namespace prism::core {

namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] std::uint64_t pack(DWORD high, DWORD low) noexcept { return (static_cast<std::uint64_t>(high) << 32) | low; }

constexpr std::uint8_t k_dir = 1, k_hidden = 2, k_system = 4, k_readonly = 8;

} // namespace

FileIndex::~FileIndex()
{
    quit_ = true;
    ++crawl_gen_;
    cv_.notify_all();
    if (crawler_.joinable()) { crawler_.join(); }
    if (searcher_.joinable()) { searcher_.join(); }
}

std::shared_ptr<const FileIndex::Data> FileIndex::data() const
{
    std::lock_guard g{lock_};
    return data_;
}

std::uint64_t FileIndex::entries() const
{
    const auto d = data();
    return d ? d->nodes.size() : 0;
}

double FileIndex::age_seconds() const
{
    const auto d = data();
    if (!d) { return 1e12; }
    return std::chrono::duration<double>(Clock::now() - d->built).count();
}

void FileIndex::start(std::vector<std::string> roots)
{
    ++crawl_gen_;
    if (crawler_.joinable()) { crawler_.join(); } // (the running crawl looks at the generation for every folder)
    if (roots.empty()) { return; }
    building_ = true;
    scanned_  = 0;
    const std::uint64_t gen = crawl_gen_.load();
    crawler_ = std::thread([this, roots = std::move(roots), gen]() mutable { crawl(std::move(roots), gen); });
}

void FileIndex::crawl(std::vector<std::string> roots, std::uint64_t gen)
{
    ::SetThreadPriority(::GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN); // (low priority for cpu and disk: this is housekeeping)
    auto d = std::make_shared<Data>();
    d->nodes.reserve(1u << 20);
    d->pool.reserve(24u << 20);
    std::uint64_t last_wake = 0;

    for (const std::string& root : roots) {
        if (crawl_gen_.load() != gen) { break; }
        std::wstring rw = text::widen(root);
        while (!rw.empty() && rw.back() == L'\\') { rw.pop_back(); }
        if (rw.empty()) { continue; }
        Node rn;
        rn.parent   = npos;
        rn.name_off = static_cast<std::uint32_t>(d->pool.size());
        const std::string rname = text::narrow(rw) + "\\";
        d->pool += rname;
        rn.name_len = static_cast<std::uint16_t>(rname.size());
        rn.ext_off  = rn.name_len;
        rn.flags    = k_dir;
        rn.kind     = static_cast<std::uint8_t>(FileKind::folder);
        const auto root_id = static_cast<std::uint32_t>(d->nodes.size());
        d->nodes.push_back(rn);

        std::vector<std::pair<std::uint32_t, std::wstring>> stack;
        stack.emplace_back(root_id, std::move(rw));
        char utf8[1024];
        while (!stack.empty() && crawl_gen_.load() == gen) {
            auto [parent, dir] = std::move(stack.back());
            stack.pop_back();
            WIN32_FIND_DATAW fd;
            HANDLE h = ::FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
            if (h == INVALID_HANDLE_VALUE) { continue; }
            do {
                const wchar_t* n = fd.cFileName;
                if (n[0] == L'.' && (n[1] == L'\0' || (n[1] == L'.' && n[2] == L'\0'))) { continue; }
                const int len = ::WideCharToMultiByte(CP_UTF8, 0, n, -1, utf8, static_cast<int>(sizeof utf8), nullptr, nullptr) - 1;
                if (len <= 0 || len > 0xfff0) { continue; }
                const bool is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                Node node;
                node.parent   = parent;
                node.name_off = static_cast<std::uint32_t>(d->pool.size());
                node.name_len = static_cast<std::uint16_t>(len);
                d->pool.append(utf8, static_cast<std::size_t>(len));
                node.flags = static_cast<std::uint8_t>((is_dir ? k_dir : 0) | ((fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0 ? k_hidden : 0) |
                                                       ((fd.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) != 0 ? k_system : 0) |
                                                       ((fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0 ? k_readonly : 0));
                node.modified = pack(fd.ftLastWriteTime.dwHighDateTime, fd.ftLastWriteTime.dwLowDateTime);
                node.ext_off  = node.name_len;
                if (is_dir) {
                    node.kind = static_cast<std::uint8_t>(FileKind::folder);
                } else {
                    node.size = pack(fd.nFileSizeHigh, fd.nFileSizeLow);
                    const std::string_view name{utf8, static_cast<std::size_t>(len)};
                    const std::size_t dot = name.rfind('.');
                    if (dot != std::string_view::npos && dot != 0 && dot + 1 < name.size()) {
                        node.ext_off = static_cast<std::uint16_t>(dot + 1);
                        node.kind    = static_cast<std::uint8_t>(classify_extension(text::lower_ascii(name.substr(dot + 1))));
                    } else {
                        node.kind = static_cast<std::uint8_t>(FileKind::other);
                    }
                }
                const auto id = static_cast<std::uint32_t>(d->nodes.size());
                d->nodes.push_back(node);
                scanned_.fetch_add(1, std::memory_order_relaxed);
                if (is_dir && (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) { stack.emplace_back(id, dir + L"\\" + n); }
                if (d->nodes.size() >= 0xfffffff0u) { break; }
            } while (::FindNextFileW(h, &fd));
            ::FindClose(h);
            if (wake_ && scanned_.load() - last_wake >= 20000) {
                last_wake = scanned_.load();
                wake_();
            }
        }
    }

    if (crawl_gen_.load() == gen) {
        d->nodes.shrink_to_fit();
        d->pool.shrink_to_fit();
        d->built = Clock::now();
        {
            std::lock_guard g{lock_};
            data_ = std::move(d);
        }
        building_ = false;
        if (wake_) { wake_(); }
    }
}

void FileIndex::path_of(const Data& d, std::uint32_t id, std::string& out)
{
    thread_local std::vector<std::uint32_t> chain;
    chain.clear();
    for (; id != npos; id = d.nodes[id].parent) { chain.push_back(id); }
    out.clear();
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        const Node& n = d.nodes[*it];
        if (!out.empty() && out.back() != '\\') { out.push_back('\\'); }
        out.append(d.pool, n.name_off, n.name_len);
    }
}

void FileIndex::search(Query query)
{
    {
        std::lock_guard g{lock_};
        pending_ = std::move(query);
        ++asked_gen_;
        if (!searcher_.joinable()) { searcher_ = std::thread([this] { worker(); }); }
    }
    cv_.notify_all();
}

std::shared_ptr<const SearchResult> FileIndex::result() const
{
    std::lock_guard g{lock_};
    return result_;
}

void FileIndex::worker()
{
    for (;;) {
        Query          q;
        std::uint64_t  gen = 0;
        {
            std::unique_lock lk{lock_};
            cv_.wait(lk, [&] { return quit_.load() || pending_.has_value(); });
            if (quit_) { return; }
            q = std::move(*pending_);
            pending_.reset();
            gen = asked_gen_.load();
        }
        const auto began = Clock::now();
        auto res = std::make_shared<SearchResult>();
        res->text       = q.text();
        res->generation = gen;
        const auto d = data();
        bool superseded = false;
        if (d && !q.empty() && q.valid()) {
            const bool need_path = q.uses_path();
            FileEntry   e;
            std::string full;
            std::vector<std::uint32_t> ids;
            const std::size_t n = d->nodes.size();
            for (std::size_t i = 0; i < n; ++i) {
                if ((i & 0x1fff) == 0 && (asked_gen_.load() != gen || quit_.load())) { superseded = true; break; }
                const Node& node = d->nodes[i];
                if (need_path) {
                    path_of(*d, static_cast<std::uint32_t>(i), full);
                    e.name       = full;
                    e.ext_offset = static_cast<std::uint16_t>(std::min<std::size_t>(0xffff, full.size() - node.name_len + node.ext_off));
                } else {
                    e.name.assign(d->pool, node.name_off, node.name_len);
                    e.ext_offset = node.ext_off;
                }
                e.size       = node.size;
                e.modified   = node.modified;
                e.attributes = ((node.flags & k_dir) != 0 ? attr::directory : 0u) | ((node.flags & k_hidden) != 0 ? attr::hidden : 0u) |
                               ((node.flags & k_system) != 0 ? attr::system : 0u) | ((node.flags & k_readonly) != 0 ? attr::readonly : 0u);
                e.kind       = static_cast<FileKind>(node.kind);
                if (!q.matches(e)) { continue; }
                ++res->total;
                if (ids.size() < max_hits) { ids.push_back(static_cast<std::uint32_t>(i)); }
            }
            if (!superseded) {
                res->hits.reserve(ids.size());
                for (const std::uint32_t id : ids) {
                    IndexHit h;
                    path_of(*d, id, h.path);
                    h.is_dir   = (d->nodes[id].flags & k_dir) != 0;
                    h.size     = d->nodes[id].size;
                    h.modified = d->nodes[id].modified;
                    res->hits.push_back(std::move(h));
                }
                const auto leaf = [](const std::string& p) { const std::size_t s = p.rfind('\\'); return std::string_view{p}.substr(s == std::string::npos ? 0 : s + 1); };
                std::ranges::stable_sort(res->hits, [&](const IndexHit& a, const IndexHit& b) {
                    const int c = text::compare_natural(leaf(a.path), leaf(b.path));
                    return c != 0 ? c < 0 : a.path < b.path;
                });
            }
        }
        if (superseded || asked_gen_.load() != gen) { continue; } // (a newer query is waiting: this answer is already stale)
        res->milliseconds = std::chrono::duration<double, std::milli>(Clock::now() - began).count();
        {
            std::lock_guard g{lock_};
            result_ = std::move(res);
        }
        searched_gen_ = gen;
        if (wake_) { wake_(); }
    }
}

} // namespace prism::core
