#pragma once

// Cloud storage integration via rclone: supports both in-process `librclone.dll` (c-shared library)
// and portable `rclone.exe` (`rclone rcd` local HTTP daemon via WinHTTP).
// Virtual paths use the form: `rclone:<remote>:` (root) or `rclone:<remote>:\Folder\File.txt`.

#include "core/file_entry.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace prism::core::rclone {

struct Remote {
    std::string name; // e.g. "gdrive"
    std::string type; // e.g. "drive", "onedrive", "dropbox", "sftp"
};

struct SplitPath {
    std::string remote;   // remote name without colon (e.g. "gdrive")
    std::string rel_unix; // relative path with forward slashes, no leading slash ("" at root)
};

[[nodiscard]] bool      is_path(std::string_view path) noexcept;
[[nodiscard]] SplitPath split(std::string_view path);
[[nodiscard]] std::string make_root(std::string_view remote);
[[nodiscard]] std::string normalize_path(std::string_view path);

// backend availability (librclone.dll beside exe / PATH, or rclone.exe beside exe / PATH)
[[nodiscard]] bool        available();
[[nodiscard]] std::string backend_name(); // "librclone.dll", "rclone.exe", or ""

// lists configured remotes (worker threads only)
[[nodiscard]] std::vector<Remote> list_remotes();
// storage quota for a remote (worker threads only); returns false if unsupported or unavailable
[[nodiscard]] bool about(const std::string& remote, std::uint64_t& total, std::uint64_t& free);
// lists one directory on a remote (worker threads only)
[[nodiscard]] bool list_dir(const std::string& rclone_path, std::vector<FileEntry>& out, std::string& error);

// file & folder operations (worker threads only)
[[nodiscard]] bool download_to_cache(const std::string& rclone_path, std::string& local_path, std::string& error);
[[nodiscard]] bool copy_or_move(bool move, const std::string& src, const std::string& dst_dir, std::string& error);
[[nodiscard]] bool mkdir(const std::string& rclone_path, std::string& error);
[[nodiscard]] bool rename(const std::string& rclone_path, const std::string& new_name, std::string& error);
[[nodiscard]] bool remove(const std::string& rclone_path, std::string& error);

// launches interactive Google Drive OAuth setup via rclone.exe (opens browser); returns false with message if unavailable
[[nodiscard]] bool connect_google_drive(std::string& message);

// shuts down any running rclone daemon / library cleanly on app exit
void shutdown();

} // namespace prism::core::rclone
