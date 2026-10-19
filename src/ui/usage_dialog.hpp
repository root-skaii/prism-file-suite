#pragma once

// where the disk space goes. a folder tree (a Strata tree table: sortable, resizable, keyboard driven) beside a treemap
// that shows the folder and one level of what is inside each folder tile. both show the same selection. a double click or
// Enter looks inside a folder, the breadcrumbs and Backspace go back out, right click has the usual actions.
// core::DiskUsage scans on a worker

#include "core/disk_usage.hpp"
#include "ui/env.hpp"
#include "ui/painter.hpp"
#include "ui/widgets.hpp"

#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace prism::ui {

class UsageDialog {
public:
    explicit UsageDialog(Env& env) : env_{env} {}
    ~UsageDialog() { scan_.cancel(); }

    void open(std::string folder);
    void frame(Painter& p);

private:
    // an item of the view: the node, and the nodes between the folder on screen and it (the node itself last)
    struct Pick {
        const core::UsageNode*              node{};
        std::vector<const core::UsageNode*> chain;
    };

    void rescan(std::string folder);
    [[nodiscard]] std::string current_path() const;
    [[nodiscard]] std::string path_of(const Pick& pick) const;
    void look_inside(const Pick& pick);   // goes into a folder (or scans it, past what the scan kept)
    void go_to(std::size_t depth);        // back out to trail_[depth]
    void draw_tree(strata::context& ui, const FontSet& f, const Palette& pal, const core::UsageNode& cur, float row_h);
    void tree_keys(strata::context& ui);
    void draw_map(Painter& p, const strata::item_result& mr, const core::UsageNode& cur);

    Env&                                     env_;
    core::DiskUsage                          scan_;
    std::shared_ptr<const core::UsageNode>   root_;     // keeps every node of the trail alive
    std::vector<const core::UsageNode*>      trail_;    // root first; the last one is on screen
    std::string                              base_;     // the folder the scan started at
    Pick                                     pick_;     // the selection (kept while the folder on screen stays)
    Pick                                     hover_;    // under the pointer this frame (either side)
    Pick                                     ctx_;      // what the right-click menu is about
    // the tree rows are drawn by hand: Strata skips the cells of a row scrolled out of view, and with them a tree arrow, so
    // its open / closed state and the rows after it would change as you scroll. this set is the one truth instead
    std::unordered_set<const core::UsageNode*> open_;
    std::vector<Pick>                        flat_;       // the rows in view order (last frame's, for the keyboard)
    std::vector<Pick>                        flat_next_;  // ... being built by this frame
    bool                                     follow_{};   // scroll the selected row into view
    int                                      sort_col_{1};      // 0 name, 1 size, 2 files
    bool                                     sort_asc_{false};
    unsigned                                 generation_{};     // changes with every folder change: restarts the fade-in
    bool                                     want_open_{};
    bool                                     shown_{};
};

} // namespace prism::ui
