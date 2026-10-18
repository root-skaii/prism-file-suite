#pragma once

// the shell's well-known folders and the drives, for the sidebar

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace prism::core {

struct Place {
    std::string   name;   // "Downloads", "Windows (C:)"
    std::string   path;
    std::uint64_t total{}; // drives: capacity and free bytes (0 = unknown)
    std::uint64_t free{};
    bool          removable{};
};

// the well-known folders the sidebar starts with, by token ("home", "desktop", "downloads" ...)
struct KnownFolder {
    std::string_view token;
    std::string_view name;
};
[[nodiscard]] std::vector<KnownFolder> known_folders();
// where a token points on this machine ("" for an unknown token or a folder that is not there)
[[nodiscard]] std::string known_path(std::string_view token);

// Home, Desktop, Downloads, Documents, Pictures, Music, Videos (only the ones that exist)
[[nodiscard]] std::vector<Place> quick_access();
// fixed, removable and network drives that are ready
[[nodiscard]] std::vector<Place> drives();
[[nodiscard]] std::string        home_folder();

} // namespace prism::core
