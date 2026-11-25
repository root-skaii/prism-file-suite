#include "core/path.hpp"

#include "core/rclone.hpp"
#include "core/text.hpp"

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#else
#include <cstdlib>
#include <ctime>
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cstdio>

namespace prism::core::path {

namespace {

[[nodiscard]] bool is_unc_root(std::string_view p) noexcept
{
    if (p.size() < 5 || p[0] != '\\' || p[1] != '\\') { return false; }
    if (p.back() == '\\') { p.remove_suffix(1); }
    const std::size_t s = p.find('\\', 2);
    if (s == std::string_view::npos || s == 2 || s + 1 >= p.size()) { return false; }
    return p.find('\\', s + 1) == std::string_view::npos;
}

} // namespace

std::wstring long_wide(std::string_view utf8)
{
    std::wstring w = text::widen(utf8);
#ifdef _WIN32
    if (w.size() < 248 || w.starts_with(L"\\\\?\\")) { return w; }
    for (wchar_t& c : w) {
        if (c == L'/') { c = L'\\'; }
    }
    if (w.starts_with(L"\\\\")) { return L"\\\\?\\UNC\\" + w.substr(2); }
    return L"\\\\?\\" + w;
#else
    return w;
#endif
}

bool is_link_reparse_tag(unsigned long tag) noexcept
{
#ifdef _WIN32
    return tag == IO_REPARSE_TAG_SYMLINK || tag == IO_REPARSE_TAG_MOUNT_POINT;
#else
    return tag != 0;
#endif
}

bool is_reparse_link(const std::string& p)
{
#ifdef _WIN32
    WIN32_FIND_DATAW fd{};
    const std::string clean = normalize(p);
    if (clean.empty() || is_root(clean)) { return false; }
    HANDLE h = ::FindFirstFileExW(long_wide(clean).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) { return false; }
    ::FindClose(h);
    return (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 && is_link_reparse_tag(fd.dwReserved0);
#else
    struct stat st{};
    return ::lstat(p.c_str(), &st) == 0 && S_ISLNK(st.st_mode);
#endif
}

bool is_unc_server(std::string_view p) noexcept
{
    if (p.size() < 3 || p[0] != '\\' || p[1] != '\\') { return false; }
    if (p.back() == '\\') { p.remove_suffix(1); }
    return p.size() >= 3 && p.find('\\', 2) == std::string_view::npos;
}

bool is_root(std::string_view p) noexcept
{
    if (rclone::is_path(p)) {
        const std::size_t colon = p.find(':', 7);
        if (colon == std::string_view::npos) { return true; }
        std::string_view rest = p.substr(colon + 1);
        while (!rest.empty() && (rest.front() == '\\' || rest.front() == '/')) { rest.remove_prefix(1); }
        return rest.empty();
    }
#ifndef _WIN32
    if (p == "/") { return true; }
#endif
    return (p.size() == 3 && p[1] == ':' && p[2] == '\\') || (p.size() == 2 && p[1] == ':') || is_unc_root(p) || is_unc_server(p);
}

bool same_volume(const std::string& a, const std::string& b)
{
    if (rclone::is_path(a) || rclone::is_path(b)) {
        if (!rclone::is_path(a) || !rclone::is_path(b)) { return false; }
        return text::lower_ascii(rclone::split(a).remote) == text::lower_ascii(rclone::split(b).remote);
    }
#ifdef _WIN32
    wchar_t ra[MAX_PATH]{}, rb[MAX_PATH]{};
    if (::GetVolumePathNameW(long_wide(a).c_str(), ra, MAX_PATH) && ::GetVolumePathNameW(long_wide(b).c_str(), rb, MAX_PATH)) {
        return ::_wcsicmp(ra, rb) == 0;
    }
    // fallback for UNC shares when GetVolumePathNameW fails: compare \\server\share
    const auto unc_share = [](std::string_view p) -> std::string {
        const std::string n = normalize(p);
        if (!n.starts_with("\\\\")) { return {}; }
        const std::size_t s1 = n.find('\\', 2);
        if (s1 == std::string::npos) { return {}; }
        const std::size_t s2 = n.find('\\', s1 + 1);
        return text::lower_ascii(s2 == std::string::npos ? std::string_view{n} : std::string_view{n}.substr(0, s2));
    };
    const std::string ua = unc_share(a);
    const std::string ub = unc_share(b);
    return !ua.empty() && ua == ub;
#else
    struct stat sa{}, sb{};
    if (::stat(a.c_str(), &sa) == 0 && ::stat(b.c_str(), &sb) == 0) { return sa.st_dev == sb.st_dev; }
    return false;
#endif
}

std::string parent(std::string_view p)
{
    if (p.empty() || is_root(p)) { return {}; }
    if (rclone::is_path(p)) {
        const std::string norm = rclone::normalize_path(p);
        const std::size_t cut = norm.rfind('\\');
        return cut == std::string::npos ? std::string{} : norm.substr(0, cut);
    }
#ifndef _WIN32
    while (p.size() > 1 && p.back() == '/') { p.remove_suffix(1); }
    if (p == "/") { return {}; }
    const std::size_t slash = p.rfind('/');
    if (slash == std::string_view::npos) { return {}; }
    return slash == 0 ? std::string{"/"} : std::string{p.substr(0, slash)};
#else
    if (p.size() > 1 && p.back() == '\\') { p.remove_suffix(1); }
    if (is_root(p)) { return {}; }
    const std::size_t cut = p.rfind('\\');
    if (cut == std::string_view::npos || cut <= 1) { return {}; }
    if (cut == 2 && p[1] == ':') { return std::string{p.substr(0, 3)}; } // "C:\x" -> "C:\"
    return std::string{p.substr(0, cut)};
#endif
}

std::string leaf(std::string_view p)
{
    if (rclone::is_path(p)) {
        const rclone::SplitPath sp = rclone::split(p);
        if (sp.rel_unix.empty()) { return sp.remote; }
        const std::size_t sep = sp.rel_unix.rfind('/');
        return sep == std::string::npos ? sp.rel_unix : sp.rel_unix.substr(sep + 1);
    }
    if (is_root(p)) { return std::string{p}; }
#ifndef _WIN32
    while (p.size() > 1 && p.back() == '/') { p.remove_suffix(1); }
    if (p == "/") { return "/"; }
    const std::size_t cut = p.rfind('/');
    return std::string{cut == std::string_view::npos ? p : p.substr(cut + 1)};
#else
    while (p.size() > 1 && p.back() == '\\') { p.remove_suffix(1); }
    if (is_root(p)) { return std::string{p}; }
    const std::size_t cut = p.rfind('\\');
    return std::string{cut == std::string_view::npos ? p : p.substr(cut + 1)};
#endif
}

std::string join(std::string_view dir, std::string_view name)
{
    // (a listing that mixes folders, like the all-bins view, names its entries by full path)
    if ((name.size() >= 3 && name[1] == ':' && name[2] == '\\' && name[0] != ':') || name.starts_with("\\\\") || rclone::is_path(name)) { return std::string{name}; }
    if (rclone::is_path(dir)) {
        std::string r = rclone::normalize_path(dir);
        if (!r.empty() && r.back() != '\\') { r.push_back('\\'); }
        r += name;
        return rclone::normalize_path(r);
    }
#ifndef _WIN32
    if (!name.empty() && name.front() == '/') { return std::string{name}; }
    std::string r{dir};
    if (!r.empty() && r.back() != '/') { r.push_back('/'); }
    r += name;
    return r;
#else
    std::string r{dir};
    if (!r.empty() && r.back() != '\\') { r.push_back('\\'); }
    r += name;
    return r;
#endif
}

std::string normalize(std::string_view p)
{
    if (rclone::is_path(p)) { return rclone::normalize_path(p); }
    std::string r{p};
#ifndef _WIN32
    for (char& c : r) { if (c == '\\') { c = '/'; } }
    while (r.size() > 1 && r.back() == '/') { r.pop_back(); }
    return r;
#else
    for (char& c : r) {
        if (c == '/') { c = '\\'; }
    }
    while (r.size() > 3 && r.back() == '\\') { r.pop_back(); }
    if (r.size() == 2 && r[1] == ':') { r.push_back('\\'); }
    return r;
#endif
}

std::string resolve_input(std::string_view raw)
{
    while (!raw.empty() && (raw.front() == ' ' || raw.front() == '\t')) { raw.remove_prefix(1); }
    while (!raw.empty() && (raw.back() == ' ' || raw.back() == '\t')) { raw.remove_suffix(1); }
    if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"') {
        raw.remove_prefix(1);
        raw.remove_suffix(1);
    }
    if (raw.empty()) { return {}; }
    if (raw.starts_with("prism:") || raw.starts_with("::")) { return std::string{raw}; }
    if (rclone::is_path(raw)) { return rclone::normalize_path(raw); }

    std::string s{raw};
#ifdef _WIN32
    // expand %ENV% variables (e.g. %APPDATA%, %TEMP%, %USERPROFILE%)
    if (s.find('%') != std::string::npos) {
        const std::wstring w = text::widen(s);
        const DWORD needed = ::ExpandEnvironmentStringsW(w.c_str(), nullptr, 0);
        if (needed > 1 && needed < 32768) {
            std::wstring expanded(needed, L'\0');
            const DWORD got = ::ExpandEnvironmentStringsW(w.c_str(), expanded.data(), needed);
            if (got > 1 && got <= needed) {
                expanded.resize(got - 1);
                s = text::narrow(expanded);
            }
        }
    }

    // resolve shell: URIs (e.g. shell:startup, shell:sendto, shell:downloads)
    if (s.size() > 6 && text::lower_ascii(std::string_view{s}.substr(0, 6)) == "shell:") {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(::SHParseDisplayName(text::widen(s).c_str(), nullptr, &pidl, 0, nullptr)) && pidl != nullptr) {
            PWSTR fs = nullptr;
            if (SUCCEEDED(::SHGetNameFromIDList(pidl, SIGDN_FILESYSPATH, &fs)) && fs != nullptr) {
                s = text::narrow(fs);
                ::CoTaskMemFree(fs);
            }
            ::CoTaskMemFree(pidl);
        }
    }

