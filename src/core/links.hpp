#pragma once

// symbolic links, hard links and junctions

#include <string>

namespace prism::core {

enum class LinkKind { symbolic, hard, junction };

// makes `link` point at `target` (both full paths; `target_is_dir` matters for symbolic links). returns "" on success, else
// a sentence for a toast. a symbolic link needs Developer Mode or an elevated process; a hard link only works for files on
// the same volume; a junction only for folders
[[nodiscard]] std::string make_link(LinkKind kind, const std::string& target, bool target_is_dir, const std::string& link);

// removes a link made by make_link (never what it points at); false if `link` is not a link or could not be removed
bool remove_link(const std::string& link);

} // namespace prism::core
