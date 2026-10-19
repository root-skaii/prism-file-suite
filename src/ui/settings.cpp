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

std::string Settings::folder_sorts_path()
{
    const std::string p   = sidebar_path();
    const std::size_t cut = p.find_last_of('\\') == std::string::npos ? 0 : p.find_last_of('\\') + 1;
    return p.substr(0, cut) + (p.substr(cut) == "prismfs-sidebar.ini" ? "prismfs-foldersort.ini" : "foldersort.ini");
}

std::string Settings::tags_path()
{
    const std::string p   = sidebar_path();
    const std::size_t cut = p.find_last_of('\\') == std::string::npos ? 0 : p.find_last_of('\\') + 1;
    return p.substr(0, cut) + (p.substr(cut) == "prismfs-sidebar.ini" ? "prismfs-tags.ini" : "tags.ini");
}

std::string Settings::sessions_path()
{
    const std::string p   = sidebar_path();
    const std::size_t cut = p.find_last_of('\\') == std::string::npos ? 0 : p.find_last_of('\\') + 1;
    return p.substr(0, cut) + (p.substr(cut) == "prismfs-sidebar.ini" ? "prismfs-sessions.ini" : "sessions.ini");
}

std::string Settings::log_path()
{
    const std::string p   = file_path();
    const std::size_t cut = p.find_last_of('\\') == std::string::npos ? 0 : p.find_last_of('\\') + 1;
    return p.substr(0, cut) + "prismfs.log";
}

std::string Settings::cache_path()
{
    const std::string p   = file_path();
    const std::size_t cut = p.find_last_of('\\') == std::string::npos ? 0 : p.find_last_of('\\') + 1;
    return p.substr(0, cut) + (p.substr(cut) == "prismfs.ini" ? "prismfs-cache" : "cache");
}

std::string Settings::templates_path()
{
    const std::string p   = file_path();
    const std::size_t cut = p.find_last_of('\\') == std::string::npos ? 0 : p.find_last_of('\\') + 1;
    return p.substr(0, cut) + "Templates";
}

std::vector<std::string> Settings::language_dirs()
{
    std::vector<std::string> dirs;
    wchar_t exe[MAX_PATH]{};
    if (const DWORD n = ::GetModuleFileNameW(nullptr, exe, MAX_PATH); n > 0 && n < MAX_PATH) {
        std::string beside = core::text::narrow(exe);
        beside.resize(beside.find_last_of('\\') + 1);
        dirs.push_back(beside + "lang");
    }
    const std::string p = file_path();
    dirs.push_back(p.substr(0, p.find_last_of('\\') == std::string::npos ? 0 : p.find_last_of('\\') + 1) + "Languages");
    return dirs;
}

