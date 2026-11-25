#include "core/file_index.hpp"
#include "core/thread_priority.hpp"

#include "core/log.hpp"
#include "core/path.hpp"
#include "core/text.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>

namespace prism::core {

namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] std::uint64_t pack(DWORD high, DWORD low) noexcept { return (static_cast<std::uint64_t>(high) << 32) | low; }
[[nodiscard]] std::int64_t  unix_now() noexcept
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

constexpr std::uint8_t  k_dir = 1, k_hidden = 2, k_system = 4, k_readonly = 8;
constexpr std::uint32_t k_cache_magic = 0x31584950u; // "PIX1"

struct CacheHeader {
    std::uint32_t magic{};
    std::uint32_t node_size{};
    std::int64_t  built_unix{};
    std::uint64_t node_count{};
    std::uint64_t pool_size{};
};

bool write_all(HANDLE f, const void* ptr, std::size_t bytes)
{
    const auto* p = static_cast<const unsigned char*>(ptr);
    while (bytes > 0) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(bytes, 64u << 20));
        DWORD put = 0;
        if (!::WriteFile(f, p, chunk, &put, nullptr) || put != chunk) { return false; }
        p += put;
        bytes -= put;
    }
    return true;
}

bool read_all(HANDLE f, void* ptr, std::size_t bytes)
{
    auto* p = static_cast<unsigned char*>(ptr);
    while (bytes > 0) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(bytes, 64u << 20));
        DWORD got = 0;
        if (!::ReadFile(f, p, chunk, &got, nullptr) || got != chunk) { return false; }
        p += got;
        bytes -= got;
    }
    return true;
}

} // namespace

FileIndex::~FileIndex()
{
    quit_ = true;
    ++crawl_gen_;
    crawl_cv_.notify_all();
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
    if (d->built_unix > 0) {
        const std::int64_t dt = unix_now() - d->built_unix;
        if (dt >= 0) { return static_cast<double>(dt); }
    }
    return std::chrono::duration<double>(Clock::now() - d->built).count();
}

void FileIndex::set_cache_path(std::string path)
{
    std::lock_guard g{lock_};
    cache_path_ = std::move(path);
}

