#include "core/i18n.hpp"

#include "core/text.hpp"

#include <windows.h>

#include <algorithm>
#include <memory>
#include <unordered_map>
#include <vector>

namespace prism::core::i18n {

namespace {

struct StringHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
};

enum class Plural { one_other, none, french, slavic };

struct Catalog {
    std::unordered_map<std::string, std::string, StringHash, std::equal_to<>> map;
    Plural plural{Plural::one_other};
};

std::vector<LanguageInfo>             g_languages{{"en", "English", {}}};
std::unique_ptr<Catalog>              g_catalog; // null: English
std::string                           g_code{"en"};
unsigned                              g_revision{0};
// views handed out by tr() point into a catalog; a switch mid-frame must not free the one in use
std::vector<std::unique_ptr<Catalog>> g_retired;

std::string read_file(const std::string& path)
{
    std::string out;
    const HANDLE h = ::CreateFileW(core::text::widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { return out; }
    LARGE_INTEGER size{};
    if (::GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart < (16 << 20)) {
        out.resize(static_cast<std::size_t>(size.QuadPart));
        DWORD got = 0;
        if (!::ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr)) { got = 0; }
        out.resize(got);
    }
    ::CloseHandle(h);
    if (out.starts_with("\xEF\xBB\xBF")) { out.erase(0, 3); }
    return out;
}

std::string_view trim(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) { s.remove_prefix(1); }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) { s.remove_suffix(1); }
    return s;
}

// "text" with \" \\ \n \t escapes, starting at s[0]; leaves `s` after the closing quote
bool parse_quoted(std::string_view& s, std::string& out)
{
    s = trim(s);
    if (s.empty() || s.front() != '"') { return false; }
    out.clear();
    for (std::size_t i = 1; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '"') { s.remove_prefix(i + 1); return true; }
        if (c == '\\' && i + 1 < s.size()) {
            const char e = s[++i];
            out += e == 'n' ? '\n' : e == 't' ? '\t' : e;
        } else {
            out += c;
        }
    }
    return false;
}

// the display name of a file: its "@name = ..." line (cheap, without loading the rest)
std::string file_name_of(const std::string& text)
{
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string::npos) { end = text.size(); }
        const std::string_view line = trim(std::string_view{text}.substr(pos, end - pos));
        if (line.starts_with("@name")) {
            const std::size_t eq = line.find('=');
            if (eq != std::string_view::npos) { return std::string{trim(line.substr(eq + 1))}; }
        }
        pos = end + 1;
    }
    return {};
}

// number of std::format placeholders, -1 when the braces are malformed or an explicit index is >= `limit`
int placeholders(std::string_view s, int limit)
{
    int count = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '{') {
            if (i + 1 < s.size() && s[i + 1] == '{') { ++i; continue; }
            const std::size_t close = s.find('}', i);
            if (close == std::string_view::npos) { return -1; }
            std::string_view field = s.substr(i + 1, close - i - 1);
            field = field.substr(0, field.find(':'));
            if (!field.empty()) {
                if (field.size() > 3 || !std::ranges::all_of(field, [](char c) { return c >= '0' && c <= '9'; })) { return -1; }
                if (std::stoi(std::string{field}) >= limit) { return -1; }
            }
            ++count;
            i = close;
        } else if (s[i] == '}') {
            if (i + 1 < s.size() && s[i + 1] == '}') { ++i; continue; }
            return -1;
        }
    }
    return count;
}

// a translation of a key with placeholders must be usable in the same format call
bool placeholders_match(std::string_view key, std::string_view value)
{
    if (key.find_first_of("{}") == std::string_view::npos) { return true; }
    const std::size_t bar = key.find('|'); // plural: "one|other", the forms are checked one by one
    const int want = placeholders(key.substr(0, bar), 1000);
    if (want < 0) { return true; }          // (the English text itself is not a format string)
    for (std::size_t from = 0;;) {
        const std::size_t to = bar == std::string_view::npos ? std::string_view::npos : value.find('|', from);
        if (placeholders(value.substr(from, to == std::string_view::npos ? to : to - from), want) != want) { return false; }
        if (to == std::string_view::npos) { return true; }
        from = to + 1;
    }
}