void Settings::load()
{
    strata::config c;
    if (!c.load_file(file_path())) { return; }
    if (binds != nullptr) { (void)binds->load(c); }
    dark           = c.get_bool("appearance", "dark", dark);
    compact        = c.get_bool("appearance", "compact", compact);
    language       = std::string{c.get("appearance", "language")}.empty() ? language : std::string{c.get("appearance", "language")};
    dark_style     = std::clamp(c.get_int("appearance", "dark_style", dark_style), 0, 2);
    accent         = static_cast<unsigned>(c.get_int("appearance", "accent_rgb", static_cast<int>(accent))) & 0xffffffu;
    ui_scale       = std::clamp(c.get_int("appearance", "ui_scale", ui_scale), 75, 200);
    show_hidden    = c.get_bool("behavior", "show_hidden", show_hidden);
    confirm_delete = c.get_bool("behavior", "confirm_delete", confirm_delete);
    restore_tabs   = c.get_bool("behavior", "restore_tabs", restore_tabs);
    background     = c.get_bool("behavior", "background", c.get_bool("behavior", "tray", background)); // ("tray" was the older name: keep what was chosen)
    shell_menu     = c.get_bool("behavior", "shell_menu", shell_menu);
    track_history  = c.get_bool("behavior", "track_history", track_history);
    browse_archives = c.get_bool("behavior", "browse_archives", browse_archives);
    builtin_copy    = c.get_bool("behavior", "builtin_copy", builtin_copy);
    index_drives    = c.get_bool("behavior", "index_drives", index_drives);
    log_enabled     = c.get_bool("diagnostics", "log", log_enabled);
    search_subfolders = c.get_bool("search", "subfolders", search_subfolders);
    search_case    = c.get_bool("search", "match_case", search_case);
    search_regex   = c.get_bool("search", "regex", search_regex);
    grid_view     = c.get_bool("layout", "grid_view", grid_view);
    preview        = c.get_bool("layout", "preview", preview);
    split          = c.get_bool("layout", "split", split);
    folder_sizes   = c.get_bool("layout", "folder_sizes", folder_sizes);
    git_badges     = c.get_bool("layout", "git_badges", git_badges);
    sort_key         = std::clamp(c.get_int("sorting", "key", sort_key), 0, 3);
    sort_ascending   = c.get_bool("sorting", "ascending", sort_ascending);
    sort_folders     = std::clamp(c.get_int("sorting", "folders", sort_folders), 0, 2);
    sort_natural     = c.get_bool("sorting", "natural", sort_natural);
    sort_hidden_last = c.get_bool("sorting", "hidden_last", sort_hidden_last);
    sort_per_folder  = c.get_bool("sorting", "per_folder", sort_per_folder);
    group_by         = std::clamp(c.get_int("sorting", "group_by", group_by), 0, 4);
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
    c.set("appearance", "language", language);
    c.set_int("appearance", "dark_style", dark_style);
    c.set_int("appearance", "accent_rgb", static_cast<int>(accent));
    c.set_int("appearance", "ui_scale", ui_scale);
    c.set_bool("behavior", "show_hidden", show_hidden);
    c.set_bool("behavior", "confirm_delete", confirm_delete);
    c.set_bool("behavior", "restore_tabs", restore_tabs);
    c.set_bool("behavior", "background", background);
    c.set_bool("behavior", "shell_menu", shell_menu);
    c.set_bool("behavior", "track_history", track_history);
    c.set_bool("behavior", "browse_archives", browse_archives);
    c.set_bool("behavior", "builtin_copy", builtin_copy);
    c.set_bool("behavior", "index_drives", index_drives);
    c.set_bool("diagnostics", "log", log_enabled);
    c.set_bool("search", "subfolders", search_subfolders);
    c.set_bool("search", "match_case", search_case);
    c.set_bool("search", "regex", search_regex);
    c.set_bool("layout", "grid_view", grid_view);
    c.set_bool("layout", "preview", preview);
    c.set_bool("layout", "split", split);
    c.set_bool("layout", "folder_sizes", folder_sizes);
    c.set_bool("layout", "git_badges", git_badges);
    c.set_int("sorting", "key", sort_key);
    c.set_bool("sorting", "ascending", sort_ascending);
    c.set_int("sorting", "folders", sort_folders);
    c.set_bool("sorting", "natural", sort_natural);
    c.set_bool("sorting", "hidden_last", sort_hidden_last);
    c.set_bool("sorting", "per_folder", sort_per_folder);
    c.set_int("sorting", "group_by", group_by);
    c.set_float("layout", "sidebar_width", sidebar_width);
    c.set_int("layout", "window_w", window_w);
    c.set_int("layout", "window_h", window_h);
    c.set_int("tabs", "active", active_tab);
    c.set_int("tabs", "count", static_cast<int>(tabs.size()));
    for (std::size_t i = 0; i < tabs.size(); ++i) { c.set("tabs", std::format("tab{}", i), tabs[i]); }
    (void)c.save_file(file_path());
}

} // namespace prism::ui