bool FileIndex::write_data(const Data& d, const std::string& path)
{
    if (path.empty() || d.nodes.empty()) { return false; }
    const std::wstring wpath = text::widen(path);
    if (const std::size_t sep = wpath.find_last_of(L'\\'); sep != std::wstring::npos) {
        ::CreateDirectoryW(wpath.substr(0, sep).c_str(), nullptr);
    }
    const std::wstring tmp = wpath + L".tmp";
    HANDLE f = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { return false; }
    CacheHeader h{k_cache_magic, static_cast<std::uint32_t>(sizeof(Node)), d.built_unix, d.nodes.size(), d.pool.size()};
    const bool ok = write_all(f, &h, sizeof h) &&
                    write_all(f, d.nodes.data(), d.nodes.size() * sizeof(Node)) &&
                    write_all(f, d.pool.data(), d.pool.size());
    ::CloseHandle(f);
    if (!ok || !::MoveFileExW(tmp.c_str(), wpath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        ::DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

std::shared_ptr<FileIndex::Data> FileIndex::read_data(const std::string& path)
{
    if (path.empty()) { return nullptr; }
    HANDLE f = ::CreateFileW(text::widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) { return nullptr; }
    LARGE_INTEGER fsize{};
    CacheHeader   h{};
    std::shared_ptr<Data> out;
    if (::GetFileSizeEx(f, &fsize) && fsize.QuadPart >= static_cast<LONGLONG>(sizeof h) && read_all(f, &h, sizeof h) &&
        h.magic == k_cache_magic && h.node_size == sizeof(Node) && h.node_count > 0 && h.node_count < (1ull << 26) &&
        h.pool_size > 0 && h.pool_size < (2ull << 30)) {
        const std::uint64_t expected = sizeof(CacheHeader) + h.node_count * sizeof(Node) + h.pool_size;
        if (static_cast<std::uint64_t>(fsize.QuadPart) == expected) {
            auto d = std::make_shared<Data>();
            d->nodes.resize(static_cast<std::size_t>(h.node_count));
            d->pool.resize(static_cast<std::size_t>(h.pool_size));
            if (read_all(f, d->nodes.data(), d->nodes.size() * sizeof(Node)) && read_all(f, d->pool.data(), d->pool.size())) {
                // validate parent and pool bounds so a corrupted cache can never crash search
                bool valid = true;
                for (const Node& n : d->nodes) {
                    if ((n.parent != npos && n.parent >= d->nodes.size()) ||
                        static_cast<std::uint64_t>(n.name_off) + n.name_len > d->pool.size()) {
                        valid = false;
                        break;
                    }
                }
                if (valid) {
                    d->built      = Clock::now();
                    d->built_unix = h.built_unix;
                    out           = std::move(d);
                }
            }
        }
    }
    ::CloseHandle(f);
    return out;
}

bool FileIndex::save_to(const std::string& path) const
{
    const auto d = data();
    return d != nullptr && write_data(*d, path);
}

bool FileIndex::load_from(const std::string& path)
{
    auto d = read_data(path);
    if (!d) { return false; }
    {
        std::lock_guard g{lock_};
        data_ = std::move(d);
    }
    return true;
}

void FileIndex::start(std::vector<std::string> roots, bool force)
{
    ++crawl_gen_;
    if (roots.empty()) {
        building_ = false;
        return;
    }
    building_ = true;
    scanned_  = 0;
    {
        std::lock_guard g{lock_};
        crawl_pending_ = CrawlReq{std::move(roots), force};
        if (!crawler_.joinable()) { crawler_ = std::thread([this] { crawler_loop(); }); }
    }
    crawl_cv_.notify_one();
}

void FileIndex::crawler_loop()
{
    ::SetThreadPriority(::GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN); // (low priority for cpu and disk: this is housekeeping)
    for (;;) {
        CrawlReq      req;
        std::uint64_t gen = 0;
        {
            std::unique_lock lk{lock_};
            crawl_cv_.wait(lk, [&] { return quit_.load() || crawl_pending_.has_value(); });
            if (quit_) { return; }
            req = std::move(*crawl_pending_);
            crawl_pending_.reset();
            gen = crawl_gen_.load();
        }
        crawl(std::move(req.roots), gen, req.force);
    }
}

void FileIndex::crawl(std::vector<std::string> roots, std::uint64_t gen, bool force)
{
    std::string cache;
    bool        have_data = false;
    {
        std::lock_guard g{lock_};
        cache     = cache_path_;
        have_data = data_ != nullptr;
    }
    // warm start: load the last snapshot from disk immediately so Ctrl+Shift+F works right away
    if (!have_data && !cache.empty()) {
        if (auto cached = read_data(cache)) {
            const std::int64_t age = cached->built_unix > 0 ? (unix_now() - cached->built_unix) : 1000000;
            const std::size_t  cnt = cached->nodes.size();
            {
                std::lock_guard g{lock_};
                if (!data_) { data_ = std::move(cached); }
            }
            log::info("drive index loaded from cache: {} entries ({}s old)", cnt, age);
            if (!force && age >= 0 && age < 600) {
                building_ = false;
                if (wake_) { wake_(); }
                return;
            }
            if (wake_) { wake_(); }
        }
    }

    log::Timer crawl_time{"drive index crawl"};
    auto d = std::make_shared<Data>();
    std::atomic<std::uint64_t> last_wake{0};

    const auto crawl_one = [&](const std::string& root, Data& out) {
        std::wstring rw = text::widen(root);
        while (!rw.empty() && rw.back() == L'\\') { rw.pop_back(); }
        if (rw.empty()) { return; }
        out.nodes.reserve(1u << 18);
        out.pool.reserve(6u << 20);
        Node rn;
        rn.parent   = npos;
        rn.name_off = static_cast<std::uint32_t>(out.pool.size());
        const std::string rname = text::narrow(rw) + "\\";
        out.pool += rname;
        rn.name_len = static_cast<std::uint16_t>(rname.size());
        rn.ext_off  = rn.name_len;
        rn.flags    = k_dir;
        rn.kind     = static_cast<std::uint8_t>(FileKind::folder);
        const auto root_id = static_cast<std::uint32_t>(out.nodes.size());
        out.nodes.push_back(rn);

        std::vector<std::pair<std::uint32_t, std::wstring>> stack;
        stack.emplace_back(root_id, std::move(rw));
        char utf8[1024];
        while (!stack.empty() && crawl_gen_.load(std::memory_order_relaxed) == gen) {
            auto [parent, dir] = std::move(stack.back());
            stack.pop_back();
            WIN32_FIND_DATAW fd;
            std::wstring pat = dir + L"\\*";
            if (pat.size() >= 248 && !pat.starts_with(L"\\\\?\\")) {
                pat = pat.starts_with(L"\\\\") ? (L"\\\\?\\UNC\\" + pat.substr(2)) : (L"\\\\?\\" + pat);
            }
            HANDLE h = ::FindFirstFileExW(pat.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
            if (h == INVALID_HANDLE_VALUE) { continue; }
            do {
                const wchar_t* n = fd.cFileName;
                if (n[0] == L'.' && (n[1] == L'\0' || (n[1] == L'.' && n[2] == L'\0'))) { continue; }
                const int len = ::WideCharToMultiByte(CP_UTF8, 0, n, -1, utf8, static_cast<int>(sizeof utf8), nullptr, nullptr) - 1;
                if (len <= 0 || len > 0xfff0) { continue; }
                const bool is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                Node node;
                node.parent   = parent;
                node.name_off = static_cast<std::uint32_t>(out.pool.size());
                node.name_len = static_cast<std::uint16_t>(len);
                out.pool.append(utf8, static_cast<std::size_t>(len));
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
                const auto id = static_cast<std::uint32_t>(out.nodes.size());
                out.nodes.push_back(node);
                const std::uint64_t total = scanned_.fetch_add(1, std::memory_order_relaxed) + 1;
                const bool is_link = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 && path::is_link_reparse_tag(fd.dwReserved0);
                if (is_dir && !is_link) { stack.emplace_back(id, dir + L"\\" + n); }
                if (out.nodes.size() >= 0xfffffff0u) { break; }
                if (wake_ && (total & 0x3fff) == 0) {
                    std::uint64_t prev = last_wake.load(std::memory_order_relaxed);
                    if (total - prev >= 20000 && last_wake.compare_exchange_strong(prev, total, std::memory_order_relaxed)) { wake_(); }
                }
            } while (::FindNextFileW(h, &fd));
            ::FindClose(h);
        }
    };

    if (roots.size() == 1) {
        crawl_one(roots.front(), *d);
    } else {
        // crawl fixed drives in parallel (each drive has its own spindle / NVMe queue), then merge into one table
        std::vector<Data>        parts(roots.size());
        std::vector<std::thread> threads;
        threads.reserve(roots.size() - 1);
        for (std::size_t r = 1; r < roots.size(); ++r) {
            threads.emplace_back([&, r] {
                ::SetThreadPriority(::GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
                crawl_one(roots[r], parts[r]);
            });
        }
        crawl_one(roots[0], parts[0]);
        for (std::thread& t : threads) { t.join(); }
        if (crawl_gen_.load() == gen) {
            std::size_t total_nodes = 0, total_pool = 0;
            for (const Data& p : parts) { total_nodes += p.nodes.size(); total_pool += p.pool.size(); }
            d->nodes.reserve(total_nodes);
            d->pool.reserve(total_pool);
            for (Data& p : parts) {
                const auto base_node = static_cast<std::uint32_t>(d->nodes.size());
                const auto base_pool = static_cast<std::uint32_t>(d->pool.size());
                d->pool.append(p.pool);
                for (Node& n : p.nodes) {
                    if (n.parent != npos) { n.parent += base_node; }
                    n.name_off += base_pool;
                    d->nodes.push_back(n);
                }
            }
        }
    }

    if (crawl_gen_.load() == gen) {
        d->nodes.shrink_to_fit();
        d->pool.shrink_to_fit();
        d->built      = Clock::now();
        d->built_unix = unix_now();
        {
            std::lock_guard g{lock_};
            data_ = d;
        }
        building_ = false;
        log::info("drive index ready: {} entries, {} roots", d->nodes.size(), roots.size());
        if (wake_) { wake_(); }
        if (!cache.empty()) { (void)write_data(*d, cache); }
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
    lower_thread_priority();
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
            const std::size_t n  = d->nodes.size();
            const auto scan_range = [&](std::size_t from, std::size_t to, std::vector<std::uint32_t>& out_ids, std::size_t& out_total, bool& out_superseded) {
                std::string full;
                for (std::size_t i = from; i < to; ++i) {
                    if (((i - from) & 0x1fff) == 0 && (asked_gen_.load(std::memory_order_relaxed) != gen || quit_.load(std::memory_order_relaxed))) {
                        out_superseded = true;
                        return;
                    }
                    const Node& node = d->nodes[i];
                    FileEntryView v;
                    if (need_path) {
                        path_of(*d, static_cast<std::uint32_t>(i), full);
                        v.name    = full;
                        v.display = full;
                        const std::size_t ext_pos = full.size() - node.name_len + node.ext_off;
                        v.ext     = ext_pos <= full.size() ? std::string_view{full}.substr(ext_pos) : std::string_view{};
                    } else {
                        const std::string_view nm{d->pool.data() + node.name_off, node.name_len};
                        v.name    = nm;
                        v.display = nm;
                        v.ext     = node.ext_off <= nm.size() ? nm.substr(node.ext_off) : std::string_view{};
                    }
                    v.size       = node.size;
                    v.modified   = node.modified;
                    v.attributes = ((node.flags & k_dir) != 0 ? attr::directory : 0u) | ((node.flags & k_hidden) != 0 ? attr::hidden : 0u) |
                                   ((node.flags & k_system) != 0 ? attr::system : 0u) | ((node.flags & k_readonly) != 0 ? attr::readonly : 0u);
                    v.kind       = static_cast<FileKind>(node.kind);
                    if (!q.matches(v)) { continue; }
                    ++out_total;
                    if (out_ids.size() < max_hits) { out_ids.push_back(static_cast<std::uint32_t>(i)); }
                }
            };

            std::vector<std::uint32_t> ids;
            const unsigned threads = n >= 200000 ? std::clamp(std::thread::hardware_concurrency(), 1u, 4u) : 1u;
            if (threads <= 1) {
                scan_range(0, n, ids, res->total, superseded);
            } else {
                struct Slice {
                    std::vector<std::uint32_t> ids;
                    std::size_t                total{};
                    bool                       superseded{};
                };
                std::vector<Slice>       slices(threads);
                std::vector<std::thread> helpers;
                helpers.reserve(threads - 1);
                const std::size_t chunk = (n + threads - 1) / threads;
                for (unsigned t = 1; t < threads; ++t) {
                    const std::size_t a = static_cast<std::size_t>(t) * chunk;
                    const std::size_t b = std::min(n, a + chunk);
                    if (a >= b) { continue; }
                    helpers.emplace_back([&, t, a, b] {
                        lower_thread_priority();
                        scan_range(a, b, slices[t].ids, slices[t].total, slices[t].superseded);
                    });
                }
                scan_range(0, std::min(n, chunk), slices[0].ids, slices[0].total, slices[0].superseded);
                for (std::thread& h : helpers) { h.join(); }
                for (const Slice& s : slices) {
                    superseded |= s.superseded;
                    res->total += s.total;
                    if (!superseded && ids.size() < max_hits) {
                        const std::size_t take = std::min(s.ids.size(), max_hits - ids.size());
                        ids.insert(ids.end(), s.ids.begin(), s.ids.begin() + static_cast<std::ptrdiff_t>(take));
                    }
                }
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
