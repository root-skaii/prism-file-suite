#include "core/transfer.hpp"

#include "core/path.hpp"
#include "core/recycle.hpp"
#include "core/text.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <format>

namespace prism::core {

#ifdef _WIN32

namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] std::uint64_t pack(DWORD high, DWORD low) noexcept { return (static_cast<std::uint64_t>(high) << 32) | low; }

[[nodiscard]] std::wstring lp(const std::string& utf8) { return path::long_wide(utf8); }

[[nodiscard]] bool same_text(std::string_view a, std::string_view b)
{
    return text::lower_ascii(a) == text::lower_ascii(b);
}

// `path` is `folder` or below it
[[nodiscard]] bool is_inside(const std::string& path, const std::string& folder)
{
    const std::string p = text::lower_ascii(path::normalize(path));
    const std::string f = text::lower_ascii(path::normalize(folder));
    return p == f || (p.size() > f.size() && p.starts_with(f) && (p[f.size()] == '\\' || f.back() == '\\'));
}

[[nodiscard]] bool same_volume(const std::string& a, const std::string& b)
{
    return path::same_volume(a, b);
}

struct Attr {
    bool          exists{};
    bool          dir{};
    bool          reparse{}; // symlink or junction (NOT cloud placeholders)
    DWORD         raw_attrs{};
    FILETIME      ft_created{}, ft_accessed{}, ft_modified{};
    std::uint64_t size{};
    std::uint64_t modified{};
};

[[nodiscard]] Attr attr_of(const std::string& p)
{
    Attr a;
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!::GetFileAttributesExW(lp(p).c_str(), GetFileExInfoStandard, &d)) { return a; }
    a.exists      = true;
    a.raw_attrs   = d.dwFileAttributes;
    a.dir         = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    a.reparse     = (d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 && path::is_reparse_link(p);
    a.ft_created  = d.ftCreationTime;
    a.ft_accessed = d.ftLastAccessTime;
    a.ft_modified = d.ftLastWriteTime;
    a.size        = a.dir ? 0 : pack(d.nFileSizeHigh, d.nFileSizeLow);
    a.modified    = pack(d.ftLastWriteTime.dwHighDateTime, d.ftLastWriteTime.dwLowDateTime);
    return a;
}

// an item dragged out of the Recycle Bin is the bin's own copy (`$R4ND0M.png`): the name it had is in the `$I` record beside it.
// `info` is that record (deleted once the item has been moved out), both empty for anything else
struct RecycledName {
    std::string name;
    std::string info;
};

[[nodiscard]] RecycledName recycled_name(const std::string& src)
{
    const std::optional<recycle::Record> rec = recycle::read(src);
    if (!rec) { return {}; }
    std::string name = path::leaf(rec->original);
    if (name.empty()) { return {}; }
    return {std::move(name), rec->info};
}

// "name (2).ext" in `dir`
[[nodiscard]] std::string numbered(const std::string& dir, const std::string& name, bool is_dir)
{
    const std::size_t dot = is_dir ? std::string::npos : name.rfind('.');
    const std::string stem = dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
    const std::string ext  = dot == std::string::npos || dot == 0 ? std::string{} : name.substr(dot);
    for (int i = 2; i < 10000; ++i) {
        const std::string candidate = std::format("{} ({}){}", stem, i, ext);
        if (!attr_of(path::join(dir, candidate)).exists) { return candidate; }
    }
    return std::format("{} ({}){}", stem, ::GetTickCount64(), ext);
}

// "name - Copy.ext", "name - Copy (2).ext": a copy next to its original
[[nodiscard]] std::string copy_name(const std::string& dir, const std::string& name, bool is_dir)
{
    const std::size_t dot = is_dir ? std::string::npos : name.rfind('.');
    const std::string stem = dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
    const std::string ext  = dot == std::string::npos || dot == 0 ? std::string{} : name.substr(dot);
    for (int i = 1; i < 10000; ++i) {
        const std::string candidate = (i == 1 ? stem + " - Copy" : std::format("{} - Copy ({})", stem, i)) + ext;
        if (!attr_of(path::join(dir, candidate)).exists) { return candidate; }
    }
    return std::format("{} - Copy ({}){}", stem, ::GetTickCount64(), ext);
}

