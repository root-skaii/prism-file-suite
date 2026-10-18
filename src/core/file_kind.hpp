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

// "PNG File", "Folder", "Text Document": what explorer's Type column shows (no shell lookup, so it is instant)
[[nodiscard]] std::string type_label(FileKind kind, std::string_view ext);

} // namespace prism::core
