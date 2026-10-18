#include "core/path.hpp"

#include "core/text.hpp"

#include <windows.h>

#include <cstdio>

namespace prism::core::path {

bool is_root(std::string_view p) noexcept
{
    return (p.size() == 3 && p[1] == ':' && p[2] == '\\') || (p.size() == 2 && p[1] == ':');
}

std::string parent(std::string_view p)
{
    if (p.empty() || is_root(p)) { return {}; }
    const std::size_t cut = p.rfind('\\');
    if (cut == std::string_view::npos) { return {}; }
    if (cut == 2 && p[1] == ':') { return std::string{p.substr(0, 3)}; } // "C:\x" -> "C:\"
    return std::string{p.substr(0, cut)};
}

std::string leaf(std::string_view p)
{
    if (is_root(p)) { return std::string{p}; }
    const std::size_t cut = p.rfind('\\');
    return std::string{cut == std::string_view::npos ? p : p.substr(cut + 1)};
}

std::string join(std::string_view dir, std::string_view name)
{
    std::string r{dir};
    if (!r.empty() && r.back() != '\\') { r.push_back('\\'); }
    r += name;
    return r;
}

std::string normalize(std::string_view p)
{
    std::string r{p};
    for (char& c : r) {
        if (c == '/') { c = '\\'; }
    }
    while (r.size() > 3 && r.back() == '\\') { r.pop_back(); }
    if (r.size() == 2 && r[1] == ':') { r.push_back('\\'); }
    return r;
}

std::vector<Segment> segments(std::string_view p)
{
    std::vector<Segment> out;
    std::string          acc;
    std::size_t          i = 0;
    if (p.size() >= 2 && p[1] == ':') { // drive
        acc = std::string{p.substr(0, 2)} + "\\";
        out.push_back({acc, acc});
        i = p.size() >= 3 ? 3 : 2;
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
    const DWORD a = ::GetFileAttributesW(text::widen(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool exists(const std::string& p)
{
    return ::GetFileAttributesW(text::widen(p).c_str()) != INVALID_FILE_ATTRIBUTES;
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
    FILETIME ft{static_cast<DWORD>(filetime), static_cast<DWORD>(filetime >> 32)};
    FILETIME local{};
    SYSTEMTIME st{};
    if (!::FileTimeToLocalFileTime(&ft, &local) || !::FileTimeToSystemTime(&local, &st)) { return {}; }
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return buf;
}

} // namespace prism::core::path
