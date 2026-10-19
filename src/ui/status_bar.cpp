#include "ui/status_bar.hpp"
#include "ui/tr.hpp"

#include "core/path.hpp"
#include "ui/tr.hpp"
#include "core/git_status.hpp"
#include "ui/tr.hpp"
#include "ui/panel.hpp"
#include "ui/tr.hpp"

#include <format>

namespace prism::ui {

using namespace strata;

void StatusBar::frame(Painter& p, const rect& area)
{
    context&              ui  = p.ui();
    const Palette&        pal = p.pal();
    model::Tab&           tab = env_.tab();
    const model::Listing& L   = tab.listing();

    PanelScope panel{ui, "##status", area};
    p.dl().rect_filled(area, pal.window);
    const item_result hit = ui.custom_item("##status_hit", area.size());

    std::string left;
    color       left_col = pal.text_dim;
    if (!L.query().valid()) {
        left     = trf("Invalid search: {}", L.query().error());
        left_col = pal.danger;
    } else if (tab.searching()) {
        const long long rows = static_cast<long long>(L.rows());
        left = tab.loading() ? trf_n("Searching \"{}\":  {} result found", "Searching \"{}\":  {} results found", rows, tab.search_query(), L.rows())
                             : trf_n("Search \"{}\":  {} result", "Search \"{}\":  {} results", rows, tab.search_query(), L.rows());
    } else {
        left = trf_n("{} item", "{} items", static_cast<long long>(L.rows()), L.rows());
        if (L.total() > L.rows()) { left += trf("  ({} hidden or filtered)", L.total() - L.rows()); }
    }
    if (L.selected_count() > 0) {
        left += "   |   " + trf("{} selected", L.selected_count());
        if (L.selected_bytes() > 0) { left += "  " + core::path::format_size(L.selected_bytes()); }
    }
    p.text_left(rect{area.min, {area.max.x - 160.0f, area.max.y}}, left, left_col, env_.fonts.small, 16.0f);

    // a running search can be stopped, a finished one dismissed
    if (tab.searching()) {
        const std::string_view label = tab.loading() ? tr("Cancel search") : tr("Back to folder");
        const float w     = p.measure(env_.fonts.small, label).x + 20.0f;
        const rect  btn{{area.max.x - w - 12.0f, area.min.y + 3.0f}, {area.max.x - 12.0f, area.max.y - 3.0f}};
        const bool  over = hit.hovered && btn.contains(ui.mouse_pos());
        if (over) { p.dl().rect_filled(btn, pal.hover, 6.0f); env_.cmd.set_cursor(cursor_kind::hand); }
        p.text_center(btn, label, over ? pal.text : pal.accent, env_.fonts.small);
        if (over && ui.mouse_released(mouse_button::left)) { tab.clear_search(); }
    } else if (tab.loading()) {
        p.text_right(area, tr("Loading..."), pal.accent, env_.fonts.small, 16.0f);
    } else if (env_.settings.git_badges) {
        // the branch of the repository this folder is in, with how far it is from its upstream and how much is changed
        if (const auto repo = env_.git.lookup(tab.path()); repo && !repo->branch.empty()) {
            std::string g = "git " + repo->branch;
            if (repo->ahead > 0) { g += std::format("  +{}", repo->ahead); }
            if (repo->behind > 0) { g += std::format("  -{}", repo->behind); }
            const std::size_t changed = repo->files.size() + repo->untracked_dirs.size();
            g += changed == 0 ? "  " + std::string{tr("clean")} : trf("  {} changed", changed);
            p.text_right(area, g, changed == 0 ? pal.text_dim : pal.warning, env_.fonts.small, 16.0f);
        }
    }
}

} // namespace prism::ui
