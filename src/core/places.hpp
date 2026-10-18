#pragma once

// the shell's well-known folders and the drives, for the sidebar

#include <cstdint>
#include <string>
#include <vector>

namespace prism::core {

struct Place {
    std::string   name;   // "Downloads", "Windows (C:)"
    std::string   path;
    std::uint64_t total{}; // drives: capacity and free bytes (0 = unknown)
    std::uint64_t free{};
    bool          removable{};
};

// Home, Desktop, Downloads, Documents, Pictures, Music, Videos (only the ones that exist)
[[nodiscard]] std::vector<Place> quick_access();
// fixed, removable and network drives that are ready
[[nodiscard]] std::vector<Place> drives();
[[nodiscard]] std::string        home_folder();

} // namespace prism::core
