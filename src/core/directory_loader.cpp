#include "core/directory_loader.hpp"
#include "core/thread_priority.hpp"

#include "core/log.hpp"
#include "core/path.hpp"
#include "core/rclone.hpp"
#include "core/recycle.hpp"
#include "core/text.hpp"

#ifdef _WIN32
#include <windows.h>
#include <winnetwk.h>
#else
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <chrono>
#include <format>
#include <cstdio>
#include <thread>

namespace prism::core {

namespace {

#ifdef _WIN32
[[nodiscard]] std::uint64_t pack(DWORD hi, DWORD lo) noexcept
{
    return (static_cast<std::uint64_t>(hi) << 32) | lo;
}

[[nodiscard]] FileEntry make_entry(const WIN32_FIND_DATAW& fd, const std::string& rel_dir = {})
{
    FileEntry e;
    const std::string leaf = text::narrow(fd.cFileName);
    e.name       = rel_dir + leaf;
    e.attributes = fd.dwFileAttributes;
    e.modified   = pack(fd.ftLastWriteTime.dwHighDateTime, fd.ftLastWriteTime.dwLowDateTime);
    if (e.is_dir()) {
        e.kind       = FileKind::folder;
        e.ext_offset = static_cast<std::uint16_t>(e.name.size());
    } else {
        e.size = pack(fd.nFileSizeHigh, fd.nFileSizeLow);
        // the extension is what follows the last dot, unless that dot starts the name (".gitignore" is a name)
        const std::size_t dot = e.name.rfind('.');
        if (dot == std::string::npos || dot < rel_dir.size() + 1 || dot + 1 >= e.name.size()) {
            e.ext_offset = static_cast<std::uint16_t>(e.name.size());
        } else {
            e.ext_offset = static_cast<std::uint16_t>(dot + 1);
        }
        e.kind = classify_extension(text::lower_ascii(e.ext()));
    }
    return e;
}
#else
[[nodiscard]] FileEntry make_entry_posix(const std::string& full_path, const std::string& leaf, const std::string& rel_dir = {})
{
    FileEntry e;
    e.name = rel_dir + leaf;
    struct stat lst{}, st{};
    const bool has_lstat = ::lstat(full_path.c_str(), &lst) == 0;
    const bool is_link   = has_lstat && S_ISLNK(lst.st_mode);
    const bool has_stat  = ::stat(full_path.c_str(), &st) == 0;
    const struct stat& s = has_stat ? st : lst;
    if (has_stat || has_lstat) {
        if (S_ISDIR(s.st_mode)) { e.attributes |= attr::directory; }
        if (is_link) { e.attributes |= attr::reparse; }
        if (!leaf.empty() && leaf.front() == '.') { e.attributes |= attr::hidden; }
        if ((s.st_mode & 0222) == 0) { e.attributes |= attr::readonly; }
        e.modified = (static_cast<std::uint64_t>(s.st_mtime) + 11644473600ULL) * 10000000ULL;
        if (!e.is_dir()) { e.size = static_cast<std::uint64_t>(std::max<off_t>(0, s.st_size)); }
    }
    if (e.is_dir()) {
        e.kind       = FileKind::folder;
        e.ext_offset = static_cast<std::uint16_t>(e.name.size());
    } else {
        const std::size_t dot = e.name.rfind('.');
        if (dot == std::string::npos || dot < rel_dir.size() + 1 || dot + 1 >= e.name.size()) {
            e.ext_offset = static_cast<std::uint16_t>(e.name.size());
        } else {
            e.ext_offset = static_cast<std::uint16_t>(dot + 1);
        }
        e.kind = classify_extension(text::lower_ascii(e.ext()));
    }
    return e;
}
#endif

} // namespace

FileEntry entry_from_find_data(const void* fd, const std::string& rel_dir)
{
#ifdef _WIN32
    return make_entry(*static_cast<const WIN32_FIND_DATAW*>(fd), rel_dir);
#else
    (void)fd; (void)rel_dir;
    return {};
#endif
}

void DirectoryLoader::start(const std::string& utf8_path)
{
    cancel();
    job_      = std::make_shared<Job>();
    job_done_ = false;

    std::wstring pattern;
    if (rclone::is_path(utf8_path)) {
        pattern = text::widen(utf8_path);
    } else {
        pattern = path::long_wide(utf8_path);
#ifdef _WIN32
        if (!pattern.empty() && pattern.back() != L'\\') { pattern.push_back(L'\\'); }
        pattern.push_back(L'*');
#endif
    }

    // detached: an abandoned listing of a dead share may block in the kernel for a long time; it owns its job, and
    // `cancelled` makes it drop everything the moment it returns
    std::thread{&DirectoryLoader::work, job_, std::move(pattern), wake_}.detach();
}

void DirectoryLoader::cancel() noexcept
{
    if (job_ != nullptr) { job_->cancelled.store(true, std::memory_order_relaxed); }
    job_.reset();
    job_done_ = true;
}

DirectoryLoader::Progress DirectoryLoader::poll(std::vector<FileEntry>& out)
{
    Progress p;
    if (job_ == nullptr) { return p; }
    std::lock_guard guard{job_->lock};
    if (!job_->pending.empty()) {
        if (out.empty()) {
            out = std::move(job_->pending);
        } else {
            out.insert(out.end(), std::make_move_iterator(job_->pending.begin()), std::make_move_iterator(job_->pending.end()));
        }
        job_->pending.clear();
        p.any = true;
    }
    if (job_->done) {
        p.done  = true;
        p.error = job_->error;
        p.any   = true;
        job_done_ = true;
    }
    return p;
}

void DirectoryLoader::work(std::shared_ptr<Job> job, std::wstring pattern, std::function<void()> wake)
{
    using clock = std::chrono::steady_clock;

    std::vector<FileEntry> batch;
    batch.reserve(1024);
    auto last_flush = clock::now();

    const auto flush = [&](bool final_batch, std::uint32_t error) {
        if (job->cancelled.load(std::memory_order_relaxed)) { return; }
        {
            std::lock_guard guard{job->lock};
            if (job->pending.empty()) {
                job->pending = std::move(batch);
            } else {
                job->pending.insert(job->pending.end(), std::make_move_iterator(batch.begin()), std::make_move_iterator(batch.end()));
            }
            if (final_batch) {
                job->done  = true;
                job->error = error;
            }
        }
        batch.clear();
        batch.reserve(1024);
        last_flush = clock::now();
        if (wake) { wake(); }
    };

    // rclone cloud remote ("rclone:<remote>:\...")
    if (pattern.starts_with(L"rclone:")) {
        std::string err;
        const bool ok = rclone::list_dir(text::narrow(pattern), batch, err);
        flush(true, ok ? 0u : 53u);
        return;
    }

#ifdef _WIN32

    // bare UNC server ("\\server" or "\\wsl.localhost"): enumerate SMB / WSL shares via WNetOpenEnumW
    {
        std::string raw_dir = text::narrow(pattern);
        if (raw_dir.size() >= 2 && raw_dir.ends_with("\\*")) { raw_dir.resize(raw_dir.size() - 2); }
        if (path::is_unc_server(raw_dir)) {
            std::wstring srv = text::widen(raw_dir);
            NETRESOURCEW nr{};
            nr.dwScope       = RESOURCE_GLOBALNET;
            nr.dwType        = RESOURCETYPE_DISK;
            nr.dwDisplayType = RESOURCEDISPLAYTYPE_SERVER;
            nr.dwUsage       = RESOURCEUSAGE_CONTAINER;
            nr.lpRemoteName  = srv.data();
            HANDLE henum = nullptr;
            DWORD err = ::WNetOpenEnumW(RESOURCE_GLOBALNET, RESOURCETYPE_DISK, 0, &nr, &henum);
            if (err == NO_ERROR && henum != nullptr) {
                std::vector<std::uint8_t> buf(16384);
                for (;;) {
                    DWORD count = 0xffffffffu;
                    DWORD sz    = static_cast<DWORD>(buf.size());
                    const DWORD r = ::WNetEnumResourceW(henum, &count, buf.data(), &sz);
                    if (r == NO_ERROR) {
                        const auto* res = reinterpret_cast<const NETRESOURCEW*>(buf.data());
                        for (DWORD i = 0; i < count; ++i) {
                            if (res[i].lpRemoteName == nullptr) { continue; }
                            const std::string full = text::narrow(res[i].lpRemoteName);
                            const std::string share = path::leaf(full);
                            if (share.empty()) { continue; }
                            FileEntry e;
                            e.name       = share;
                            e.attributes = attr::directory;
                            e.kind       = FileKind::folder;
                            e.ext_offset = static_cast<std::uint16_t>(e.name.size());
                            batch.push_back(std::move(e));
                        }
                    } else {
                        err = r == ERROR_NO_MORE_ITEMS ? 0 : r;
                        break;
                    }
                }
                ::WNetCloseEnum(henum);
            }
            flush(true, err);
            return;
        }
    }

    // the all-bins view lists every drive's bin together, so its entries carry their full paths as names
    const bool all_bins = pattern == text::widen(std::string{recycle::all} + "\\*");
    std::vector<std::wstring> patterns;
    if (all_bins) {
        for (const std::string& b : recycle::folders()) { patterns.push_back(text::widen(b) + L"\\*"); }
    } else {
        patterns.push_back(pattern);
    }
    WIN32_FIND_DATAW fd;
    const auto started = clock::now();
    std::size_t listed = 0;
    DWORD last_error = 0;
    // a slow folder is the thing the log is for: how long the first entry took (network, antivirus, sleeping disk) and the whole listing
    const auto note = [&](const char* how) {
        const double total = std::chrono::duration<double, std::milli>(clock::now() - started).count();
        if (log::enabled() && total >= 150.0) { log::write(std::format("folder {} {}: {} entries in {:.0f} ms", how, text::narrow(pattern), listed, total)); }
    };
    for (const std::wstring& pat : patterns) {
        const std::string raw     = text::narrow(pat);
        const bool        in_bin  = recycle::inside(raw);
        const std::string bin_dir = in_bin && raw.size() > 1 ? raw.substr(0, raw.size() - 1) : std::string{}; // (the pattern ends in "\*")
        // FindExInfoBasic skips the short (8.3) name; LARGE_FETCH asks the filesystem for bigger buffers
        HANDLE h = ::FindFirstFileExW(pat.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) {
            if (all_bins) { continue; } // (a drive that went away: the others still list)
            const DWORD e = ::GetLastError();
            note("failed");
            flush(true, e);
            return;
        }
        if (log::enabled()) {
            const double first = std::chrono::duration<double, std::milli>(clock::now() - started).count();
            if (first >= 100.0) { log::write(std::format("folder {}: first entry took {:.0f} ms", raw, first)); }
        }
        do {
            ++listed;
            const wchar_t* n = fd.cFileName;
            if (n[0] == L'.' && (n[1] == L'\0' || (n[1] == L'.' && n[2] == L'\0'))) { continue; }
            if (in_bin && (n[0] == L'$') && (n[1] == L'I' || n[1] == L'i')) { continue; } // (a record, not an item)
            batch.push_back(make_entry(fd));
            FileEntry& e = batch.back();
            if (in_bin && n[0] == L'$' && (n[1] == L'R' || n[1] == L'r')) {
                // the bin's own name is `$R4ND0M.ext`: show the name it was deleted under, and when
                if (const auto rec = recycle::read(bin_dir + text::narrow(n))) {
                    e.set_recycle(path::leaf(rec->original), rec->original);
                    e.modified = rec->deleted;
                }
            }
            if (all_bins) {
                if (!e.has_display_override()) { e.set_shown(e.name); } // (no record: the bin's own name, not the whole path)
                e.name       = bin_dir + e.name;
                e.ext_offset = static_cast<std::uint16_t>(e.ext_offset + bin_dir.size());
            }
            // first rows quickly, then bigger and rarer batches (check clock every 64 entries to avoid 200k syscalls)
            if (batch.size() >= 4096 || ((batch.size() & 63) == 0 && batch.size() >= 64 && clock::now() - last_flush > std::chrono::milliseconds{40})) {
                if (job->cancelled.load(std::memory_order_relaxed)) { break; }
                flush(false, 0);
            }
        } while (::FindNextFileW(h, &fd));
        const DWORD err = ::GetLastError();
        ::FindClose(h);
        last_error = err == ERROR_NO_MORE_FILES ? 0 : err;
    }
    note("listed");
    flush(true, all_bins ? 0 : last_error);
#else
    std::string dir = text::narrow(pattern);
    if (dir == recycle::all) {
        const auto bins = recycle::folders();
        dir = bins.empty() ? std::string{} : bins.front();
    }
    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr) {
        const int e = errno;
        flush(true, e == ENOENT ? 2u : e == EACCES ? 5u : 1u);
        return;
    }
    while (const dirent* ent = ::readdir(d)) {
        const char* n = ent->d_name;
        if (n[0] == '.' && (n[1] == '\0' || (n[1] == '.' && n[2] == '\0'))) { continue; }
        const std::string leaf{n};
        batch.push_back(make_entry_posix(path::join(dir, leaf), leaf));
        if (batch.size() >= 4096 || ((batch.size() & 63) == 0 && batch.size() >= 64 && clock::now() - last_flush > std::chrono::milliseconds{40})) {
            if (job->cancelled.load(std::memory_order_relaxed)) { break; }
            flush(false, 0);
        }
    }
    ::closedir(d);
    flush(true, 0);
#endif
}

} // namespace prism::core

