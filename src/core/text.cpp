#include "core/text.hpp"

#include <windows.h>

#include "strata/font.hpp"

namespace prism::core::text {

std::wstring widen(std::string_view s)
{
    std::wstring w;
    if (s.empty()) { return w; }
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n > 0) {
        w.resize(static_cast<std::size_t>(n));
        ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    }
    return w;
}

std::string narrow(std::wstring_view w)
{
    std::string s;
    if (w.empty()) { return s; }
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n > 0) {
        s.resize(static_cast<std::size_t>(n));
        ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    }
    return s;
}

namespace {

[[nodiscard]] constexpr bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

[[nodiscard]] char32_t fold(char32_t c) noexcept
{
    if (c < 0x80) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
    if (c < 0x10000) { return static_cast<char32_t>(reinterpret_cast<std::uintptr_t>(::CharLowerW(reinterpret_cast<wchar_t*>(static_cast<std::uintptr_t>(c))))); }
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

std::string lower_ascii(std::string_view s)
{
    std::string out{s};
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c + 32); }
    }
    return out;
}

} // namespace prism::core::text
