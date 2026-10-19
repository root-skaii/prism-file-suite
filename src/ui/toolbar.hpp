#pragma once

// navigation buttons, the breadcrumb / address bar, the folder filter and the view buttons

#include "core/query.hpp"
#include "core/shell_menu.hpp"
#include "ui/env.hpp"
#include "ui/painter.hpp"

#include <string>

namespace prism::ui {

class Toolbar {
public:
    explicit Toolbar(Env& env) : env_{env}, shell_menu_{env.window.waker()} {}

    void frame(Painter& p, const strata::rect& area, bool show_hidden);

    void begin_edit() noexcept { want_edit_ = true; }
    void focus_filter() noexcept { want_filter_focus_ = true; }

private:
    [[nodiscard]] bool icon_button(Painter& p, std::string_view id, char32_t glyph, bool enabled, bool on, std::string_view tip, float size);
    void address(Painter& p, float x, float y, float width, float height);
    void crumb_menu(Painter& p); // the folders next to a breadcrumb segment (the chevron's dropdown)
    void crumb_context_menu(Painter& p); // right click on a path segment: the folder's own menu
    static void field_background(Painter& p, const strata::rect& r, bool invalid = false);

    // the filter / search box: scope control, the field, option chips, the syntax popover
    [[nodiscard]] bool scope_control(Painter& p, float half, float height);
    [[nodiscard]] bool option_chip(Painter& p, std::string_view id, std::string_view label, bool on, std::string_view tip, float w, float h);
    [[nodiscard]] core::QueryOptions query_options() const;
    void help_popup(Painter& p, model::Tab& tab);
    // the "view" button's menu: layout, compact rows, panes, hidden files
    void view_menu(Painter& p, bool show_hidden);
    void new_menu(Painter& p, const model::Tab& tab);
    // parses the box and shows / searches accordingly (`run`: start the subfolder search now)
    void apply_filter(model::Tab& tab, bool run);

    Env&         env_;
    core::ShellMenu shell_menu_; // Explorer's own "New" entries (Word document, Bitmap image ...) for the New menu, read on a worker
    bool         editing_{};
    bool         want_edit_{};
    bool         want_filter_focus_{};
    int          edit_age_{};
    std::string  edit_text_;
    std::string  filter_text_;
    std::string  crumb_parent_, crumb_next_; // the dropdown's folder and the segment the path continues with
    std::string  crumb_ctx_path_;   // the folder the right click menu is about
    bool         crumb_ctx_paste_{}; // the clipboard held files when it opened (looked at once, not every frame)
    std::vector<std::string> crumb_dirs_;
    const void*  last_tab_{};
    std::uint32_t last_generation_{};
};

} // namespace prism::ui
