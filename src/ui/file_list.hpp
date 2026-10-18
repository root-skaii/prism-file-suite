#pragma once

// one folder view: details list or thumbnail grid. only visible items are drawn, so a 200k-entry folder costs what a
// 40-entry one does. selection, keyboard navigation, type-to-find, sorting, clipboard, delete, rename, drag and drop.

#include "core/shell_menu.hpp"
#include "ui/env.hpp"
#include "ui/painter.hpp"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace prism::ui {

class FileList {
public:
    // `right`: this is the second pane of a split view
    FileList(Env& env, bool right) : env_{env}, right_{right}, shell_menu_{env.window.waker()} {}

    // `active`: this pane has the keyboard (the other one ignores shortcuts)
    void frame(Painter& p, const strata::rect& area, model::Tab& tab, const FrameInput& in, bool active);

    void new_folder(model::Tab& tab);
    // an empty file "<stem><ext>" in the folder, scrolled into view and open for renaming like a new folder
    void new_file(model::Tab& tab, const std::string& stem, const std::string& ext);
    // runs a file action by its registry name (see ui/actions.cpp); false if it is not one of the list's
    bool perform(strata::context& ui, model::Tab& tab, std::string_view action);

    // drag and drop: the folder under a point (logical), "" if the point is outside this pane; and the pointer while
    // a drag from elsewhere is over the window (nullopt: none)
    [[nodiscard]] std::string drop_target_at(strata::vec2 p) const;
    void set_drop_point(std::optional<strata::vec2> p) { drop_point_ = p; }
    [[nodiscard]] bool contains(strata::vec2 p) const { return area_.contains(p); }

private:
    enum Col : int { col_modified, col_type, col_size, col_count };

    struct Geo {
        bool  grid{};
        float body_w{};
        float cell_w{}, cell_h{};   // details: cell_w = body_w, cell_h = row height
        int   per_row{1};
    };
    [[nodiscard]] Geo make_geo(float body_w) const;
    [[nodiscard]] strata::rect cell_rect(const Geo& g, std::size_t i, strata::vec2 origin) const;
    [[nodiscard]] int index_at(const Geo& g, strata::vec2 origin, strata::vec2 p, std::size_t rows) const;

    void open_row(model::Tab& tab, std::size_t row);
    void handle_keys(strata::context& ui, model::Tab& tab, const FrameInput& in, const Geo& g, float viewport_h);
    void draw_header(Painter& p, model::Listing& listing, float width, float height, float body_w);
    void draw_details(Painter& p, model::Tab& tab, const Geo& g, strata::vec2 origin, float viewport_top, float viewport_bottom, int hover, float enter);
    void draw_grid(Painter& p, model::Tab& tab, const Geo& g, strata::vec2 origin, float viewport_top, float viewport_bottom, int hover, float enter);
    void draw_empty_state(Painter& p, model::Tab& tab, const strata::rect& viewport);
    void context_menu(Painter& p, model::Tab& tab, const strata::rect& area);
    void move_focus(model::Tab& tab, std::size_t row, bool extend);
    // drag-to-select: press on empty space and sweep a rectangle over items
    void update_band(strata::context& ui, model::Tab& tab, const Geo& g, strata::vec2 origin, const strata::rect& vp, float dt);
    void draw_band(Painter& p, strata::vec2 origin) const;

    // file operations
    [[nodiscard]] std::vector<std::string> selected_paths(const model::Tab& tab) const;
    void copy_selection(model::Tab& tab, bool cut);
    void paste(model::Tab& tab);
    void to_other_pane(model::Tab& tab, bool move);
    void begin_rename(strata::context& ui, model::Tab& tab, std::size_t row);
    void draw_rename(Painter& p, model::Tab& tab, const Geo& g, strata::vec2 origin);
    [[nodiscard]] bool is_cut(const model::Tab& tab, std::string_view name) const;

    void draw_shell_items(strata::context& ui, const std::vector<core::ShellMenuItem>& items, bool drill);
    std::vector<std::size_t> shell_path_;   // the entries opened in place inside "More options"
    bool                     shell_page_drawn_{}; // (the page was drawn this frame: when it was not, the menu closed)
    void draw_shell_page(strata::context& ui, const std::vector<core::ShellMenuItem>& items);

    Env&  env_;
    bool  right_{};
    core::ShellMenu shell_menu_; // (Explorer's own entries for the right-click menu, read on a worker)
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
    float         enter_{1.0f};      // 0 -> 1 after navigating: items fade and rise in
    // last frame's geometry (drops are resolved between frames)
    strata::rect  area_{};
    strata::rect  viewport_{};
    strata::vec2  origin_{};
    Geo           geo_{};
    const model::Tab* shown_{};
    // type-to-find
    std::string   find_buffer_;
    double        find_age_{};
    // cut items stay listed but dimmed until pasted
    std::vector<std::string> cut_names_;
    std::string              cut_dir_;
    // rename field
    bool         rename_active_{};
    std::string  rename_path_;
    std::string  rename_orig_;
    std::string  rename_text_;
    std::size_t  rename_row_{};
    int          rename_age_{};
    int          rename_gen_{};
    std::string  pending_rename_; // a folder just created: rename it once the listing has it
    // drag
    bool                        drag_armed_{};
    strata::vec2                drag_origin_{};
    int                         collapse_row_{-1}; // a press on part of a multi-selection collapses it on release
    // selection rectangle (content space, so it stays put while the list scrolls)
    bool                     band_armed_{};
    bool                     band_active_{};
    bool                     band_ctrl_{};
    strata::vec2             band_anchor_{};
    std::vector<std::size_t> band_base_;   // what was selected when it began (kept with Ctrl)
    std::optional<strata::vec2> drop_point_;
};

} // namespace prism::ui
