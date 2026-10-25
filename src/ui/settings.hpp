#pragma once

// what survives a restart: %APPDATA%\PrismFS\settings.ini

#include "strata/keybinds.hpp"

#include <string>
#include <vector>

namespace prism::ui {

struct Settings {
    bool  dark{true};
    bool  compact{false};
    std::string language{"en"}; // core::i18n code
    int   dark_style{0};      // DarkStyle: 0 neutral, 1 graphite, 2 slate
    unsigned accent{0x6d8cff};  // 0xRRGGBB, any colour
    bool  show_hidden{false};
    bool  confirm_delete{false}; // ask before moving to the recycle bin (deleting for good always asks)
    bool  restore_tabs{true};
    bool  background{true};      // PrismFS keeps running after the last window closes, with a notification-area icon (open / settings / exit); off: the last window quits it
    bool  detached_menus{false}; // experimental: menus may reach past the window edge (drawn in a window of their own)
    bool  shell_menu{true};     // the right-click menu also lists Explorer's own entries (Open with Code, 7-Zip, New ...)
    bool  browse_archives{true}; // Enter on a zip / 7z / tar opens it as a folder (a temporary unpacked copy) instead of the default app
    bool  index_drives{true};    // a background index of the fixed drives' file names, for "Search everywhere" (Ctrl+Shift+F)
    bool  builtin_copy{true};   // copy and move run in the app's own queue (progress panel, conflict prompt); off: Windows' copy window
    bool  log_enabled{false};   // write a troubleshooting log (startup steps, slow frames, slow folders) beside the settings file
    bool  track_history{false};  // remember visited folders (Recent / Frequent in the sidebar); off: nothing is recorded or kept
    bool  search_subfolders{false}; // the filter box searches below the folder (Enter) instead of filtering what is shown
    bool  search_case{false};       // match case
    bool  search_regex{false};      // plain words are regular expressions
    bool  grid_view{false};     // thumbnails instead of the details list
    bool  preview{false};       // the preview pane on the right
    bool  split{false};         // two folders side by side
    bool  folder_sizes{false};  // the size column also shows folder sizes (counted in the background)
    bool  git_badges{true};     // changed files in a git repository carry a status letter (needs git installed)
    // sorting (see model::SortSpec): the order every folder starts with, and the options of the order itself
    int   sort_key{0};          // model::SortKey
    bool  sort_ascending{true};
    int   sort_folders{0};      // model::FolderPlacement: 0 first, 1 mixed, 2 last
    bool  sort_natural{true};   // file2 before file10
    bool  sort_hidden_last{false};
    bool  sort_per_folder{false}; // remember the order of each folder separately
    int   group_by{0};          // model::GroupBy: titled groups in the details view
    int   ui_scale{100};        // percent on top of the monitor's dpi
    float sidebar_width{0.0f};  // 0: the theme's default
    int   window_w{0};          // logical client size of the last session (0: default)
    int   window_h{0};
    std::vector<std::string> tabs;
    int   active_tab{0};

    // the action registry whose chords are stored too (set before load())
    strata::keybinds* binds{};

    void load();
    void save() const;
    [[nodiscard]] static std::string file_path();
    // the sidebar layout lives beside the settings (sidebar.ini, or prismfs-sidebar.ini in portable mode)
    [[nodiscard]] static std::string sidebar_path();
    [[nodiscard]] static std::string visits_path(); // the folders you visited (Recent / Frequent)
    // documents offered in the "New" menu: a Templates folder beside the settings file
    [[nodiscard]] static std::string templates_path();
    // *.lang translation files: the ones shipped beside the exe, then the user's own (same code overrides)
    [[nodiscard]] static std::vector<std::string> language_dirs();
    // the order remembered per folder (when sort_per_folder is on)
    [[nodiscard]] static std::string folder_sorts_path();
    // named sessions (tabs + second pane)
    [[nodiscard]] static std::string sessions_path();
    // colour labels on files
    [[nodiscard]] static std::string tags_path();
    // the troubleshooting log (when log_enabled)
    [[nodiscard]] static std::string log_path();
    [[nodiscard]] static std::string cache_path(); // folder for rebuildable data (the baked font atlas); safe to delete
};

} // namespace prism::ui
