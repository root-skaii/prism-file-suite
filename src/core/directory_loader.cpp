#include "core/directory_loader.hpp"

#include "core/text.hpp"

#include <windows.h>

#include <chrono>
#include <thread>

namespace prism::core {

namespace {

[[nodiscard]] std::uint64_t pack(DWORD hi, DWORD lo) noexcept
{
    return (static_cast<std::uint64_t>(hi) << 32) | lo;
}

[[nodiscard]] FileEntry make_entry(const WIN32_FIND_DATAW& fd)
{
    FileEntry e;
    e.name       = text::narrow(fd.cFileName);
    e.attributes = fd.dwFileAttributes;
    e.modified   = pack(fd.ftLastWriteTime.dwHighDateTime, fd.ftLastWriteTime.dwLowDateTime);
    if (e.is_dir()) {
        e.kind       = FileKind::folder;
        e.ext_offset = static_cast<std::uint16_t>(e.name.size());
    } else {
        e.size = pack(fd.nFileSizeHigh, fd.nFileSizeLow);
        // the extension is what follows the last dot, unless that dot starts the name (".gitignore" is a name)
        const std::size_t dot = e.name.rfind('.');
        if (dot == std::string::npos || dot == 0 || dot + 1 >= e.name.size()) {
            e.ext_offset = static_cast<std::uint16_t>(e.name.size());
        } else {
            e.ext_offset = static_cast<std::uint16_t>(dot + 1);
        }
        e.kind = classify_extension(text::lower_ascii(e.ext()));
    }
    return e;
}

} // namespace

void DirectoryLoader::start(const std::string& utf8_path)
{
    cancel();
    job_      = std::make_shared<Job>();
    job_done_ = false;

    std::wstring pattern = text::widen(utf8_path);
    if (!pattern.empty() && pattern.back() != L'\\') { pattern.push_back(L'\\'); }
    pattern.push_back(L'*');

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

    const auto flush = [&](bool final_batch, DWORD error) {
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

    WIN32_FIND_DATAW fd;
    // FindExInfoBasic skips the short (8.3) name; LARGE_FETCH asks the filesystem for bigger buffers
    HANDLE h = ::FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) {
        flush(true, ::GetLastError());
        return;
    }
    do {
        const wchar_t* n = fd.cFileName;
        if (n[0] == L'.' && (n[1] == L'\0' || (n[1] == L'.' && n[2] == L'\0'))) { continue; }
        batch.push_back(make_entry(fd));
        // first rows quickly, then bigger and rarer batches
        const auto age = clock::now() - last_flush;
        if (batch.size() >= 4096 || (batch.size() >= 64 && age > std::chrono::milliseconds{40})) {
            if (job->cancelled.load(std::memory_order_relaxed)) { break; }
            flush(false, 0);
        }
    } while (::FindNextFileW(h, &fd));
    const DWORD err = ::GetLastError();
    ::FindClose(h);
    flush(true, err == ERROR_NO_MORE_FILES ? 0 : err);
}

} // namespace prism::core