    s = normalize(s);
    // resolve relative paths (".", "..", "sub\dir") and ".." segments to full paths
    const bool looks_abs = (s.size() >= 2 && s[1] == ':') || s.starts_with("\\\\");
    const bool has_rel   = s == "." || s == ".." || s.starts_with(".\\") || s.starts_with("..\\") || s.find("\\.\\") != std::string::npos || s.find("\\..\\") != std::string::npos || s.ends_with("\\.") || s.ends_with("\\..");
    if ((!looks_abs && (s.find('\\') != std::string::npos || s == "." || s == "..")) || has_rel) {
        const std::wstring w = text::widen(s);
        const DWORD needed = ::GetFullPathNameW(w.c_str(), 0, nullptr, nullptr);
        if (needed > 1 && needed < 32768) {
            std::wstring full(needed, L'\0');
            const DWORD got = ::GetFullPathNameW(w.c_str(), needed, full.data(), nullptr);
            if (got > 0 && got < needed) {
                full.resize(got);
                s = normalize(text::narrow(full));
            }
        }
    }
#else
    if (s == "~" || s.starts_with("~/")) {
        if (const char* home = std::getenv("HOME")) { s = std::string{home} + s.substr(1); }
    }
    s = normalize(s);
    char resolved[PATH_MAX]{};
    if (::realpath(s.c_str(), resolved) != nullptr) { s = normalize(resolved); }
#endif
    return s;
}

