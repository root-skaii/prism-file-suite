#include "ui/fonts.hpp"

#include <windows.h>

#include "core/log.hpp"
#include "core/text.hpp"
#include "ui/settings.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace prism::ui {

namespace {

[[nodiscard]] bool system_font_exists(const char* file_name)
{
    wchar_t dir[MAX_PATH]{};
    if (::GetWindowsDirectoryW(dir, MAX_PATH) == 0) { return false; }
    std::wstring path = std::wstring{dir} + L"\\Fonts\\";
    for (const char* c = file_name; *c != '\0'; ++c) { path.push_back(static_cast<wchar_t>(c[0])); }
    return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// the baked set: the defaults plus exactly the extra characters the names on screen use. a context keeps pointing at
// what it was built from (it rebakes the atlas on scale changes), so these live for the rest of the process: one small
// leak per rebuild, and rebuilds only happen when a new character shows up
[[nodiscard]] const std::vector<strata::codepoint_range>& ranges_for(const std::vector<char32_t>& extra)
{
    using namespace strata;
    auto* v = new std::vector<codepoint_range>{glyph_ranges::latin, glyph_ranges::greek, glyph_ranges::cyrillic,
                                               glyph_ranges::punctuation, glyph_ranges::currency, glyph_ranges::arrows,
                                               glyph_ranges::math_operators, glyph_ranges::box_and_shapes};
    bool arabic = false;
    bool kana   = false;
    for (std::size_t i = 0; i < extra.size();) {
        std::size_t j = i;
        while (j + 1 < extra.size() && extra[j + 1] == extra[j] + 1) { ++j; } // neighbours join one range
        v->push_back({extra[i], extra[j]});
        for (std::size_t k = i; k <= j; ++k) {
            const char32_t c = extra[k];
            arabic |= (c >= 0x600 && c <= 0x6ff) || (c >= 0x750 && c <= 0x77f);
            kana   |= c >= 0x3040 && c <= 0x30ff;
        }
        i = j + 1;
    }
    // arabic is shaped from its presentation forms; kana come in whole blocks (small)
    if (arabic) {
        v->push_back(glyph_ranges::arabic);
        v->push_back(glyph_ranges::arabic_supplement);
        v->push_back(glyph_ranges::arabic_forms_a);
        v->push_back(glyph_ranges::arabic_forms_b);
    }
    if (kana) {
        v->push_back(glyph_ranges::hiragana);
        v->push_back(glyph_ranges::katakana);
    }
    return *v;
}

// --- baked atlas cache: rasterising the fonts is most of the start-up time, and the result only changes when the inputs do ---

constexpr std::size_t k_cache_keep = 6; // atlases kept in the cache folder (one per scale / glyph set seen lately)

[[nodiscard]] std::string file_stamp(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) { return "-;"; }
    return std::format("{}:{}:{};", d.nFileSizeLow, d.ftLastWriteTime.dwHighDateTime, d.ftLastWriteTime.dwLowDateTime);
}

// everything the raster depends on: the glyph set and scale, the font files, the gdi smoothing settings and this exe
// (the sizes and ranges are code, so a new build starts a new cache)
[[nodiscard]] std::string atlas_key(float scale, const std::vector<char32_t>& extra, std::string_view text_face, std::string_view icon_face)
{
    static constexpr std::array<const wchar_t*, 12> files{L"SegUIVar.ttf", L"segoeui.ttf", L"segoeuib.ttf", L"SegoeIcons.ttf", L"segmdl2.ttf", L"consola.ttf",
                                                          L"msyh.ttc", L"YuGothR.ttc", L"malgun.ttf", L"seguisym.ttf", L"seguiemj.ttf", L"LeelawUI.ttf"};
    std::string key = std::format("v1;{};{};{};", scale, text_face, icon_face);
    for (const char32_t c : extra) { key += std::format("{:x},", static_cast<std::uint32_t>(c)); }
    wchar_t dir[MAX_PATH]{};
    if (::GetWindowsDirectoryW(dir, MAX_PATH) != 0) {
        for (const wchar_t* f : files) { key += file_stamp(std::wstring{dir} + L"\\Fonts\\" + f); }
    }
    UINT smoothing = 0, smoothing_type = 0, contrast = 0;
    ::SystemParametersInfoW(SPI_GETFONTSMOOTHING, 0, &smoothing, 0);
    ::SystemParametersInfoW(SPI_GETFONTSMOOTHINGTYPE, 0, &smoothing_type, 0);
    ::SystemParametersInfoW(SPI_GETFONTSMOOTHINGCONTRAST, 0, &contrast, 0);
    key += std::format("{},{},{};", smoothing, smoothing_type, contrast);
    wchar_t exe[MAX_PATH]{};
    if (::GetModuleFileNameW(nullptr, exe, MAX_PATH) != 0) { key += file_stamp(exe); }
    return key;
}

[[nodiscard]] std::wstring cache_file(const std::string& key)
{
    std::uint64_t h = 1469598103934665603ull; // fnv-1a
    for (const char c : key) { h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ull; }
    return core::text::widen(Settings::cache_path()) + std::format(L"\\fonts-{:016x}.bin", h);
}

[[nodiscard]] std::optional<std::vector<strata::u8>> read_file(const std::wstring& path)
{
    HANDLE f = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) { return std::nullopt; }
    LARGE_INTEGER size{};
    std::optional<std::vector<strata::u8>> out;
    if (::GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (256ll << 20)) {
        std::vector<strata::u8> bytes(static_cast<std::size_t>(size.QuadPart));
        DWORD got = 0;
        if (::ReadFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &got, nullptr) && got == bytes.size()) { out = std::move(bytes); }
    }
    ::CloseHandle(f);
    return out;
}

