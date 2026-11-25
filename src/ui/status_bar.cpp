#include "ui/status_bar.hpp"

#include "core/git_status.hpp"
#include "core/path.hpp"
#include "ui/panel.hpp"
#include "ui/tr.hpp"

#include <algorithm>
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

    // the file card ends 2 px above this bar and the window has a border at the bottom: the line is centred between them
    const rect line{{area.min.x, area.min.y - 2.0f}, {area.max.x, area.max.y - 2.0f}};

    // compact view-mode toggle icons (Details / Thumbnails) anchored on the far right
    const float icon_btn = std::min(22.0f, line.height() - 2.0f);
    const float btn_y    = line.center().y - icon_btn * 0.5f;
    float right_edge     = area.max.x - 10.0f;
    const auto view_btn  = [&](char32_t g, bool on, std::string_view tip) {
        const rect r{{right_edge - icon_btn, btn_y}, {right_edge, btn_y + icon_btn}};
        const bool over = hit.hovered && r.contains(ui.mouse_pos());
        if (on) { p.dl().rect_filled(r, pal.accent.scaled_alpha(0.18f), 5.0f); }
        else if (over) { p.dl().rect_filled(r, pal.hover, 5.0f); }
        p.glyph_center(r, g, on ? pal.accent : (over ? pal.text : pal.text_faint), env_.fonts.tile);
        if (over) {
            env_.cmd.set_cursor(cursor_kind::hand);
            ui.tooltip(tip);
        }
        right_edge -= icon_btn + 2.0f;
        return over && ui.mouse_released(mouse_button::left);
    };
    const bool grid = env_.settings.grid_view;
    if (view_btn(glyph::grid, grid, tr("Thumbnails")) && !grid) { env_.cmd.toggle_view(); }
    if (view_btn(glyph::list, !grid, tr("Details")) && grid)    { env_.cmd.toggle_view(); }
    right_edge -= 8.0f;

    // right-side status (search cancel button, loading indicator, or Git branch summary) — measured so left text never overlaps
    if (tab.searching()) {
        const std::string_view label = tab.loading() ? tr("Cancel search") : tr("Back to folder");
        const float w     = p.measure(env_.fonts.small, label).x + 20.0f;
        const rect  btn{{right_edge - w, line.min.y + 3.0f}, {right_edge, line.max.y - 3.0f}};
        const bool  over = hit.hovered && btn.contains(ui.mouse_pos());
        if (over) { p.dl().rect_filled(btn, pal.hover, 6.0f); env_.cmd.set_cursor(cursor_kind::hand); }
        p.text_center(btn, label, over ? pal.text : pal.accent, env_.fonts.small);
        if (over && ui.mouse_released(mouse_button::left)) { tab.clear_search(); }
        right_edge = btn.min.x - 10.0f;
    } else if (tab.loading()) {
        const std::string_view lbl = tr("Loading...");
        const float w = p.measure(env_.fonts.small, lbl).x;
        p.text_right(rect{line.min, {right_edge, line.max.y}}, lbl, pal.accent, env_.fonts.small, 0.0f);
        right_edge -= w + 12.0f;
    } else if (env_.settings.git_badges) {
        if (const auto repo = env_.git.lookup(tab.path()); repo && !repo->branch.empty()) {
            std::string g = "git " + repo->branch;
            if (repo->ahead > 0) { g += std::format("  +{}", repo->ahead); }
            if (repo->behind > 0) { g += std::format("  -{}", repo->behind); }
            const std::size_t changed = repo->files.size() + repo->untracked_dirs.size();
            g += changed == 0 ? "  " + std::string{tr("clean")} : trf("  {} changed", changed);
            const float w = p.measure(env_.fonts.small, g).x;
            p.text_right(rect{line.min, {right_edge, line.max.y}}, g, changed == 0 ? pal.text_dim : pal.warning, env_.fonts.small, 0.0f);
            right_edge -= w + 14.0f;
        }
    }

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
    p.text_left(rect{line.min, {std::max(line.min.x + 80.0f, right_edge), line.max.y}}, left, left_col, env_.fonts.small, 16.0f);
}

} // namespace prism::ui
