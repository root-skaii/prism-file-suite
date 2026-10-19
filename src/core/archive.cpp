#include "core/archive.hpp"

#include "core/path.hpp"
#include "core/process.hpp"
#include "core/text.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdio>

namespace prism::core {

namespace {

constexpr std::array<std::string_view, 12> archive_exts{".zip", ".7z", ".tar", ".gz", ".tgz", ".bz2", ".xz", ".rar", ".cab", ".iso", ".zst", ".tbz2"};

[[nodiscard]] std::wstring tar_exe()
{
    wchar_t dir[MAX_PATH]{};
    ::GetSystemDirectoryW(dir, MAX_PATH);
    return std::wstring{dir} + L"\\tar.exe";
}

[[nodiscard]] std::wstring quote_arg(const std::string& s) { return L"\"" + text::widen(s) + L"\""; }

// the last meaningful line of what tar said
[[nodiscard]] std::string reason(const std::string& output)
{
    std::string line;
    std::size_t end = output.find_last_not_of("\r\n ");
    if (end == std::string::npos) { return "unknown error"; }
    const std::size_t start = output.find_last_of('\n', end);
    line = output.substr(start == std::string::npos ? 0 : start + 1, end - (start == std::string::npos ? 0 : start));
    return line;
}

[[nodiscard]] std::string unique_name(const std::string& dir, const std::string& stem, const std::string& ext)
{
    for (int i = 1; i < 1000; ++i) {
        const std::string name = (i == 1 ? stem : stem + " (" + std::to_string(i) + ")") + ext;
        if (!path::exists(path::join(dir, name))) { return name; }
    }
    return stem + ext;
}

} // namespace

bool is_archive_name(std::string_view name)
{
    const std::string lower = text::lower_ascii(name);
    return std::ranges::any_of(archive_exts, [&](std::string_view e) { return lower.size() > e.size() && lower.ends_with(e); });
}

std::string archive_stem(std::string_view name)
{
    std::string s{name};
    const std::string lower = text::lower_ascii(name);
    for (const std::string_view two : {".tar.gz", ".tar.bz2", ".tar.xz", ".tar.zst"}) {
        if (lower.ends_with(two) && lower.size() > two.size()) { return s.substr(0, s.size() - two.size()); }
    }
    const std::size_t dot = s.rfind('.');
    return dot == std::string::npos || dot == 0 ? s : s.substr(0, dot);
}

// --- browsing in place --------------------------------------------------------------------------------------------

namespace {

[[nodiscard]] std::string key_of(std::string_view p) { return text::lower_ascii(path::normalize(p)); }

[[nodiscard]] std::string cache_root()
{
    wchar_t tmp[MAX_PATH + 1]{};
    const DWORD n = ::GetTempPathW(MAX_PATH, tmp);
    std::string t = text::narrow(std::wstring_view{tmp, n});
    while (!t.empty() && t.back() == '\\') { t.pop_back(); }
    return t + "\\PrismFS\\archive-view";
}

void remove_tree(const std::wstring& dir)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = ::FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") { continue; }
            const std::wstring full = dir + L"\\" + name;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 && (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
                remove_tree(full);
            } else {
                ::SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_NORMAL); // (a read-only file inside an archive would refuse to go)
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) { ::RemoveDirectoryW(full.c_str()); } else { ::DeleteFileW(full.c_str()); }
            }
        } while (::FindNextFileW(h, &fd));
        ::FindClose(h);
    }
    ::RemoveDirectoryW(dir.c_str());
}

} // namespace

void ArchiveViews::add(std::string root, std::string archive)
{
    root = key_of(root);
    std::erase_if(views_, [&](const auto& v) { return v.first == root; });
    views_.emplace_back(std::move(root), std::move(archive));
}

bool ArchiveViews::has_root(std::string_view root) const
{
    const std::string k = key_of(root);
    return std::ranges::any_of(views_, [&](const auto& v) { return v.first == k; });
}

std::optional<ArchiveView> ArchiveViews::find(std::string_view p) const
{
    if (views_.empty()) { return std::nullopt; }
    const std::string full = path::normalize(p);
    const std::string k    = text::lower_ascii(full);
    for (const auto& [root, archive] : views_) {
        if (k == root) { return ArchiveView{archive, {}}; }
        if (k.size() > root.size() && k.starts_with(root) && k[root.size()] == '\\') { return ArchiveView{archive, full.substr(root.size() + 1)}; }
    }
    return std::nullopt;
}

std::optional<std::string> ArchiveViews::leave_target(std::string_view p) const
{
    const std::string k = key_of(p);
    for (const auto& [root, archive] : views_) {
        if (k == root) { return path::parent(archive); }
    }
    return std::nullopt;
}

