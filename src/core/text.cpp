#include "core/text.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <cwctype>
#endif

#include "strata/font.hpp"

#include <algorithm>

namespace prism::core::text {

std::wstring widen(std::string_view s)
{
    std::wstring w;
    if (s.empty()) { return w; }
#ifdef _WIN32
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n > 0) {
        w.resize(static_cast<std::size_t>(n));
        ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    }
#else
    std::string_view cur = s;
    w.reserve(s.size());
    while (!cur.empty()) { w.push_back(static_cast<wchar_t>(strata::decode_utf8(cur))); }
#endif
    return w;
}

std::string narrow(std::wstring_view w)
{
    std::string s;
    if (w.empty()) { return s; }
#ifdef _WIN32
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n > 0) {
        s.resize(static_cast<std::size_t>(n));
        ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    }
#else
    s.reserve(w.size());
    for (const wchar_t wc : w) {
        const auto cp = static_cast<std::uint32_t>(wc);
        if (cp <= 0x7f) { s.push_back(static_cast<char>(cp)); }
        else if (cp <= 0x7ff) { s.push_back(static_cast<char>(0xc0 | (cp >> 6))); s.push_back(static_cast<char>(0x80 | (cp & 0x3f))); }
        else if (cp <= 0xffff) { s.push_back(static_cast<char>(0xe0 | (cp >> 12))); s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f))); s.push_back(static_cast<char>(0x80 | (cp & 0x3f))); }
        else { s.push_back(static_cast<char>(0xf0 | (cp >> 18))); s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f))); s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f))); s.push_back(static_cast<char>(0x80 | (cp & 0x3f))); }
    }
#endif
    return s;
}

namespace {

[[nodiscard]] constexpr bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

[[nodiscard]] char32_t fold(char32_t c) noexcept
{
    if (c < 0x80) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
#ifdef _WIN32
    if (c < 0x10000) { return static_cast<char32_t>(reinterpret_cast<std::uintptr_t>(::CharLowerW(reinterpret_cast<wchar_t*>(static_cast<std::uintptr_t>(c))))); }
#else
    return static_cast<char32_t>(std::towlower(static_cast<wint_t>(c)));
#endif
    return c;
}

} // namespace

int compare_natural(std::string_view a, std::string_view b) noexcept
{
    while (!a.empty() && !b.empty()) {
        if (is_digit(a.front()) && is_digit(b.front())) {
            std::size_t i = 0;
            std::size_t j = 0;
            while (i < a.size() && a[i] == '0') { ++i; }
            while (j < b.size() && b[j] == '0') { ++j; }
            std::size_t ie = i;
            std::size_t je = j;
            while (ie < a.size() && is_digit(a[ie])) { ++ie; }
            while (je < b.size() && is_digit(b[je])) { ++je; }
            const std::size_t la = ie - i;
            const std::size_t lb = je - j;
            if (la != lb) { return la < lb ? -1 : 1; }
            if (const int c = a.substr(i, la).compare(b.substr(j, lb)); c != 0) { return c < 0 ? -1 : 1; }
            // equal values: the longer run of leading zeros sorts later, so the order is total
            std::size_t ra = 0;
            std::size_t rb = 0;
            while (ra < a.size() && is_digit(a[ra])) { ++ra; }
            while (rb < b.size() && is_digit(b[rb])) { ++rb; }
            if (ra != rb) { return ra < rb ? -1 : 1; }
            a.remove_prefix(ra);
            b.remove_prefix(rb);
            continue;
        }
        const char32_t ca = fold(strata::decode_utf8(a));
        const char32_t cb = fold(strata::decode_utf8(b));
        if (ca != cb) { return ca < cb ? -1 : 1; }
    }
    if (a.empty() && b.empty()) { return 0; }
    return a.empty() ? -1 : 1;
}

int compare_plain(std::string_view a, std::string_view b) noexcept
{
    while (!a.empty() && !b.empty()) {
        const char32_t ca = fold(strata::decode_utf8(a));
        const char32_t cb = fold(strata::decode_utf8(b));
        if (ca != cb) { return ca < cb ? -1 : 1; }
    }
    if (a.empty() && b.empty()) { return 0; }
    return a.empty() ? -1 : 1;
}

namespace {

std::vector<char32_t> g_demand;
unsigned              g_revision = 0;

// what the ui fonts bake without being asked (see ui/fonts.cpp)
[[nodiscard]] bool baked_by_default(char32_t c) noexcept
{
    return c < 0x250 || (c >= 0x370 && c <= 0x3ff) || (c >= 0x400 && c <= 0x52f) || (c >= 0x2000 && c <= 0x206f) ||
           (c >= 0x20a0 && c <= 0x20cf) || (c >= 0x2190 && c <= 0x22ff) || (c >= 0x2500 && c <= 0x25ff) || (c >= 0xe000 && c <= 0xf8ff);
}

} // namespace

void note_codepoints(std::string_view s)
{
    while (!s.empty()) {
        if (static_cast<unsigned char>(s.front()) < 0x80) { s.remove_prefix(1); continue; }
        const char32_t c = strata::decode_utf8(s);
        if (baked_by_default(c) || c == 0xfffd) { continue; }
        const auto it = std::lower_bound(g_demand.begin(), g_demand.end(), c);
        if (it == g_demand.end() || *it != c) {
            g_demand.insert(it, c);
            ++g_revision;
        }
    }
}

const std::vector<char32_t>& demanded_codepoints() noexcept { return g_demand; }
unsigned demand_revision() noexcept { return g_revision; }

std::string lower_ascii(std::string_view s)
{
    std::string out{s};
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c + 32); }
    }
    return out;
}

bool starts_with_icase(std::string_view text, std::string_view prefix) noexcept
{
    while (!prefix.empty()) {
        if (text.empty()) { return false; }
        const char32_t ct = fold(strata::decode_utf8(text));
        const char32_t cp = fold(strata::decode_utf8(prefix));
        if (ct != cp) { return false; }
    }
    return true;
}

} // namespace prism::core::text
