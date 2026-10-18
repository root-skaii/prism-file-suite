#pragma once

// the details view of the active tab's folder. rows are drawn by hand and only the visible ones: a 200k-entry folder
// costs what a 40-entry one does. selection, keyboard navigation, type-to-find, sorting by header, resizable columns.

#include "ui/env.hpp"
#include "ui/painter.hpp"

#include <array>
#include <string>

namespace prism::ui {

class FileList {
public:
    explicit FileList(Env& env) : env_{env} {}

    // `typed`: the text typed this frame (for type-to-find)
    void frame(Painter& p, const strata::rect& area, const FrameInput& in);

private:
    enum Col : int { col_modified, col_type, col_size, col_count };

    void open_row(model::Tab& tab, std::size_t row);
    void handle_keys(strata::context& ui, model::Tab& tab, const FrameInput& in, float viewport_h);
    void draw_header(Painter& p, model::Listing& listing, float width, float height, float body_w);
    void draw_rows(Painter& p, model::Tab& tab, const strata::item_result& canvas, float body_w, float viewport_top,
                   float viewport_bottom, int hover_row);
    void draw_empty_state(Painter& p, model::Tab& tab, const strata::rect& viewport);
    void context_menu(Painter& p, model::Tab& tab, const strata::rect& area);
    void move_focus(model::Tab& tab, std::size_t row, bool extend);

    Env& env_;
    std::array<float, col_count> widths_{170.0f, 150.0f, 100.0f};
    int         resize_col_{-1};
    float       resize_start_x_{};
    float       resize_start_w_{};
    // scroll bookkeeping
    const void*   last_tab_{};
    std::uint32_t applied_generation_{~0u};
    bool          restore_pending_{};
    bool          ensure_visible_{};
    bool          keyboard_mode_{};
    // type-to-find
    std::string   find_buffer_;
    double        find_age_{};
};

} // namespace prism::ui
