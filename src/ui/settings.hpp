#pragma once

// what survives a restart: %APPDATA%\PrismFS\settings.ini

#include "strata/keybinds.hpp"

#include <string>
#include <vector>

namespace prism::ui {

struct Settings {
    bool  dark{true};
    bool  compact{false};
    int   dark_style{0};        // DarkStyle: 0 neutral, 1 graphite, 2 slate
    unsigned accent{0x6d8cff};  // 0xRRGGBB, any colour
    bool  show_hidden{false};
    bool  confirm_delete{false}; // ask before moving to the recycle bin (deleting for good always asks)
    bool  restore_tabs{true};
    bool  track_history{true};   // remember visited folders (Recent / Frequent in the sidebar); off: nothing is recorded or kept
    bool  search_subfolders{false}; // the filter box searches below the folder (Enter) instead of filtering what is shown
    bool  search_case{false};       // match case
    bool  search_regex{false};      // plain words are regular expressions
    bool  grid_view{false};     // thumbnails instead of the details list
    bool  preview{false};       // the preview pane on the right
    bool  split{false};         // two folders side by side
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
};

} // namespace prism::ui
