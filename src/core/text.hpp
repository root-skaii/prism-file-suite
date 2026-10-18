#pragma once

// utf-8 <-> utf-16 (win32) and the name comparison the file list sorts by

#include <string>
#include <string_view>

namespace prism::core::text {

[[nodiscard]] std::wstring widen(std::string_view utf8);
[[nodiscard]] std::string  narrow(std::wstring_view utf16);

// case-insensitive, digit runs compared as numbers ("file2" < "file10"): the order explorer shows
[[nodiscard]] int compare_natural(std::string_view a, std::string_view b) noexcept;

// lower-cases ascii only: extensions and quick-find prefixes
[[nodiscard]] std::string lower_ascii(std::string_view s);

} // namespace prism::core::text