std::vector<Segment> segments(std::string_view p)
{
    std::vector<Segment> out;
    if (rclone::is_path(p)) {
        const rclone::SplitPath sp = rclone::split(p);
        if (sp.remote.empty()) { return out; }
        std::string acc = rclone::make_root(sp.remote);
        out.push_back({sp.remote, acc});
        std::size_t pos = 0;
        while (pos < sp.rel_unix.size()) {
            std::size_t next = sp.rel_unix.find('/', pos);
            if (next == std::string::npos) { next = sp.rel_unix.size(); }
            if (next > pos) {
                const std::string part = sp.rel_unix.substr(pos, next - pos);
                acc = join(acc, part);
                out.push_back({part, acc});
            }
            pos = next + 1;
        }
        return out;
    }
    std::string          acc;
    std::size_t          i = 0;
#ifndef _WIN32
    if (!p.empty() && p.front() == '/') {
        acc = "/";
        out.push_back({"/", "/"});
        i = 1;
        while (i < p.size()) {
            std::size_t e = p.find('/', i);
            if (e == std::string_view::npos) { e = p.size(); }
            if (e > i) {
                acc = join(acc, p.substr(i, e - i));
                out.push_back({std::string{p.substr(i, e - i)}, acc});
            }
            i = e + 1;
        }
        return out;
    }
#endif
    if (p.size() >= 2 && p[1] == ':') { // drive
        acc = std::string{p.substr(0, 2)} + "\\";
        out.push_back({acc, acc});
        i = p.size() >= 3 ? 3 : 2;
    } else if (p.starts_with("\\\\")) { // UNC share ("\\server\share") or bare server ("\\server")
        const std::size_t s1 = p.find('\\', 2);
        if (s1 == std::string_view::npos) {
            if (p.size() > 2) {
                acc = std::string{p};
                out.push_back({acc, acc});
                i = p.size();
            }
        } else if (s1 > 2) {
            const std::size_t s2 = p.find('\\', s1 + 1);
            const std::size_t end = s2 == std::string_view::npos ? p.size() : s2;
            if (end > s1 + 1) {
                acc = std::string{p.substr(0, end)};
                out.push_back({acc, acc});
                i = s2 == std::string_view::npos ? p.size() : s2 + 1;
            }
        }
    }
    while (i < p.size()) {
        std::size_t e = p.find('\\', i);
        if (e == std::string_view::npos) { e = p.size(); }
        if (e > i) {
            acc = join(acc, p.substr(i, e - i));
            out.push_back({std::string{p.substr(i, e - i)}, acc});
        }
        i = e + 1;
    }
    return out;
}

