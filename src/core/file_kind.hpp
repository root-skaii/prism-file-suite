#pragma once

// what a file is, as far as colour, icon and the "Type" column go

#include <cstdint>
#include <string>
#include <string_view>

namespace prism::core {

enum class FileKind : std::uint8_t {
    folder, image, video, audio, archive, document, spreadsheet, presentation, pdf, code, text, executable, library,
    config, font, disk_image, shortcut, drive, other,
    count_
};

// `ext` is lower-case without the dot ("" = none)
[[nodiscard]] FileKind classify_extension(std::string_view ext) noexcept;

// a file the shell lets you drop other files on to run it with them as arguments (programs, batch files, shortcuts). only a
// candidate: the shell's own drop handler for the file has the last word
[[nodiscard]] bool is_program_name(std::string_view name) noexcept;

// "PNG File", "Folder", "Text Document": what explorer's Type column shows (no shell lookup, so it is instant)
[[nodiscard]] std::string type_label(FileKind kind, std::string_view ext);

} // namespace prism::core
