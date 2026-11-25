#include "core/syntax.hpp"

#include <algorithm>
#include <array>
#include <cstdio>

namespace prism::core {

namespace {

[[nodiscard]] bool is_ident_start(char c) noexcept
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

[[nodiscard]] bool is_ident_char(char c) noexcept
{
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

[[nodiscard]] bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

[[nodiscard]] bool is_hex(char c) noexcept
{
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

[[nodiscard]] bool in_sorted(std::span<const std::string_view> list, std::string_view word) noexcept
{
    return std::binary_search(list.begin(), list.end(), word);
}

[[nodiscard]] TokenKind classify_c_word(std::string_view w) noexcept
{
    static constexpr std::array<std::string_view, 71> k_keywords = {
        "alignas", "alignof", "as", "async", "auto", "await", "break", "case", "catch", "class",
        "const", "consteval", "constexpr", "constinit", "continue", "co_await", "co_return", "co_yield",
        "default", "defer", "delete", "do", "else", "enum", "explicit", "export", "extern", "false",
        "final", "fn", "for", "friend", "func", "function", "goto", "if", "impl", "import", "in",
        "inline", "interface", "let", "match", "module", "mut", "namespace", "new", "noexcept",
        "null", "nullptr", "operator", "override", "package", "private", "protected", "pub", "public",
        "return", "sizeof", "static", "struct", "switch", "template", "this", "throw", "trait",
        "true", "try", "typedef", "typename", "using",
    };
    static constexpr std::array<std::string_view, 32> k_types = {
        "BOOL", "DWORD", "HANDLE", "HRESULT", "HWND", "bool", "char", "char16_t", "char32_t",
        "double", "f32", "f64", "float", "i16", "i32", "i64", "i8", "int", "int16_t", "int32_t",
        "int64_t", "int8_t", "long", "short", "size_t", "string", "u16", "u32", "u64", "u8",
        "unsigned", "void",
    };
    if (in_sorted(k_keywords, w)) { return TokenKind::keyword; }
    if (in_sorted(k_types, w)) { return TokenKind::type; }
    if (w == "virtual" || w == "volatile" || w == "while" || w == "yield" || w == "var" || w == "self" || w == "super") {
        return TokenKind::keyword;
    }
    if (w == "wchar_t" || w == "uint8_t" || w == "uint16_t" || w == "uint32_t" || w == "uint64_t" || w == "std") {
        return TokenKind::type;
    }
    return TokenKind::plain;
}

[[nodiscard]] TokenKind classify_hash_word(std::string_view w) noexcept
{
    static constexpr std::array<std::string_view, 38> k_keywords = {
        "False", "None", "True", "and", "as", "assert", "async", "await", "break", "class",
        "continue", "def", "del", "elif", "else", "endif", "endforeach", "endfunction", "except",
        "false", "finally", "for", "foreach", "from", "function", "if", "import", "in", "is",
        "lambda", "not", "null", "or", "param", "pass", "return", "true", "while",
    };
    if (in_sorted(k_keywords, w)) { return TokenKind::keyword; }
    if (w == "try" || w == "with" || w == "yield" || w == "set" || w == "option" || w == "project" || w == "add_executable" || w == "add_library" || w == "target_link_libraries") {
        return TokenKind::keyword;
    }
    return TokenKind::plain;
}

void push_span(std::vector<TokenSpan>& out, std::size_t start, std::size_t len, TokenKind kind)
{
    if (len == 0) { return; }
    if (!out.empty() && out.back().kind == kind && out.back().start + out.back().length == start) {
        out.back().length += static_cast<std::uint32_t>(len);
        return;
    }
    out.push_back({static_cast<std::uint32_t>(start), static_cast<std::uint32_t>(len), kind});
}

void tokenize_c_like(std::string_view line, bool& in_block_comment, std::vector<TokenSpan>& out)
{
    const std::size_t n = line.size();
    std::size_t       i = 0;

    // preprocessor line: "#include ...", "#define ..."
    if (!in_block_comment) {
        std::size_t lead = 0;
        while (lead < n && (line[lead] == ' ' || line[lead] == '\t')) { ++lead; }
        if (lead < n && line[lead] == '#') {
            std::size_t end = lead + 1;
            while (end < n && (line[end] == ' ' || line[end] == '\t')) { ++end; }
            while (end < n && is_ident_char(line[end])) { ++end; }
            push_span(out, 0, lead, TokenKind::plain);
            push_span(out, lead, end - lead, TokenKind::preproc);
            i = end;
        }
    }

    while (i < n) {
        if (in_block_comment) {
            const std::size_t end = line.find("*/", i);
            if (end == std::string_view::npos) {
                push_span(out, i, n - i, TokenKind::comment);
                return;
            }
            push_span(out, i, end + 2 - i, TokenKind::comment);
            i = end + 2;
            in_block_comment = false;
            continue;
        }
        const char c = line[i];
        if (c == '/' && i + 1 < n && line[i + 1] == '/') {
            push_span(out, i, n - i, TokenKind::comment);
            return;
        }
        if (c == '/' && i + 1 < n && line[i + 1] == '*') {
            in_block_comment = true;
            const std::size_t end = line.find("*/", i + 2);
            if (end == std::string_view::npos) {
                push_span(out, i, n - i, TokenKind::comment);
                return;
            }
            push_span(out, i, end + 2 - i, TokenKind::comment);
            i = end + 2;
            in_block_comment = false;
            continue;
        }
        if (c == '"' || c == '\'') {
            const std::size_t start = i++;
            while (i < n) {
                if (line[i] == '\\' && i + 1 < n) { i += 2; continue; }
                if (line[i] == c) { ++i; break; }
                ++i;
            }
            push_span(out, start, i - start, TokenKind::string);
            continue;
        }
        if (is_digit(c)) {
            const std::size_t start = i++;
            if (c == '0' && i < n && (line[i] == 'x' || line[i] == 'X')) {
                ++i;
                while (i < n && (is_hex(line[i]) || line[i] == '\'' || line[i] == '_')) { ++i; }
            } else {
                while (i < n && (is_digit(line[i]) || line[i] == '.' || line[i] == '\'' || line[i] == '_')) { ++i; }
            }
            while (i < n && (line[i] == 'u' || line[i] == 'U' || line[i] == 'l' || line[i] == 'L' || line[i] == 'f' || line[i] == 'F')) { ++i; }
            push_span(out, start, i - start, TokenKind::number);
            continue;
        }
        if (is_ident_start(c)) {
            const std::size_t start = i++;
            while (i < n && is_ident_char(line[i])) { ++i; }
            push_span(out, start, i - start, classify_c_word(line.substr(start, i - start)));
            continue;
        }
        push_span(out, i, 1, TokenKind::plain);
        ++i;
    }
}

void tokenize_hash(std::string_view line, std::vector<TokenSpan>& out)
{
    const std::size_t n = line.size();
    std::size_t       i = 0;
    while (i < n && (line[i] == ' ' || line[i] == '\t')) { ++i; }
    if (i > 0) { push_span(out, 0, i, TokenKind::plain); }
    if (i < n && line[i] == '[') { // INI / TOML section header
        const std::size_t close = line.find(']', i + 1);
        if (close != std::string_view::npos) {
            push_span(out, i, close + 1 - i, TokenKind::preproc);
            i = close + 1;
        }
    }
    while (i < n) {
        const char c = line[i];
        if (c == '#' || (c == ';' && i == 0)) {
            push_span(out, i, n - i, TokenKind::comment);
            return;
        }
        if (c == '"' || c == '\'') {
            const std::size_t start = i++;
            while (i < n) {
                if (line[i] == '\\' && i + 1 < n) { i += 2; continue; }
                if (line[i] == c) { ++i; break; }
                ++i;
            }
            push_span(out, start, i - start, TokenKind::string);
            continue;
        }
        if (is_digit(c)) {
            const std::size_t start = i++;
            while (i < n && (is_hex(line[i]) || line[i] == '.' || line[i] == 'x' || line[i] == 'X' || line[i] == '_')) { ++i; }
            push_span(out, start, i - start, TokenKind::number);
            continue;
        }
        if (is_ident_start(c)) {
            const std::size_t start = i++;
            while (i < n && is_ident_char(line[i])) { ++i; }
            push_span(out, start, i - start, classify_hash_word(line.substr(start, i - start)));
            continue;
        }
        push_span(out, i, 1, TokenKind::plain);
        ++i;
    }
}

void tokenize_json(std::string_view line, std::vector<TokenSpan>& out)
{
    const std::size_t n = line.size();
    std::size_t       i = 0;
    while (i < n) {
        const char c = line[i];
        if (c == '/' && i + 1 < n && line[i + 1] == '/') {
            push_span(out, i, n - i, TokenKind::comment);
            return;
        }
        if (c == '"') {
            const std::size_t start = i++;
            while (i < n) {
                if (line[i] == '\\' && i + 1 < n) { i += 2; continue; }
                if (line[i] == '"') { ++i; break; }
                ++i;
            }
            std::size_t after = i;
            while (after < n && (line[after] == ' ' || line[after] == '\t')) { ++after; }
            const bool is_key = after < n && line[after] == ':';
            push_span(out, start, i - start, is_key ? TokenKind::keyword : TokenKind::string);
            continue;
        }
        if (is_digit(c) || (c == '-' && i + 1 < n && is_digit(line[i + 1]))) {
            const std::size_t start = i++;
            while (i < n && (is_digit(line[i]) || line[i] == '.' || line[i] == 'e' || line[i] == 'E' || line[i] == '+' || line[i] == '-')) { ++i; }
            push_span(out, start, i - start, TokenKind::number);
            continue;
        }
        if (is_ident_start(c)) {
            const std::size_t start = i++;
            while (i < n && is_ident_char(line[i])) { ++i; }
            const std::string_view w = line.substr(start, i - start);
            push_span(out, start, i - start, (w == "true" || w == "false" || w == "null") ? TokenKind::type : TokenKind::plain);
            continue;
        }
        push_span(out, i, 1, TokenKind::plain);
        ++i;
    }
}

void tokenize_markup(std::string_view line, bool& in_comment, std::vector<TokenSpan>& out)
{
    const std::size_t n = line.size();
    std::size_t       i = 0;
    while (i < n) {
        if (in_comment) {
            const std::size_t end = line.find("-->", i);
            if (end == std::string_view::npos) {
                push_span(out, i, n - i, TokenKind::comment);
                return;
            }
            push_span(out, i, end + 3 - i, TokenKind::comment);
            i = end + 3;
            in_comment = false;
            continue;
        }
        if (line.substr(i).starts_with("<!--")) {
            in_comment = true;
            const std::size_t end = line.find("-->", i + 4);
            if (end == std::string_view::npos) {
                push_span(out, i, n - i, TokenKind::comment);
                return;
            }
            push_span(out, i, end + 3 - i, TokenKind::comment);
            i = end + 3;
            in_comment = false;
            continue;
        }
        if (line[i] == '<') {
            const std::size_t start = i++;
            if (i < n && (line[i] == '/' || line[i] == '?' || line[i] == '!')) { ++i; }
            while (i < n && (is_ident_char(line[i]) || line[i] == '-' || line[i] == ':')) { ++i; }
            push_span(out, start, i - start, TokenKind::keyword);
            continue;
        }
        if (line[i] == '"' || line[i] == '\'') {
            const char q = line[i];
            const std::size_t start = i++;
            while (i < n && line[i] != q) { ++i; }
            if (i < n) { ++i; }
            push_span(out, start, i - start, TokenKind::string);
            continue;
        }
        push_span(out, i, 1, TokenKind::plain);
        ++i;
    }
}

} // namespace

SyntaxLang syntax_for_extension(std::string_view ext) noexcept
{
    if (ext == "c" || ext == "cc" || ext == "cpp" || ext == "cxx" || ext == "h" || ext == "hh" || ext == "hpp" || ext == "hxx" ||
        ext == "cs" || ext == "java" || ext == "js" || ext == "jsx" || ext == "mjs" || ext == "ts" || ext == "tsx" ||
        ext == "rs" || ext == "go" || ext == "swift" || ext == "kt" || ext == "hlsl" || ext == "glsl" || ext == "css" || ext == "scss") {
        return SyntaxLang::c_like;
    }
    if (ext == "py" || ext == "ps1" || ext == "psm1" || ext == "sh" || ext == "bash" || ext == "zsh" ||
        ext == "yml" || ext == "yaml" || ext == "toml" || ext == "ini" || ext == "cfg" || ext == "conf" ||
        ext == "cmake" || ext == "rb" || ext == "lua" || ext == "lang" || ext == "gitignore") {
        return SyntaxLang::hash;
    }
    if (ext == "json" || ext == "jsonc") { return SyntaxLang::json; }
    if (ext == "xml" || ext == "html" || ext == "htm" || ext == "svg" || ext == "xaml" || ext == "manifest" || ext == "csproj" || ext == "vcxproj") {
        return SyntaxLang::markup;
    }
    return SyntaxLang::none;
}

std::vector<HighlightedLine> highlight_lines(std::vector<std::string> lines, SyntaxLang lang)
{
    std::vector<HighlightedLine> out;
    out.reserve(lines.size());
    bool in_block = false;
    for (std::string& line : lines) {
        HighlightedLine hl;
        hl.text = std::move(line);
        switch (lang) {
        case SyntaxLang::c_like: tokenize_c_like(hl.text, in_block, hl.spans); break;
        case SyntaxLang::hash:   tokenize_hash(hl.text, hl.spans); break;
        case SyntaxLang::json:   tokenize_json(hl.text, hl.spans); break;
        case SyntaxLang::markup: tokenize_markup(hl.text, in_block, hl.spans); break;
        case SyntaxLang::none:   push_span(hl.spans, 0, hl.text.size(), TokenKind::plain); break;
        }
        out.push_back(std::move(hl));
    }
    return out;
}

std::vector<std::string> format_hex_dump(std::span<const unsigned char> bytes, std::size_t max_rows)
{
    static constexpr char hex[] = "0123456789ABCDEF";
    std::vector<std::string> out;
    const std::size_t rows = std::min(max_rows, (bytes.size() + 15) / 16);
    out.reserve(rows);
    for (std::size_t r = 0; r < rows; ++r) {
        const std::size_t off = r * 16;
        std::string       line;
        line.reserve(78);
        char addr[12];
        std::snprintf(addr, sizeof addr, "%08X  ", static_cast<unsigned>(off));
        line += addr;
        for (std::size_t c = 0; c < 16; ++c) {
            if (c == 8) { line.push_back(' '); }
            if (off + c < bytes.size()) {
                const unsigned char b = bytes[off + c];
                line.push_back(hex[b >> 4]);
                line.push_back(hex[b & 15]);
                line.push_back(' ');
            } else {
                line += "   ";
            }
        }
        line += " |";
        for (std::size_t c = 0; c < 16 && off + c < bytes.size(); ++c) {
            const unsigned char b = bytes[off + c];
            line.push_back(b >= 0x20 && b <= 0x7e ? static_cast<char>(b) : '.');
        }
        line.push_back('|');
        out.push_back(std::move(line));
    }
    return out;
}

} // namespace prism::core
