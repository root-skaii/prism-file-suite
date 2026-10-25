#pragma once

// the sidebar: the user's own sections of pinned folders plus the drive list (model::SidebarLayout). click goes to the
// folder in the active tab, middle click opens a tab, press-and-move reorders, right click edits. dropping files on a
// folder moves / copies them there; dropping folders between rows pins them

#include "core/drive_watcher.hpp"
#include "core/path_probe.hpp"
#include "core/places.hpp"
#include "core/shell_menu.hpp"
#include "ui/env.hpp"
#include "ui/painter.hpp"
#include "ui/shell_page.hpp"

#include <optional>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace prism::ui {

class Sidebar {
public:
    explicit Sidebar(Env& env);

    void frame(Painter& p, const strata::rect& area, const FrameInput& in);
    void refresh_drives();
    [[nodiscard]] const std::vector<core::Place>& drives() const noexcept { return drives_; } // (as last read by the watcher)

    // where a drop at this point lands: a folder to move / copy into, a pin slot between rows (see is_pin_target), or ""
    [[nodiscard]] std::string drop_target_at(strata::vec2 p) const;
    void set_drop_point(std::optional<strata::vec2> p) { drop_ = p; }
    [[nodiscard]] static bool is_pin_target(std::string_view dest) noexcept;
    // the folders among `paths` get pinned at the slot a pin target names
    void pin_dropped(const std::vector<std::string>& paths, std::string_view dest);

private:
    enum class Kind { none, header, item, drive, empty, visit, note };

    struct Row {
        strata::rect cell;   // the whole row, screen space (hit area)
        strata::rect label;  // where the name sits (rename opens there)
        Kind         kind{};
        std::size_t  section{};
        std::size_t  index{};
        std::string  path;
    };
    struct Target {
        Kind        kind{Kind::none};
        std::size_t section{};
        std::size_t index{};
        std::string path;
    };
    // a button press that may become a click or a drag
    struct Press {
        bool         armed{};
        bool         active{};   // moved far enough to be a drag
        Target       target;
        strata::vec2 origin{};
    };
    // what the pointer is over while something is dragged here
    struct Spot {
        enum class Type { none, into, between } type{Type::none};
        std::string  path;                // into: the folder
        std::size_t  section{}, slot{};   // between: where it would be pinned
        strata::rect line{};              // between: the insertion line
    };
    struct Rename {
        bool         active{};
        Target       target;
        std::string  text;
        int          age{};
        int          miss{};
        int          gen{};
    };

    void header(Painter& p, std::size_t si, float top_space);
    void item_row(Painter& p, std::size_t si, std::size_t ii);
    void drive_row(Painter& p, const core::Place& place);
    void devices_header(Painter& p, std::size_t si);
    void empty_row(Painter& p, std::size_t si);
    void visit_row(Painter& p, std::size_t si, const std::string& path);
    void note_row(Painter& p, std::size_t si, std::string_view text);
    void row_background(Painter& p, const std::string& key, const strata::item_result& r, const strata::rect& row, bool active);
    void look(const model::SidebarItem& it, char32_t& glyph, strata::color& tint, const Palette& pal) const;

    [[nodiscard]] Spot spot_at(strata::vec2 p, bool into_ok) const;
    [[nodiscard]] std::pair<std::size_t, strata::rect> section_slot_at(float y) const;
    [[nodiscard]] const Row* row_for(const Target& t) const;
    [[nodiscard]] bool valid(const Target& t) const;

    void settle_press(Painter& p);
    void click(const Target& t);
    void draw_drag(Painter& p);
    void draw_drop_hint(Painter& p);
    void ghost(Painter& p, const strata::rect& at, char32_t glyph, strata::color tint, std::string_view label);
    void pin_current(std::size_t section);

    void begin_rename(const Target& t);
    void draw_rename(Painter& p);
    void context_menu(Painter& p);
    void icon_popup(Painter& p);

    Env&                       env_;
    std::vector<core::Place>   drives_;
    core::DriveWatcher         drive_watch_;   // (the drive list is read on a worker: a slow disk must not stall a frame)
    unsigned                   drives_rev_{};
    bool                       devices_collapsed_{};
    core::PathProbe            probe_;
    core::ShellMenu            shell_menu_;                    // Explorer's own entries for the folder menus, read on a worker
    ShellPage                  shell_page_{env_, shell_menu_};
    strata::rect               area_{};
    std::vector<Row>           rows_;    // this frame's rows, screen space (the drop target reads last frame's)
    std::vector<strata::rect>  blocks_;  // each section from its title to its last row
    std::vector<std::vector<std::string>> auto_rows_; // what each automatic section lists this frame
    std::optional<strata::vec2> drop_;
    Spot                       hint_;    // what a drop at drop_ would do (from last frame's rows)
    Press                      press_;
    Target                     menu_;
    bool                       paste_ok_{};    // the clipboard holds a sidebar layout (looked at once, when the menu opens)
    Rename                     rename_;
    Target                     icon_target_;   // the item whose icon the picker is editing
    bool                       icon_want_{};   // open the picker (asked from the menu, opened after it closed)
    strata::vec2               icon_pos_{};
};

} // namespace prism::ui
