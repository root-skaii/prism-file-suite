#pragma once

#include "core/file_kind.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace prism::core {

// win32 attribute bits that matter here (windows.h stays out of headers)
namespace attr {
inline constexpr std::uint32_t readonly = 0x1;
inline constexpr std::uint32_t hidden   = 0x2;
inline constexpr std::uint32_t system   = 0x4;
inline constexpr std::uint32_t directory = 0x10;
inline constexpr std::uint32_t reparse  = 0x400; // symlink / junction / cloud placeholder
} // namespace attr

// colour labels: a file can carry any of these (bit i of FileEntry::tags / model::Tags masks)
namespace tag {
inline constexpr int count = 8;
inline constexpr std::string_view names[count] = {"red", "orange", "yellow", "green", "teal", "blue", "purple", "pink"};
inline constexpr std::uint16_t all = (1u << count) - 1u;
} // namespace tag

// one directory entry. a few dozen bytes + the name: a 200k-file folder stays under ~20 MB
struct FileEntry {
    std::string   name;            // utf-8
    std::uint64_t size{};          // bytes; 0 for folders
    std::uint64_t modified{};      // FILETIME: 100 ns ticks since 1601
    std::uint32_t attributes{};
    std::uint16_t ext_offset{};    // where the extension starts in `name` (== size: none)
    FileKind      kind{FileKind::other};
    std::uint16_t tags{};          // colour labels (core::tag bits); the listing fills it from the tag store, not the loader

    [[nodiscard]] bool is_dir() const noexcept { return (attributes & attr::directory) != 0; }
    [[nodiscard]] bool is_hidden() const noexcept { return (attributes & (attr::hidden | attr::system)) != 0; }
    [[nodiscard]] std::string_view ext() const noexcept { return std::string_view{name}.substr(ext_offset); }
};

} // namespace prism::core
