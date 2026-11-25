#pragma once

// the Recycle Bin as plain folders: each drive keeps `$Recycle.Bin\<your SID>` holding `$R<id>.ext` (the item) and
// `$I<id>.ext` (a record with the original path and the time it was deleted)

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace prism::core::recycle {

// the virtual folder that lists every drive's bin together (its items are named by their full paths)
inline constexpr std::string_view all = "prism:RecycleBin"; // ("::" paths are portable devices, "\\" ones are shares)

[[nodiscard]] bool inside(std::string_view path); // `path` is a Recycle Bin folder (or `all`) or below one
// this user's existing bin folders on every fixed / removable drive
[[nodiscard]] std::vector<std::string> folders();

struct Record {
    std::string   original;  // the full path it was deleted from
    std::uint64_t deleted{}; // FILETIME
    std::string   info;      // the `$I` file
};
// the record of a top-level `$R` item (nullopt for anything else, or an unreadable record)
[[nodiscard]] std::optional<Record> read(const std::string& item);

// this user's Recycle Bin folder on a drive (`drive_root` "D:\"; empty = the system drive); "" while nothing was ever deleted there
[[nodiscard]] std::string folder(std::string_view drive_root = {});
// where it is, or will be (it is made when something is first deleted)
[[nodiscard]] std::string folder_path(std::string_view drive_root = {});
// "C:\" for a path on a lettered drive, else ""
[[nodiscard]] std::string drive_of(std::string_view path);

// puts the item back where it was deleted from (a number is added when something is there now). false: `error` says why
[[nodiscard]] bool restore(const std::string& item, std::string& error);
// deletes the item for good, with its record
[[nodiscard]] bool erase(const std::string& item);
// drops the record of an item that was deleted from the bin by other means
void forget(const std::string& item);
// empties one drive's bin (`drive_root` "D:\") or, when empty, every drive's, through the shell (it asks first). blocks: worker threads only
[[nodiscard]] bool empty(const std::string& drive_root = {}, void* owner_hwnd = nullptr);

} // namespace prism::core::recycle
