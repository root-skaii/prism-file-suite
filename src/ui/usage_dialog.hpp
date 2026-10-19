#pragma once

// where the disk space goes: a folder tree (open any folder in place, as deep as you like) with a size bar per row.
// only the rows in view are drawn (strata list_clipper), the flat row list is rebuilt only when something structural
// changes, and the list scrolls both ways. folders past what a scan kept are scanned on demand; Back and the crumbs
// return across those scans, finished scans are cached. core::DiskUsage scans on a worker

#include "core/disk_usage.hpp"
#include "ui/env.hpp"
#include "ui/painter.hpp"
#include "ui/widgets.hpp"

#include <memory>
#include <string>
#include <unordered_map>
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
    struct Row {
        const core::UsageNode* node{};
        int                    parent{-1}; // index of the row it is under
        int                    depth{};
        std::size_t            top{};      // which top-level item it belongs to (its colour)
    };
    // a scan the user drilled out of: its trail is kept so Back lands where they were
    struct View {
        std::string                            base;
        std::shared_ptr<const core::UsageNode> root;
        std::vector<const core::UsageNode*>    trail;
    };
    struct Cached {
        std::shared_ptr<const core::UsageNode> root;
        std::uint64_t                          when{};
    };
    static constexpr std::uint64_t cache_ttl_ms = 10 * 60 * 1000;

    static std::string cache_key(std::string path);
    void rescan(std::string folder, bool force, bool remember_view);   // reuses a scan of the same folder from the last 10 minutes unless forced
    void restore(std::size_t view, std::size_t depth);                 // back to history_[view] at trail depth
    void back();
    [[nodiscard]] std::string current_path() const;
    [[nodiscard]] std::string path_of(const Pick& pick) const;
    [[nodiscard]] Pick pick_at(std::size_t row) const;
    void look_inside(const Pick& pick);   // goes into a folder (or scans it, past what the scan kept)
    void go_to(std::size_t depth);        // back out to trail_[depth]
    void rebuild_rows(strata::context& ui, const FontSet& f, const core::UsageNode& cur);
    void draw_tree(strata::context& ui, const FontSet& f, const Palette& pal, const core::UsageNode& cur, float row_h, float height);
    void tree_keys(strata::context& ui);

    Env&                                     env_;
    core::DiskUsage                          scan_;
    std::unordered_map<std::string, Cached>  cache_;    // finished scans by folder, kept after the dialog closes
    std::shared_ptr<const core::UsageNode>   root_;     // keeps every node of the trail alive
    std::vector<const core::UsageNode*>      trail_;    // root first; the last one is on screen
    std::vector<View>                        history_;
    std::string                              base_;     // the folder the scan started at
    Pick                                     pick_;     // the selection
    Pick                                     ctx_;      // what the right-click menu is about
    std::unordered_set<const core::UsageNode*> open_;   // folders opened in the tree
    std::vector<Row>                         rows_;
    const core::UsageNode*                   rows_root_{};
    float                                    name_w_{};     // the widest name row (sets the sideways scroll)
    float                                    scroll_x_{};   // the list's sideways scroll, for the header
    bool                                     dirty_{true};  // rows_ must be rebuilt
    bool                                     follow_{};     // scroll the selected row into view
    int                                      sort_col_{1};  // 0 name, 1 size, 2 files
    bool                                     sort_asc_{false};
    unsigned                                 generation_{};
    bool                                     want_open_{};
    bool                                     shown_{};
};

} // namespace prism::ui
