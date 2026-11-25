#pragma once

// lightweight, exception-free syntax tokenizer and hex formatter for Quick Look and Preview Pane.
// runs once when a file is loaded (never per frame).

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace prism::core {

enum class TokenKind : std::uint8_t {
    plain,
    keyword,
    type,
    string,
    comment,
    number,
    preproc,
};

struct TokenSpan {
    std::uint32_t start{};
    std::uint32_t length{};
    TokenKind     kind{TokenKind::plain};
};

struct HighlightedLine {
    std::string            text;
    std::vector<TokenSpan> spans;
};

enum class SyntaxLang : std::uint8_t {
    none,
    c_like, // C, C++, C#, Java, JS/TS, Rust, Go, HLSL, GLSL, CSS
    hash,   // Python, PowerShell, Bash, YAML, TOML, INI, CMake, Makefile
    json,   // JSON / JSONC
    markup, // HTML, XML, SVG, XAML
};

[[nodiscard]] SyntaxLang syntax_for_extension(std::string_view lower_ext) noexcept;
// tokenizes `lines` in one pass (tracking multi-line block comments across lines)
[[nodiscard]] std::vector<HighlightedLine> highlight_lines(std::vector<std::string> lines, SyntaxLang lang);
// formats `bytes` as 16-byte-per-row hex + printable ASCII lines ("00000000  4D 5A ...  |MZ...|")
[[nodiscard]] std::vector<std::string> format_hex_dump(std::span<const unsigned char> bytes, std::size_t max_rows = 64);

} // namespace prism::core
