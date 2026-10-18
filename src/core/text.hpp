#pragma once

// utf-8 <-> utf-16 (win32) and the name comparison the file list sorts by

#include <string>
#include <string_view>
#include <vector>

namespace prism::core::text {

[[nodiscard]] std::wstring widen(std::string_view utf8);
[[nodiscard]] std::string  narrow(std::wstring_view utf16);

// case-insensitive, digit runs compared as numbers ("file2" < "file10"): the order explorer shows
[[nodiscard]] int compare_natural(std::string_view a, std::string_view b) noexcept;

// the characters names use that the ui fonts do not bake at start (latin, greek, cyrillic, common symbols). the file
// listing reports every such character here (ui thread); the host rebuilds the fonts with exactly these glyphs, so
// a folder of chinese names costs a few hundred glyphs, not the 21k of the whole han block
void note_codepoints(std::string_view utf8);
[[nodiscard]] const std::vector<char32_t>& demanded_codepoints() noexcept; // sorted, unique
[[nodiscard]] unsigned demand_revision() noexcept;                         // changes when the set grows

// lower-cases ascii only: extensions and quick-find prefixes
[[nodiscard]] std::string lower_ascii(std::string_view s);

} // namespace prism::core::text
