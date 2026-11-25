#pragma once

#include "core/file_kind.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace prism::core {

// win32 attribute bits that matter here (windows.h stays out of headers)
namespace attr {
inline constexpr std::uint32_t readonly  = 0x1;
inline constexpr std::uint32_t hidden    = 0x2;
inline constexpr std::uint32_t system    = 0x4;
inline constexpr std::uint32_t directory = 0x10;
inline constexpr std::uint32_t reparse   = 0x400; // symlink / junction / cloud placeholder
} // namespace attr

// colour labels: a file can carry any of these (bit i of FileEntry::tags / model::Tags masks)
namespace tag {
inline constexpr int count = 8;
inline constexpr std::string_view names[count] = {"red", "orange", "yellow", "green", "teal", "blue", "purple", "pink"};
inline constexpr std::uint16_t all = (1u << count) - 1u;
} // namespace tag

// Recycle Bin metadata: kept out-of-line so normal FileEntry stays compact (~72 B instead of ~128 B)
struct RecycleInfo {
    std::string shown;    // display name (the name it was deleted under)
    std::string original; // full path it was deleted from
};

// lightweight non-owning view used by Query::matches (lets FileIndex match millions of pool slices with zero allocations)
struct FileEntryView {
    std::string_view name;
    std::string_view display;
    std::string_view ext;
    std::uint64_t    size{};
    std::uint64_t    modified{};
    std::uint32_t    attributes{};
    FileKind         kind{FileKind::other};
    std::uint16_t    tags{};

    [[nodiscard]] bool is_dir() const noexcept { return (attributes & attr::directory) != 0; }
    [[nodiscard]] bool is_hidden() const noexcept { return (attributes & (attr::hidden | attr::system)) != 0; }
};

// copyable 8-byte unique pointer for optional Recycle Bin info
struct RecycleBox {
    std::unique_ptr<RecycleInfo> ptr;

    RecycleBox() = default;
    RecycleBox(RecycleBox&&) noexcept = default;
    RecycleBox& operator=(RecycleBox&&) noexcept = default;
    RecycleBox(const RecycleBox& o) : ptr{o.ptr ? std::make_unique<RecycleInfo>(*o.ptr) : nullptr} {}
    RecycleBox& operator=(const RecycleBox& o)
    {
        if (this != &o) { ptr = o.ptr ? std::make_unique<RecycleInfo>(*o.ptr) : nullptr; }
        return *this;
    }
    [[nodiscard]] explicit operator bool() const noexcept { return ptr != nullptr; }
    [[nodiscard]] const RecycleInfo* operator->() const noexcept { return ptr.get(); }
    [[nodiscard]] RecycleInfo*       operator->() noexcept { return ptr.get(); }
};

// one directory entry (~72 B + name): keeps 200k-file folders compact and cache-friendly
struct FileEntry {
    std::string   name;            // utf-8
    std::uint64_t size{};          // bytes; 0 for folders
    std::uint64_t modified{};      // FILETIME: 100 ns ticks since 1601
    std::uint32_t attributes{};
    std::uint16_t ext_offset{};    // where the extension starts in `name` (== size: none)
    std::uint16_t tags{};          // colour labels (core::tag bits); the listing fills it from the tag store, not the loader
    FileKind      kind{FileKind::other};
    RecycleBox    recycle;         // non-null only for Recycle Bin items

    [[nodiscard]] std::string_view display() const noexcept
    {
        return recycle && !recycle->shown.empty() ? std::string_view{recycle->shown} : std::string_view{name};
    }
    [[nodiscard]] std::string_view original_path() const noexcept
    {
        return recycle ? std::string_view{recycle->original} : std::string_view{};
    }
    [[nodiscard]] bool has_display_override() const noexcept { return recycle && !recycle->shown.empty(); }
    void set_recycle(std::string shown, std::string original)
    {
        if (!recycle.ptr) { recycle.ptr = std::make_unique<RecycleInfo>(); }
        recycle->shown    = std::move(shown);
        recycle->original = std::move(original);
    }
    void set_shown(std::string shown)
    {
        if (!recycle.ptr) { recycle.ptr = std::make_unique<RecycleInfo>(); }
        recycle->shown = std::move(shown);
    }
    [[nodiscard]] bool is_dir() const noexcept { return (attributes & attr::directory) != 0; }
    [[nodiscard]] bool is_hidden() const noexcept { return (attributes & (attr::hidden | attr::system)) != 0; }
    [[nodiscard]] std::string_view ext() const noexcept { return std::string_view{name}.substr(ext_offset); }
    [[nodiscard]] FileEntryView view() const noexcept
    {
        return {name, display(), ext(), size, modified, attributes, kind, tags};
    }
};

} // namespace prism::core
