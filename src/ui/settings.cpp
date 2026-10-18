#include "ui/settings.hpp"

#include "core/text.hpp"
#include "strata/config.hpp"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <format>

namespace prism::ui {

std::string Settings::file_path()
{
    // portable mode: a prismfs.ini beside the exe wins (copy the folder to a stick and the settings come along)
    wchar_t exe[MAX_PATH]{};
    if (const DWORD n = ::GetModuleFileNameW(nullptr, exe, MAX_PATH); n > 0 && n < MAX_PATH) {
        std::wstring beside{exe};
        beside.resize(beside.find_last_of(L'\\') + 1);
        beside += L"prismfs.ini";
        if (::GetFileAttributesW(beside.c_str()) != INVALID_FILE_ATTRIBUTES) { return core::text::narrow(beside); }
    }

    PWSTR raw = nullptr;
    std::string dir;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE, nullptr, &raw)) && raw != nullptr) { dir = core::text::narrow(raw); }
    ::CoTaskMemFree(raw);
    if (dir.empty()) { return "prismfs.ini"; }
    dir += "\\PrismFS";
    ::CreateDirectoryW(core::text::widen(dir).c_str(), nullptr);
    return dir + "\\settings.ini";
}

std::string Settings::sidebar_path()
{
    const std::string p   = file_path();
    const std::size_t cut = p.find_last_of('\\') == std::string::npos ? 0 : p.find_last_of('\\') + 1;
    const bool portable   = p.substr(cut) == "prismfs.ini";
    return p.substr(0, cut) + (portable ? "prismfs-sidebar.ini" : "sidebar.ini");
}

std::string Settings::visits_path()
{
    const std::string p   = sidebar_path();
    const std::size_t cut = p.find_last_of('\\') == std::string::npos ? 0 : p.find_last_of('\\') + 1;
    return p.substr(0, cut) + (p.substr(cut) == "prismfs-sidebar.ini" ? "prismfs-visits.ini" : "visits.ini");
}

void Settings::load()
{
    strata::config c;
    if (!c.load_file(file_path())) { return; }
    if (binds != nullptr) { (void)binds->load(c); }
    dark           = c.get_bool("appearance", "dark", dark);
    compact        = c.get_bool("appearance", "compact", compact);
    dark_style     = std::clamp(c.get_int("appearance", "dark_style", dark_style), 0, 2);
    accent         = static_cast<unsigned>(c.get_int("appearance", "accent_rgb", static_cast<int>(accent))) & 0xffffffu;
    ui_scale       = std::clamp(c.get_int("appearance", "ui_scale", ui_scale), 75, 200);
    show_hidden    = c.get_bool("behavior", "show_hidden", show_hidden);
    confirm_delete = c.get_bool("behavior", "confirm_delete", confirm_delete);
    restore_tabs   = c.get_bool("behavior", "restore_tabs", restore_tabs);
    shell_menu     = c.get_bool("behavior", "shell_menu", shell_menu);
    track_history  = c.get_bool("behavior", "track_history", track_history);
    search_subfolders = c.get_bool("search", "subfolders", search_subfolders);
    search_case    = c.get_bool("search", "match_case", search_case);
    search_regex   = c.get_bool("search", "regex", search_regex);
    grid_view     = c.get_bool("layout", "grid_view", grid_view);
    preview        = c.get_bool("layout", "preview", preview);
    split          = c.get_bool("layout", "split", split);
    sidebar_width  = c.get_float("layout", "sidebar_width", sidebar_width);
    window_w       = c.get_int("layout", "window_w", window_w);
    window_h       = c.get_int("layout", "window_h", window_h);
    active_tab     = c.get_int("tabs", "active", 0);
    const int n    = std::clamp(c.get_int("tabs", "count", 0), 0, 32);
    for (int i = 0; i < n; ++i) {
        const std::string_view p = c.get("tabs", std::format("tab{}", i));
        if (!p.empty()) { tabs.emplace_back(p); }
    }
}

void Settings::save() const
{
    strata::config c;
    if (binds != nullptr) { binds->store(c); }
    c.set_bool("appearance", "dark", dark);
    c.set_bool("appearance", "compact", compact);
    c.set_int("appearance", "dark_style", dark_style);
    c.set_int("appearance", "accent_rgb", static_cast<int>(accent));
    c.set_int("appearance", "ui_scale", ui_scale);
    c.set_bool("behavior", "show_hidden", show_hidden);
    c.set_bool("behavior", "confirm_delete", confirm_delete);
    c.set_bool("behavior", "restore_tabs", restore_tabs);
    c.set_bool("behavior", "shell_menu", shell_menu);
    c.set_bool("behavior", "track_history", track_history);
    c.set_bool("search", "subfolders", search_subfolders);
    c.set_bool("search", "match_case", search_case);
    c.set_bool("search", "regex", search_regex);
    c.set_bool("layout", "grid_view", grid_view);
    c.set_bool("layout", "preview", preview);
    c.set_bool("layout", "split", split);
    c.set_float("layout", "sidebar_width", sidebar_width);
    c.set_int("layout", "window_w", window_w);
    c.set_int("layout", "window_h", window_h);
    c.set_int("tabs", "active", active_tab);
    c.set_int("tabs", "count", static_cast<int>(tabs.size()));
    for (std::size_t i = 0; i < tabs.size(); ++i) { c.set("tabs", std::format("tab{}", i), tabs[i]); }
    (void)c.save_file(file_path());
}

} // namespace prism::ui