std::unique_ptr<Catalog> parse(const std::string& text)
{
    auto cat = std::make_unique<Catalog>();
    std::size_t pos = 0;
    std::string key, value;
    while (pos < text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string::npos) { end = text.size(); }
        std::string_view line = trim(std::string_view{text}.substr(pos, end - pos));
        pos = end + 1;
        if (line.empty() || line.front() == '#') { continue; }
        if (line.front() == '@') {
            const std::size_t eq = line.find('=');
            if (eq != std::string_view::npos && trim(line.substr(0, eq)) == "@plural") {
                const std::string_view v = trim(line.substr(eq + 1));
                cat->plural = v == "none" ? Plural::none : v == "french" ? Plural::french : v == "slavic" ? Plural::slavic : Plural::one_other;
            }
            continue;
        }
        if (!parse_quoted(line, key)) { continue; }
        line = trim(line);
        if (line.empty() || line.front() != '=') { continue; }
        line.remove_prefix(1);
        if (!parse_quoted(line, value) || value.empty()) { continue; } // empty = not translated yet
        if (!placeholders_match(key, value)) { continue; }
        core::text::note_codepoints(value); // (the host rebuilds the fonts when a language brings new glyphs)
        cat->map[key] = value;
    }
    return cat;
}

bool load(const LanguageInfo& info)
{
    if (info.path.empty()) { return false; }
    const std::string text = read_file(info.path);
    if (text.empty()) { return false; }
    if (g_catalog) { g_retired.push_back(std::move(g_catalog)); }
    g_catalog = parse(text);
    core::text::note_codepoints(info.name);
    return true;
}

std::size_t plural_index(Plural rule, long long n)
{
    const long long a = n < 0 ? -n : n;
    switch (rule) {
    case Plural::none:   return 0;
    case Plural::french: return a < 2 ? 0 : 1;
    case Plural::slavic: {
        if (a % 10 == 1 && a % 100 != 11) { return 0; }
        if (a % 10 >= 2 && a % 10 <= 4 && (a % 100 < 12 || a % 100 > 14)) { return 1; }
        return 2;
    }
    default: return a == 1 ? 0 : 1;
    }
}

// the `index`-th '|' separated form (the last one when the file lists fewer)
std::string_view nth_form(std::string_view forms, std::size_t index)
{
    for (std::size_t i = 0; i < index; ++i) {
        const std::size_t bar = forms.find('|');
        if (bar == std::string_view::npos) { break; }
        forms.remove_prefix(bar + 1);
    }
    return forms.substr(0, forms.find('|'));
}

} // namespace

void scan(std::span<const std::string> dirs)
{
    g_languages.assign(1, LanguageInfo{"en", "English", {}});
    for (const std::string& dir : dirs) {
        WIN32_FIND_DATAW fd{};
        const HANDLE h = ::FindFirstFileW(core::text::widen(dir + "\\*.lang").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) { continue; }
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) { continue; }
            std::string file = core::text::narrow(fd.cFileName);
            std::string code = core::text::lower_ascii(file.substr(0, file.size() - 5));
            if (code.empty() || code == "en" || code == "template") { continue; }
            const std::string path = dir + "\\" + file;
            std::string name = file_name_of(read_file(path));
            if (name.empty()) { name = code; }
            const auto it = std::ranges::find(g_languages, code, &LanguageInfo::code);
            if (it != g_languages.end()) { it->name = name; it->path = path; }
            else { g_languages.push_back({code, name, path}); }
        } while (::FindNextFileW(h, &fd));
        ::FindClose(h);
    }
    std::sort(g_languages.begin() + 1, g_languages.end(), [](const LanguageInfo& a, const LanguageInfo& b) { return a.name < b.name; });
}

std::span<const LanguageInfo> languages() { return g_languages; }

std::string_view set_language(std::string_view code)
{
    ++g_revision;
    const auto it = std::ranges::find(g_languages, code, &LanguageInfo::code);
    if (it != g_languages.end() && load(*it)) {
        g_code = it->code;
        return g_code;
    }
    if (g_catalog) { g_retired.push_back(std::move(g_catalog)); }
    g_code = "en";
    return g_code;
}

std::string_view reload()
{
    const std::string code = g_code;
    return set_language(code);
}

std::string_view language() { return g_code; }

unsigned revision() noexcept { return g_revision; }

std::string_view tr(std::string_view english)
{
    if (!g_catalog) { return english; }
    const auto it = g_catalog->map.find(english);
    return it == g_catalog->map.end() ? english : std::string_view{it->second};
}

std::string_view tr_n(std::string_view one, std::string_view other, long long n)
{
    if (g_catalog) {
        std::string key;
        key.reserve(one.size() + other.size() + 1);
        key.append(one).append("|").append(other);
        if (const auto it = g_catalog->map.find(key); it != g_catalog->map.end()) {
            return nth_form(it->second, plural_index(g_catalog->plural, n));
        }
    }
    return n == 1 ? one : other;
}

} // namespace prism::core::i18n
