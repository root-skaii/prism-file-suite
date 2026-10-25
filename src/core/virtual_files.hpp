#pragma once

// files a drag offers without having them on disk (MobaXterm's SFTP browser, mail clients, browsers ...): a file group
// descriptor plus the contents of each file on request. They are copied into a staging folder, then handled like dropped paths

#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct IDataObject;

namespace prism::core {

struct ReceivedFiles {
    std::vector<std::string> items;  // the top-level files and folders now in the staging folder
    std::size_t              failed{}; // entries that could not be read or written (the rest is still delivered)
};

// the data object carries a file group descriptor (and no real paths)
[[nodiscard]] bool offers_virtual_files(IDataObject* data);

// copies the offered files into a new folder below `staging_root` (created if needed). Blocks while the source delivers the
// contents: call it where the drop is handled, not per frame
[[nodiscard]] ReceivedFiles receive_virtual_files(IDataObject* data, std::string_view staging_root);

// an entry name from a descriptor as a safe relative path ("dir\file"), or nullopt when it could leave the staging folder
// (absolute, drive, "..") or is not a valid Windows name
[[nodiscard]] std::optional<std::string> safe_relative_path(std::wstring_view name);

} // namespace prism::core