struct Item {
    std::string   src, dst;
    bool          dir{};
    bool          link{};      // a symbolic link to a file: copied as the link
    bool          same_vol{};  // the destination is on the source's volume (a move is a rename)
    std::uint64_t size{};
    std::size_t   top{};       // which source it belongs to
};

// the folder's children, with sizes
struct Child {
    std::string name;
    bool        dir{}, reparse{};
    std::uint64_t size{};
};

std::vector<Child> list_children(const std::string& dir)
{
    std::vector<Child> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = ::FindFirstFileExW((lp(dir) + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) { return out; }
    do {
        const wchar_t* n = fd.cFileName;
        if (n[0] == L'.' && (n[1] == L'\0' || (n[1] == L'.' && n[2] == L'\0'))) { continue; }
        Child c;
        c.name    = text::narrow(n);
        c.dir     = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        c.reparse = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 && path::is_link_reparse_tag(fd.dwReserved0);
        c.size    = c.dir ? 0 : pack(fd.nFileSizeHigh, fd.nFileSizeLow);
        out.push_back(std::move(c));
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
    return out;
}

void clear_readonly(const std::string& p)
{
    const std::wstring w = lp(p);
    const DWORD a = ::GetFileAttributesW(w.c_str());
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY) != 0) { ::SetFileAttributesW(w.c_str(), a & ~FILE_ATTRIBUTE_READONLY); }
}

[[nodiscard]] std::string win_error(DWORD e)
{
    wchar_t buf[256]{};
    const DWORD n = ::FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0, buf, 256, nullptr);
    std::string s = text::narrow(std::wstring_view{buf, n});
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '.')) { s.pop_back(); }
    return s.empty() ? std::format("error {}", e) : s;
}

} // namespace

// the state of the job being run: what the worker counts, shared with the progress callback
struct TransferQueue::Run {
    explicit Run(TransferQueue& owner) : q{owner} {}
    TransferQueue& q;
    std::uint64_t  bytes_done{}, bytes_total{};
    std::uint64_t  items_done{}, items_total{};
    std::string    current;
    double         speed{};
    Clock::time_point last_publish{};
    Clock::time_point last_speed{};
    std::uint64_t  last_speed_bytes{};
    std::optional<ConflictChoice> all; // "do this for all conflicts"
    bool           cancelled{};

    void publish(TransferProgress& base, bool force)
    {
        const auto now = Clock::now();
        if (!force && now - last_publish < std::chrono::milliseconds(100)) { return; }
        last_publish = now;
        const double dt = std::chrono::duration<double>(now - last_speed).count();
        if (dt >= 0.3) {
            const double inst = static_cast<double>(bytes_done - std::min(bytes_done, last_speed_bytes)) / dt;
            speed = speed <= 0.0 ? inst : speed * 0.6 + inst * 0.4;
            last_speed       = now;
            last_speed_bytes = bytes_done;
        }
        {
            std::lock_guard g{q.lock_};
            q.shown_.bytes_done       = bytes_done;
            q.shown_.bytes_total      = bytes_total;
            q.shown_.items_done       = items_done;
            q.shown_.items_total      = items_total;
            q.shown_.current          = current;
            q.shown_.bytes_per_second = speed;
            q.shown_.paused           = q.paused_.load();
            q.shown_.scanning         = base.scanning;
        }
        if (q.wake_) { q.wake_(); }
    }

    // blocks while paused; false when the job was cancelled meanwhile
    bool wait_if_paused()
    {
        if (!q.paused_.load()) { return !q.cancel_.load(); }
        {
            std::lock_guard g{q.lock_};
            q.shown_.paused = true;
        }
        if (q.wake_) { q.wake_(); }
        std::unique_lock lk{q.lock_};
        q.cv_.wait(lk, [&] { return !q.paused_.load() || q.cancel_.load() || q.quit_.load(); });
        q.shown_.paused = false;
        last_speed       = Clock::now(); // (the pause is not slow copying)
        last_speed_bytes = bytes_done;
        return !q.cancel_.load();
    }
};