bool is_directory(const std::string& p)
{
    if (rclone::is_path(p)) { return true; }
#ifdef _WIN32
    const DWORD a = ::GetFileAttributesW(long_wide(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat st{};
    return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

Kind kind(const std::string& p)
{
    if (is_unc_server(p) || rclone::is_path(p)) { return Kind::folder; }
#ifdef _WIN32
    const DWORD a = ::GetFileAttributesW(long_wide(p).c_str());
    if (a == INVALID_FILE_ATTRIBUTES) { return Kind::missing; }
    return (a & FILE_ATTRIBUTE_DIRECTORY) != 0 ? Kind::folder : Kind::file;
#else
    struct stat st{};
    if (::stat(p.c_str(), &st) != 0) { return Kind::missing; }
    return S_ISDIR(st.st_mode) ? Kind::folder : Kind::file;
#endif
}

bool exists(const std::string& p)
{
#ifdef _WIN32
    return ::GetFileAttributesW(long_wide(p).c_str()) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st{};
    return ::lstat(p.c_str(), &st) == 0;
#endif
}

std::string format_size(unsigned long long bytes)
{
    static constexpr const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(bytes);
    int    u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    char buf[32];
    if (u == 0) {
        std::snprintf(buf, sizeof buf, "%llu B", bytes);
    } else {
        std::snprintf(buf, sizeof buf, v < 10.0 ? "%.2f %s" : (v < 100.0 ? "%.1f %s" : "%.0f %s"), v, units[u]);
    }
    return buf;
}

std::string format_time(unsigned long long filetime)
{
    if (filetime == 0) { return {}; }
#ifdef _WIN32
    FILETIME ft{static_cast<DWORD>(filetime), static_cast<DWORD>(filetime >> 32)};
    FILETIME local{};
    SYSTEMTIME st{};
    if (!::FileTimeToLocalFileTime(&ft, &local) || !::FileTimeToSystemTime(&local, &st)) { return {}; }
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return buf;
#else
    constexpr unsigned long long k_epoch_diff = 11644473600ULL;
    const unsigned long long secs = filetime / 10000000ULL;
    if (secs < k_epoch_diff) { return {}; }
    const std::time_t tt = static_cast<std::time_t>(secs - k_epoch_diff);
    std::tm tm{};
    if (::localtime_r(&tt, &tm) == nullptr) { return {}; }
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
    return buf;
#endif
}

std::vector<std::string> subfolders(const std::string& dir, std::size_t limit)
{
    std::vector<std::string> out;
#ifdef _WIN32
    WIN32_FIND_DATAW         fd{};
    const std::wstring       pattern = long_wide(join(dir, "*"));
    const HANDLE h = ::FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchLimitToDirectories, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) { return out; }
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 || (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0) { continue; }
        if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == 0 || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0))) { continue; }
        out.push_back(text::narrow(fd.cFileName));
    } while (out.size() < limit && ::FindNextFileW(h, &fd));
    ::FindClose(h);
#else
    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr) { return out; }
    while (out.size() < limit) {
        const dirent* ent = ::readdir(d);
        if (ent == nullptr) { break; }
        if (ent->d_name[0] == '.') { continue; }
        const std::string full = join(dir, ent->d_name);
        struct stat st{};
        if (::stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) { out.emplace_back(ent->d_name); }
    }
    ::closedir(d);
#endif
    std::ranges::sort(out, [](const std::string& a, const std::string& b) { return text::compare_natural(a, b) < 0; });
    return out;
}

} // namespace prism::core::path