namespace prism::core {

void DirectoryLoader::start_search(const std::string& utf8_root, Query query)
{
    cancel();
    job_      = std::make_shared<Job>();
    job_done_ = false;
    std::thread{&DirectoryLoader::work_search, job_, path::long_wide(utf8_root), std::move(query), wake_}.detach();
}

void DirectoryLoader::work_search(std::shared_ptr<Job> job, std::wstring root, Query query, std::function<void()> wake)
{
    lower_thread_priority();
    using clock = std::chrono::steady_clock;
    constexpr std::size_t max_results = 200000;

    std::vector<FileEntry> batch;
    std::size_t found = 0;
    auto last_flush = clock::now();
    const auto flush = [&](bool final_batch) {
        if (job->cancelled.load(std::memory_order_relaxed)) { return; }
        {
            std::lock_guard guard{job->lock};
            job->pending.insert(job->pending.end(), std::make_move_iterator(batch.begin()), std::make_move_iterator(batch.end()));
            if (final_batch) { job->done = true; }
        }
        batch.clear();
        last_flush = clock::now();
        if (wake) { wake(); }
    };

    struct Dir { std::wstring path; std::string rel; };
    std::vector<Dir> stack;
    stack.push_back({root, {}});
    while (!stack.empty() && found < max_results && !job->cancelled.load(std::memory_order_relaxed)) {
        Dir dir = std::move(stack.back());
        stack.pop_back();
#ifdef _WIN32
        WIN32_FIND_DATAW fd;
        HANDLE h = ::FindFirstFileExW((dir.path + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) { continue; }
        do {
            const wchar_t* n = fd.cFileName;
            if (n[0] == L'.' && (n[1] == L'\0' || (n[1] == L'.' && n[2] == L'\0'))) { continue; }
            const bool is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            FileEntry entry = make_entry(fd, dir.rel);
            if (query.matches(entry)) {
                batch.push_back(std::move(entry));
                ++found;
            }
            // (symlinks / junctions are listed but not followed: no cycles; cloud sync folders ARE followed)
            const bool is_link = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 && path::is_link_reparse_tag(fd.dwReserved0);
            if (is_dir && !is_link) {
                stack.push_back({dir.path + L"\\" + n, dir.rel + text::narrow(n) + "\\"});
            }
            if (batch.size() >= 2048 || ((batch.size() & 15) == 0 && batch.size() >= 16 && clock::now() - last_flush > std::chrono::milliseconds{60})) { flush(false); }
        } while (::FindNextFileW(h, &fd) && found < max_results && !job->cancelled.load(std::memory_order_relaxed));
        ::FindClose(h);
#else
        const std::string udir = text::narrow(dir.path);
        DIR* d = ::opendir(udir.c_str());
        if (d == nullptr) { continue; }
        while (const dirent* ent = ::readdir(d)) {
            if (found >= max_results || job->cancelled.load(std::memory_order_relaxed)) { break; }
            const char* n = ent->d_name;
            if (n[0] == '.' && (n[1] == '\0' || (n[1] == '.' && n[2] == '\0'))) { continue; }
            const std::string leaf{n};
            const std::string full = path::join(udir, leaf);
            FileEntry entry = make_entry_posix(full, leaf, dir.rel);
            const bool is_dir  = entry.is_dir();
            const bool is_link = (entry.attributes & attr::reparse) != 0;
            if (query.matches(entry)) {
                batch.push_back(std::move(entry));
                ++found;
            }
            if (is_dir && !is_link) {
                stack.push_back({text::widen(full), dir.rel + leaf + "/"});
            }
            if (batch.size() >= 2048 || ((batch.size() & 15) == 0 && batch.size() >= 16 && clock::now() - last_flush > std::chrono::milliseconds{60})) { flush(false); }
        }
        ::closedir(d);
#endif
    }
    flush(true);
}

} // namespace prism::core
