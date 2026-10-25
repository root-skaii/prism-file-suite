#pragma once

// utf-8 windows paths with backslashes: "C:\Users\me", drive roots keep the slash ("C:\")

#include <string>
#include <string_view>
#include <vector>

namespace prism::core::path {

[[nodiscard]] bool is_root(std::string_view p) noexcept;
// "C:\a\b" -> "C:\a"; a root has no parent (returns empty)
[[nodiscard]] std::string parent(std::string_view p);
// last component: "C:\a\b" -> "b", "C:\" -> "C:\"
[[nodiscard]] std::string leaf(std::string_view p);
[[nodiscard]] std::string join(std::string_view dir, std::string_view name);
// forward slashes -> backslashes, trailing slash dropped (except on a root), "C:" -> "C:\"
[[nodiscard]] std::string normalize(std::string_view p);

struct Segment {
    std::string name; // what a breadcrumb shows
    std::string path; // where it goes
};
// "C:\a\b" -> [C:\ , a, b]
[[nodiscard]] std::vector<Segment> segments(std::string_view p);

// names of the visible subfolders of `dir`, naturally sorted, at most `limit` (breadcrumb dropdowns; local folders only
// in practice: it is a synchronous listing)
[[nodiscard]] std::vector<std::string> subfolders(const std::string& dir, std::size_t limit);

[[nodiscard]] bool is_directory(const std::string& p);
[[nodiscard]] bool exists(const std::string& p);

// "1.4 MB" style; folders show nothing
enum class Kind { missing, folder, file };
// what is at `p`: one attribute lookup, which can block for seconds on an unreachable network host (workers only for typed paths)
[[nodiscard]] Kind kind(const std::string& p);

[[nodiscard]] std::string format_size(unsigned long long bytes);
// "2026-10-18 14:32" in local time from a FILETIME tick count (0: empty)
[[nodiscard]] std::string format_time(unsigned long long filetime);

} // namespace prism::core::path
