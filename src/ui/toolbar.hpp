#pragma once

// navigation buttons, the breadcrumb / address bar, the folder filter and the view buttons

#include "ui/env.hpp"
#include "ui/painter.hpp"

#include <string>

namespace prism::ui {

class Toolbar {
public:
    explicit Toolbar(Env& env) : env_{env} {}

    void frame(Painter& p, const strata::rect& area, bool show_hidden);

    void begin_edit() noexcept { want_edit_ = true; }
    void focus_filter() noexcept { want_filter_focus_ = true; }

private:
    [[nodiscard]] bool icon_button(Painter& p, std::string_view id, char32_t glyph, bool enabled, bool on, std::string_view tip, float size);
    void address(Painter& p, float x, float y, float width, float height);
    static void field_background(Painter& p, const strata::rect& r);

    Env&         env_;
    bool         editing_{};
    bool         want_edit_{};
    bool         want_filter_focus_{};
    int          edit_age_{};
    std::string  edit_text_;
    std::string  filter_text_;
    const void*  last_tab_{};
    std::uint32_t last_generation_{};
};

} // namespace prism::ui