namespace {

struct Callback {
    TransferQueue::Run*  run;
    TransferProgress*    base;
    std::uint64_t        before;   // bytes of the earlier files
};

} // namespace

TransferQueue::~TransferQueue()
{
    {
        std::lock_guard g{lock_};
        quit_   = true;
        cancel_ = true;
        cancel_flag_ = TRUE;
    }
    cv_.notify_all();
    if (thread_.joinable()) { thread_.join(); }
}

void TransferQueue::add(OpKind kind, std::vector<std::string> sources, std::string dest, bool silent)
{
    if (sources.empty() || (kind != OpKind::copy && kind != OpKind::move)) { return; }
    {
        std::lock_guard g{lock_};
        queue_.push_back({kind, std::move(sources), std::move(dest), silent});
        if (!thread_.joinable()) { thread_ = std::thread([this] { work(); }); }
    }
    cv_.notify_all();
    if (wake_) { wake_(); }
}

TransferProgress TransferQueue::progress() const
{
    std::lock_guard g{lock_};
    TransferProgress p = shown_;
    p.waiting = queue_.size();
    p.active  = running_;
    return p;
}

bool TransferQueue::busy() const
{
    std::lock_guard g{lock_};
    return running_ || !queue_.empty();
}

void TransferQueue::set_paused(bool paused)
{
    {
        std::lock_guard g{lock_};
        paused_ = paused;
        shown_.paused = paused && running_;
    }
    cv_.notify_all();
}

void TransferQueue::cancel()
{
    {
        std::lock_guard g{lock_};
        cancel_ = true;
        cancel_flag_ = TRUE;
        paused_ = false;
        for (const Job& j : queue_) { // (what never started reports itself as cancelled, so its toast and undo stay honest)
            OpResult r;
            r.kind = j.kind; r.aborted = true; r.silent = j.silent; r.count = j.sources.size(); r.sources = j.sources; r.dest = j.dest;
            results_.push_back(std::move(r));
        }
        queue_.clear();
    }
    cv_.notify_all();
    if (wake_) { wake_(); }
}

std::optional<ConflictInfo> TransferQueue::conflict() const
{
    std::lock_guard g{lock_};
    return asking_;
}

void TransferQueue::resolve(std::uint64_t serial, ConflictChoice choice, bool for_all)
{
    {
        std::lock_guard g{lock_};
        if (!asking_ || asking_->serial != serial) { return; } // (an answer to a question that is gone)
        answer_     = choice;
        answer_all_ = for_all;
    }
    cv_.notify_all();
}

bool TransferQueue::poll(OpResult& out)
{
    std::lock_guard g{lock_};
    if (results_.empty()) { return false; }
    out = std::move(results_.front());
    results_.erase(results_.begin());
    return true;
}

namespace {

// copy progress: counts the bytes, keeps the ui informed, pauses and cancels from inside the copy
DWORD CALLBACK copy_progress(LARGE_INTEGER, LARGE_INTEGER transferred, LARGE_INTEGER, LARGE_INTEGER, DWORD, DWORD, HANDLE, HANDLE, LPVOID data)
{
    auto* cb = static_cast<Callback*>(data);
    cb->run->bytes_done = cb->before + static_cast<std::uint64_t>(transferred.QuadPart);
    cb->run->publish(*cb->base, false);
    if (!cb->run->wait_if_paused()) { return PROGRESS_CANCEL; }
    return PROGRESS_CONTINUE;
}

} // namespace

