#pragma once

// Explorer's own context-menu entries (7-Zip, Open with Code ...) drawn inside a strata menu: the "More options" submenu
// of the file list and of the path bar's folder menu

#include "core/shell_menu.hpp"
#include "ui/env.hpp"

#include <vector>

namespace prism::ui {

class ShellPage {
public:
    ShellPage(Env& env, core::ShellMenu& menu) : env_{env}, menu_{menu} {}

    // once per frame, before any menu: forgets the opened entries when the page was not drawn last frame (menu closed)
    void begin_frame();
    void draw(strata::context& ui, const std::vector<core::ShellMenuItem>& root);
    // `drill`: entries with children open in place (see draw); otherwise they are strata submenus
    void draw_items(strata::context& ui, const std::vector<core::ShellMenuItem>& items, bool drill);

private:
    Env&                     env_;
    core::ShellMenu&         menu_;
    std::vector<std::size_t> path_;        // the entries opened in place
    bool                     drawn_{};
};

} // namespace prism::ui
