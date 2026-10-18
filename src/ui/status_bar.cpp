#include "ui/status_bar.hpp"

#include "core/path.hpp"
#include "ui/panel.hpp"

#include <format>

namespace prism::ui {

using namespace strata;

void StatusBar::frame(Painter& p, const rect& area)
{
    context&              ui  = p.ui();
    const Palette&        pal = p.pal();
    const model::Tab&     tab = env_.tabs.active();
    const model::Listing& L   = tab.listing();

    PanelScope panel{ui, "##status", area};
    p.dl().rect_filled(area, pal.window);

    std::string left;
    if (L.rows() == 1) {
        left = "1 item";
    } else {
        left = std::format("{} items", L.rows());
    }
    if (L.total() > L.rows()) { left += std::format("  ({} hidden or filtered)", L.total() - L.rows()); }
    if (L.selected_count() > 0) {
        left += std::format("   |   {} selected", L.selected_count());
        if (L.selected_bytes() > 0) { left += "  " + core::path::format_size(L.selected_bytes()); }
    }
    p.text_left(rect{area.min, {area.max.x - 160.0f, area.max.y}}, left, pal.text_dim, env_.fonts.small, 16.0f);

    if (tab.loading()) {
        p.text_right(area, "Loading...", pal.accent, env_.fonts.small, 16.0f);
    }
}

} // namespace prism::ui