void write_cache(const std::wstring& path, std::span<const strata::u8> bytes)
{
    const std::wstring dir = path.substr(0, path.find_last_of(L'\\'));
    ::CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring tmp = path + L".tmp"; // (renamed into place: a half-written file is never read)
    HANDLE f = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { return; }
    DWORD put = 0;
    const bool ok = ::WriteFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &put, nullptr) && put == bytes.size();
    ::CloseHandle(f);
    if (!ok || !::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) { ::DeleteFileW(tmp.c_str()); return; }

    std::vector<std::pair<std::uint64_t, std::wstring>> all; // (write time, path): drop the oldest beyond the keep count
    WIN32_FIND_DATAW fd{};
    if (HANDLE h = ::FindFirstFileW((dir + L"\\fonts-*.bin").c_str(), &fd); h != INVALID_HANDLE_VALUE) {
        do {
            all.emplace_back((static_cast<std::uint64_t>(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime, dir + L"\\" + fd.cFileName);
        } while (::FindNextFileW(h, &fd));
        ::FindClose(h);
    }
    std::ranges::sort(all, std::greater<>{});
    for (std::size_t i = k_cache_keep; i < all.size(); ++i) { ::DeleteFileW(all[i].second.c_str()); }
}

} // namespace

std::expected<strata::context, strata::font_error> create_context(const strata::style& style, float scale, const std::vector<char32_t>& extra)
{
    using namespace strata;

    // faces that supply the glyphs the main face lacks, same size and baseline
    static constexpr std::array<std::string_view, 6> fallbacks{"Microsoft YaHei UI", "Yu Gothic UI", "Malgun Gothic", "Segoe UI Symbol", "Segoe UI Emoji", "Leelawadee UI"};

    // windows 11 ships a variable "Segoe UI Variable"; windows 10 has Segoe UI
    const bool variable = system_font_exists("SegUIVar.ttf");
    const std::string_view text_face = variable ? "Segoe UI Variable Text" : "Segoe UI";
    const std::string_view icon_face = system_font_exists("SegoeIcons.ttf") ? "Segoe Fluent Icons" : "Segoe MDL2 Assets";

    // only the icons the app draws (painter.hpp's glyph::, the sidebar's icon picker). baking the whole private-use
    // area rasterized ~6400 glyphs three times over, which is most of what a size change costs. a new icon = add it here
    static constexpr std::array<std::uint32_t, 64> icon_codepoints{
        0xe700, 0xe706, 0xe70d, 0xe710, 0xe711, 0xe712, 0xe713, 0xe714, 0xe715, 0xe718, 0xe719, 0xe71b, 0xe71d, 0xe721, 0xe722,
        0xe72a, 0xe72b, 0xe72c, 0xe72e, 0xe734, 0xe73e, 0xe745, 0xe74a, 0xe74d, 0xe753, 0xe76c, 0xe774, 0xe787, 0xe7b3, 0xe7c1,
        0xe7f4, 0xe7fc, 0xe80a, 0xe80f, 0xe821, 0xe838, 0xe88e, 0xe896, 0xe8a1, 0xe8a5, 0xe8a7, 0xe8a9, 0xe8b7, 0xe8bb, 0xe8c8,
        0xe768, 0xe769, 0xe8d2, 0xe8d6, 0xe8ea, 0xe8ec, 0xe8f1, 0xe8fd, 0xe91b, 0xe921, 0xe922, 0xe923, 0xe943, 0xe946, 0xe958, 0xea18, 0xeb51, 0xeda2, 0xf012,
    };
    static const std::array<codepoint_range, icon_codepoints.size()> icon_ranges = [] {
        std::array<codepoint_range, icon_codepoints.size()> r{};
        for (std::size_t i = 0; i < r.size(); ++i) { r[i] = {icon_codepoints[i], icon_codepoints[i]}; }
        return r;
    }();
    // the common scripts and symbols are always baked (the atlas cache makes that cheap after the first run: a rebuild
    // for new characters stalls the window, so names in the usual scripts must never need one); the rest is on demand
    const std::span<const codepoint_range> text_ranges{ranges_for(extra)};
    // greek / cyrillic / symbols come from the main face; the fallback faces are only for scripts it lacks
    const auto in_main_face = [](char32_t c) {
        return (c >= 0x250 && c <= 0x52f) || (c >= 0x20a0 && c <= 0x20cf) || (c >= 0x2190 && c <= 0x22ff) || (c >= 0x2500 && c <= 0x25ff);
    };

    font_config body{};
    body.face         = text_face;
    body.pixel_height = 14.0f;
    body.ranges       = text_ranges;
    body.kerning      = false; // integer-pixel kerning pairs give uneven gaps ("tom gay") at ui sizes
    if (!std::ranges::all_of(extra, in_main_face)) { body.fallback_faces = fallbacks; }

    std::array<font_config, 7> extra_fonts{};
    extra_fonts[5].face         = text_face; // id 6
    extra_fonts[5].pixel_height = 20.0f;
    extra_fonts[5].bold         = true;
    extra_fonts[5].kerning      = false;
    extra_fonts[5].ranges       = text_ranges;
    extra_fonts[6].face         = "Consolas"; // id 7
    extra_fonts[6].pixel_height = 12.5f;
    extra_fonts[6].kerning      = false;
    extra_fonts[6].ranges       = text_ranges;
    extra_fonts[4].face         = icon_face; // id 5
    extra_fonts[4].pixel_height = 30.0f;
    extra_fonts[4].ranges       = icon_ranges;
    extra_fonts[3].face         = icon_face; // id 4
    extra_fonts[3].pixel_height = 12.0f;
    extra_fonts[3].ranges       = icon_ranges;
    extra_fonts[0].face         = icon_face; // id 1
    extra_fonts[0].pixel_height = 16.0f;
    extra_fonts[0].ranges       = icon_ranges;
    extra_fonts[1].face         = text_face; // id 2
    extra_fonts[1].pixel_height = 14.0f;
    extra_fonts[1].bold         = true;
    extra_fonts[1].kerning      = false;
    extra_fonts[1].ranges       = text_ranges;
    extra_fonts[2].face         = text_face; // id 3
    extra_fonts[2].pixel_height = 12.0f;
    extra_fonts[2].ranges       = text_ranges;
    extra_fonts[2].kerning      = false;

    context_config cfg{};
    cfg.font         = body;
    cfg.extra_fonts  = extra_fonts;
    cfg.theme        = style;
    cfg.max_atlas_size = 4096;
    // a file list shows thousands of rows; the defaults (64 windows / tables) are far more than the app needs
    cfg.capacity.windows  = 24;
    cfg.capacity.tables   = 8;
    cfg.capacity.children = 24;
    cfg.capacity.cards    = 8;

    cfg.scale = std::clamp(scale, 0.5f, 4.0f);

    const std::wstring            cached_path = cache_file(atlas_key(cfg.scale, extra, text_face, icon_face));
    std::optional<font_atlas>     cached;
    if (const auto bytes = read_file(cached_path)) { cached = font_atlas::load(*bytes); }
    cfg.prebuilt = cached ? &*cached : nullptr;

    auto ui = context::create(cfg);
    if (ui && !cached) { write_cache(cached_path, ui->font().save()); } // (the first run pays; the next ones skip the bake)
    core::log::info("font atlas: {}", cached ? "loaded from the cache" : "baked");
    return ui;
}

} // namespace prism::ui