void TransferQueue::work()
{
    for (;;) {
        Job job;
        {
            std::unique_lock lk{lock_};
            cv_.wait(lk, [&] { return quit_.load() || !queue_.empty(); });
            if (quit_) { return; }
            job = std::move(queue_.front());
            queue_.erase(queue_.begin());
            running_ = true;
            cancel_  = false;
            cancel_flag_ = FALSE;
            paused_  = false;
            shown_   = {};
            shown_.kind     = job.kind;
            shown_.scanning = true;
            const std::string where = path::leaf(job.dest);
            shown_.title = std::format("{} {} to {}", job.kind == OpKind::copy ? "Copying" : "Moving",
                                       job.sources.size() == 1 ? path::leaf(job.sources.front()) : std::format("{} items", job.sources.size()),
                                       where.empty() ? job.dest : where);
        }
        if (wake_) { wake_(); }

        OpResult res;
        res.kind    = job.kind;
        res.silent  = job.silent;
        res.count   = job.sources.size();
        res.sources = job.sources;
        res.dest    = job.dest;
        res.made.assign(job.sources.size(), std::string{});

        Run run{*this};
        TransferProgress base;
        base.scanning = true;
        run.last_speed = Clock::now();
        std::size_t failed = 0, skipped_links = 0;
        std::string first_error;
        const bool move = job.kind == OpKind::move;

        if (!attr_of(job.dest).dir) {
            res.message = "The destination folder is not there any more";
            failed = 1;
        } else {
            // 1. what is there to do: whole items that a rename can move at once, and the rest as a list of files
            std::vector<Item> items;
            std::vector<std::pair<std::size_t, std::string>> recycle_records;
            for (std::size_t top = 0; top < job.sources.size() && !cancel_; ++top) {
                const std::string& src = job.sources[top];
                const Attr sa = attr_of(src);
                if (!sa.exists) { ++failed; if (first_error.empty()) { first_error = path::leaf(src) + " is not there any more"; } continue; }
                const bool same_folder = same_text(path::parent(src), path::normalize(job.dest));
                if (move && same_folder) { res.made[top] = src; continue; } // (moving into its own folder: nothing to do)
                if (sa.dir && is_inside(job.dest, src)) {
                    ++failed;
                    if (first_error.empty()) { first_error = "A folder cannot be put inside itself"; }
                    continue;
                }
                RecycledName      recycled = recycled_name(src);
                const std::string name     = recycled.name.empty() ? path::leaf(src) : recycled.name;
                if (!recycled.info.empty()) { recycle_records.emplace_back(top, std::move(recycled.info)); }
                const std::string target_name = !move && same_folder ? copy_name(job.dest, name, sa.dir) : name;
                const std::string dst = path::join(job.dest, target_name);

                const bool sv = move && same_volume(src, job.dest);
                if (sv && !attr_of(dst).exists) {
                    if (::MoveFileExW(lp(src).c_str(), lp(dst).c_str(), 0)) {
                        res.made[top] = dst;
                        continue;
                    }
                    // (a rename that fails - locked, no rights - falls back to the file-by-file way below, which reports each file)
                }
                // enumerate
                Item root;
                root.src = src; root.dst = dst; root.dir = sa.dir; root.size = sa.size; root.top = top;
                root.link = !sa.dir && sa.reparse;
                root.same_vol = sv;
                items.push_back(root);
                if (sa.dir && !sa.reparse) {
                    std::vector<std::pair<std::string, std::string>> stack{{src, dst}};
                    while (!stack.empty() && !cancel_) {
                        const auto [s, d] = stack.back();
                        stack.pop_back();
                        for (const Child& c : list_children(s)) {
                            Item it;
                            it.src = path::join(s, c.name);
                            it.dst = path::join(d, c.name);
                            it.dir = c.dir;
                            it.size = c.size;
                            it.top = top;
                            it.same_vol = sv;
                            if (c.dir && c.reparse) { ++skipped_links; continue; } // (a junction or folder link is not followed)
                            it.link = !c.dir && c.reparse;
                            items.push_back(it);
                            if (c.dir) { stack.emplace_back(it.src, it.dst); }
                        }
                    }
                }
                run.current = path::leaf(src);
                if ((items.size() & 0x3ff) == 0) { run.publish(base, false); }
            }
            for (const Item& it : items) {
                if (!it.dir) { run.bytes_total += it.size; ++run.items_total; }
            }
            base.scanning = false;
            run.publish(base, true);

            // 2. do it
            std::vector<std::string> skip_prefix;  // folders that could not be made: everything below them is skipped
            std::vector<std::size_t> top_failed(job.sources.size(), 0);
            for (std::size_t i = 0; i < items.size() && !cancel_; ++i) {
                Item& it = items[i];
                const bool under_skipped = std::ranges::any_of(skip_prefix, [&](const std::string& pre) { return is_inside(it.src, pre); });
                if (under_skipped) {
                    if (!it.dir) { run.bytes_done += it.size; ++run.items_done; }
                    continue;
                }
                run.current = path::leaf(it.src);
                const Attr da = attr_of(it.dst);

                if (it.dir) {
                    if (da.exists && !da.dir) { // a file is in the way of the folder
                        ++failed; ++top_failed[it.top];
                        if (first_error.empty()) { first_error = path::leaf(it.dst) + " exists and is not a folder"; }
                        skip_prefix.push_back(it.src);
                        continue;
                    }
                    if (!da.exists && !::CreateDirectoryW(lp(it.dst).c_str(), nullptr)) {
                        const DWORD err = ::GetLastError();
                        if (err == ERROR_ACCESS_DENIED) { res.access_denied = true; }
                        ++failed; ++top_failed[it.top];
                        if (first_error.empty()) { first_error = std::format("{}: {}", path::leaf(it.dst), win_error(err)); }
                        skip_prefix.push_back(it.src);
                        continue;
                    }
                    // preserve folder timestamps and basic attributes (hidden / system / readonly)
                    if (!da.exists) {
                        const Attr sa = attr_of(it.src);
                        if (sa.exists) {
                            const DWORD keep = sa.raw_attrs & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_READONLY);
                            if (keep != 0) { ::SetFileAttributesW(lp(it.dst).c_str(), keep); }
                            HANDLE dh = ::CreateFileW(lp(it.dst).c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
                            if (dh != INVALID_HANDLE_VALUE) {
                                ::SetFileTime(dh, &sa.ft_created, &sa.ft_accessed, &sa.ft_modified);
                                ::CloseHandle(dh);
                            }
                        }
                    }
                    if (res.made[it.top].empty() && it.src == job.sources[it.top]) { res.made[it.top] = it.dst; }
                    continue;
                }

                std::string dst = it.dst;
                bool replace = false;
                if (da.exists) {
                    if (da.dir) { // a folder is in the way of the file
                        ++failed; ++top_failed[it.top];
                        if (first_error.empty()) { first_error = path::leaf(it.dst) + " exists and is a folder"; }
                        run.bytes_done += it.size; ++run.items_done;
                        continue;
                    }
                    ConflictChoice choice = run.all.value_or(ConflictChoice::replace);
                    if (!run.all) {
                        ConflictInfo info;
                        info.source = it.src; info.target = it.dst;
                        info.source_size = it.size; info.target_size = da.size;
                        info.source_modified = attr_of(it.src).modified; info.target_modified = da.modified;
                        std::unique_lock lk{lock_};
                        info.serial = ++serial_;
                        asking_ = info;
                        answer_.reset();
                        if (wake_) { lk.unlock(); wake_(); lk.lock(); }
                        cv_.wait(lk, [&] { return answer_.has_value() || cancel_.load() || quit_.load(); });
                        choice = answer_.value_or(ConflictChoice::cancel);
                        if (cancel_ || quit_) { choice = ConflictChoice::cancel; }
                        if (answer_all_ && choice != ConflictChoice::cancel) { run.all = choice; }
                        answer_.reset();
                        answer_all_ = false;
                        asking_.reset();
                    }
                    if (choice == ConflictChoice::cancel) { cancel_ = true; break; }
                    if (choice == ConflictChoice::skip) {
                        run.bytes_done += it.size; ++run.items_done;
                        continue;
                    }
                    if (choice == ConflictChoice::keep_both) { dst = path::join(path::parent(it.dst), numbered(path::parent(it.dst), path::leaf(it.dst), false)); }
                    else { replace = true; }
                }

                bool ok = false;
                DWORD error = 0;
                if (it.same_vol) {
                    if (replace) { clear_readonly(dst); }
                    ok = ::MoveFileExW(lp(it.src).c_str(), lp(dst).c_str(), replace ? MOVEFILE_REPLACE_EXISTING : 0) != FALSE;
                    error = ok ? 0 : ::GetLastError();
                    if (ok) { run.bytes_done += it.size; }
                } else {
                    if (replace) { clear_readonly(dst); }
                    Callback cb{&run, &base, run.bytes_done};
                    DWORD flags = COPY_FILE_ALLOW_DECRYPTED_DESTINATION;
                    if (it.link) { flags |= COPY_FILE_COPY_SYMLINK; }
                    ok = ::CopyFileExW(lp(it.src).c_str(), lp(dst).c_str(), copy_progress, &cb, reinterpret_cast<LPBOOL>(&cancel_flag_), flags) != FALSE;
                    error = ok ? 0 : ::GetLastError();
                    if (ok) {
                        run.bytes_done = cb.before + it.size;
                        if (move) {
                            clear_readonly(it.src);
                            if (!::DeleteFileW(lp(it.src).c_str())) { ok = false; error = ::GetLastError(); }
                        }
                    } else if (error == ERROR_REQUEST_ABORTED || cancel_) {
                        ::DeleteFileW(lp(dst).c_str()); // (a half-written copy is not left behind)
                        break;
                    } else {
                        run.bytes_done = cb.before + it.size;
                    }
                }
                ++run.items_done;
                if (ok) {
                    if (res.made[it.top].empty() && it.src == job.sources[it.top]) { res.made[it.top] = dst; }
                } else {
                    if (error == ERROR_ACCESS_DENIED) { res.access_denied = true; }
                    ++failed; ++top_failed[it.top];
                    if (first_error.empty()) { first_error = std::format("{}: {}", path::leaf(it.src), win_error(error)); }
                }
                run.publish(base, false);
            }

            // 3. a move leaves empty folders behind (it went file by file): take them away, deepest first. a folder that still
            // holds something (a skipped file, a failure) simply does not go
            if (move && !cancel_) {
                for (std::size_t i = items.size(); i-- > 0;) {
                    if (items[i].dir && top_failed[items[i].top] == 0) { ::RemoveDirectoryW(lp(items[i].src).c_str()); }
                }
            }
            // what left the Recycle Bin is no longer listed there: drop its record too
            if (move) {
                for (const auto& [top, info] : recycle_records) {
                    if (!attr_of(job.sources[top]).exists) { ::DeleteFileW(lp(info).c_str()); }
                }
            }
            res.aborted = cancel_.load();
        }
        run.publish(base, true);

        if (failed > 0 && res.message.empty()) {
            res.message = failed == 1 ? "Could not finish: " + first_error : std::format("{} items were not {}: {}", failed, move ? "moved" : "copied", first_error);
        }
        res.ok = !res.aborted && failed == 0;
        (void)skipped_links;
        {
            std::lock_guard g{lock_};
            results_.push_back(std::move(res));
            running_ = false;
            shown_   = {};
        }
        if (wake_) { wake_(); }
    }
}
#else
TransferQueue::~TransferQueue()
{
    {
        std::lock_guard g{lock_};
        quit_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) { thread_.join(); }
}
void TransferQueue::add(OpKind, std::vector<std::string>, std::string, bool) {}
TransferProgress TransferQueue::progress() const { return {}; }
bool TransferQueue::busy() const { return false; }
void TransferQueue::set_paused(bool paused) { paused_ = paused; }
void TransferQueue::cancel() { cancel_ = true; }
std::optional<ConflictInfo> TransferQueue::conflict() const { return std::nullopt; }
void TransferQueue::resolve(std::uint64_t, ConflictChoice, bool) {}
bool TransferQueue::poll(OpResult&) { return false; }
void TransferQueue::work() {}
#endif

} // namespace prism::core