std::optional<std::string> ArchiveViews::root_for(std::string_view archive) const
{
    const std::string k = key_of(archive);
    for (const auto& [root, a] : views_) {
        if (key_of(a) == k) { return root; }
    }
    return std::nullopt;
}

std::string archive_cache_dir(const std::string& archive, bool create)
{
    WIN32_FILE_ATTRIBUTE_DATA a{};
    ::GetFileAttributesExW(text::widen(archive).c_str(), GetFileExInfoStandard, &a);
    std::uint64_t h = 1469598103934665603ull; // FNV-1a over the path, size and write time
    const auto mix = [&](std::uint64_t v) { for (int i = 0; i < 8; ++i) { h = (h ^ ((v >> (i * 8)) & 0xff)) * 1099511628211ull; } };
    for (const char c : key_of(archive)) { h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ull; }
    mix((static_cast<std::uint64_t>(a.nFileSizeHigh) << 32) | a.nFileSizeLow);
    mix((static_cast<std::uint64_t>(a.ftLastWriteTime.dwHighDateTime) << 32) | a.ftLastWriteTime.dwLowDateTime);
    char hex[17];
    std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(h));
    const std::string dir = cache_root() + "\\" + hex;
    if (create) {
        // every level from the temp folder down (CreateDirectory makes one at a time)
        const std::wstring w = text::widen(dir);
        for (std::size_t i = 3; i <= w.size(); ++i) {
            if (i == w.size() || w[i] == L'\\') { ::CreateDirectoryW(w.substr(0, i).c_str(), nullptr); }
        }
    }
    return dir;
}

void clear_archive_cache() { remove_tree(text::widen(cache_root())); }

ArchiveOps::~ArchiveOps()
{
    quit_ = true;
    for (std::thread& t : threads_) { if (t.joinable()) { t.join(); } }
}

void ArchiveOps::spawn(std::function<Result()> job)
{
    ++running_;
    const std::lock_guard lock{lock_};
    threads_.emplace_back([this, job = std::move(job)] {
        Result r = job();
        {
            const std::lock_guard l{lock_};
            results_.push_back(std::move(r));
        }
        --running_;
        if (wake_) { wake_(); }
    });
}

bool ArchiveOps::poll(Result& out)
{
    const std::lock_guard lock{lock_};
    if (results_.empty()) { return false; }
    out = std::move(results_.front());
    results_.erase(results_.begin());
    return true;
}

void ArchiveOps::extract(std::string archive, std::string dest_dir, bool make_folder, std::string tag)
{
    spawn([this, archive = std::move(archive), dest_dir = std::move(dest_dir), make_folder, tag = std::move(tag)]() -> Result {
        Result r;
        r.dir = dest_dir;
        r.tag = tag;
        std::string target = dest_dir;
        if (make_folder) {
            r.made = unique_name(dest_dir, archive_stem(path::leaf(archive)), "");
            target = path::join(dest_dir, r.made);
            if (!::CreateDirectoryW(text::widen(target).c_str(), nullptr)) {
                r.message = "Could not create the folder " + r.made;
                r.made.clear();
                return r;
            }
        }
        const ProcessResult p = run_process((L"\"" + tar_exe() + L"\"") + L" -xf " + quote_arg(archive) + L" -C " + quote_arg(target), text::widen(dest_dir), 30 * 60 * 1000, &quit_);
        r.ok = p.started && !p.timed_out && p.exit_code == 0;
        if (r.ok) { r.message = make_folder ? "Extracted to " + r.made : "Extracted " + path::leaf(archive); }
        else { r.message = p.started ? "Could not extract: " + reason(p.output) : "tar.exe was not found (needs Windows 10 or newer)"; }
        return r;
    });
}

void ArchiveOps::compress(std::string dir, std::vector<std::string> names, std::string zip_name)
{
    spawn([this, dir = std::move(dir), names = std::move(names), zip_name = std::move(zip_name)]() -> Result {
        Result r;
        r.dir  = dir;
        r.made = unique_name(dir, zip_name, ".zip");
        std::wstring line = (L"\"" + tar_exe() + L"\"") + L" -a -cf " + quote_arg(r.made);
        for (const std::string& n : names) { line += L" " + quote_arg(n); }
        const ProcessResult p = run_process(line, text::widen(dir), 30 * 60 * 1000, &quit_);
        r.ok = p.started && !p.timed_out && p.exit_code == 0;
        if (r.ok) { r.message = "Created " + r.made; }
        else {
            r.message = p.started ? "Could not compress: " + reason(p.output) : "tar.exe was not found (needs Windows 10 or newer)";
            ::DeleteFileW(text::widen(path::join(dir, r.made)).c_str());
            r.made.clear();
        }
        return r;
    });
}

} // namespace prism::core
