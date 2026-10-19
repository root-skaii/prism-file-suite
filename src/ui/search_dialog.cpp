#include "ui/search_dialog.hpp"
#include "ui/tr.hpp"

#include "core/path.hpp"
#include "ui/tr.hpp"
#include "core/text.hpp"
#include "ui/tr.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace prism::ui {

using namespace strata;

void SearchDialog::open()
{
    want_open_ = true;
    age_       = 0;
    // an index that is old (or was never made) is rebuilt when the dialog opens; the old one stays searchable meanwhile
    if (!index_.building() && (!index_.ready() || index_.age_seconds() > 600.0)) { rebuild_(); }
}

void SearchDialog::activate(const core::IndexHit& hit)
{
    model::Tab& tab = env_.tab();
    if (hit.is_dir) {
        tab.navigate(hit.path);
    } else {
        tab.reveal_name = core::path::leaf(hit.path);
        tab.navigate(core::path::parent(hit.path));
    }
}

void SearchDialog::frame(Painter& p)
{
    context& ui = p.ui();
    if (want_open_) {
        want_open_ = false;
        open_      = true;
        sel_       = 0;
        Kit::reset_focus();
        ui.open_modal("Search everywhere");
    }
    if (!open_) { return; }
    const Palette&    pal = p.pal();
    const FontSet     f   = p.fonts();
    const DialogStyle style{ui, pal};
    const float width = std::min(980.0f, ui.display_size().x - 40.0f);
    auto modal = ui.modal("Search everywhere", {width, 0.0f}, modal_flags::esc_closes | modal_flags::backdrop_closes | modal_flags::no_title_bar);
    if (!modal) {
        open_ = false;
        Kit::reset_focus();
        return;
    }

    Kit kit{p, env_.cmd};
    kit.begin_frame();
    {
        const item_result r = ui.custom_item("##se_head", {ui.content_width() - 36.0f, 48.0f});
        p.text_left(rect{{r.bounds.min.x, r.bounds.min.y + 2.0f}, {r.bounds.max.x, r.bounds.min.y + 30.0f}}, tr("Search everywhere"), pal.text, f.title);
        std::string sub;
        if (index_.building() && !index_.ready()) { sub = trf("Indexing your drives for the first time ... {} items so far", index_.scanned()); }
        else if (index_.building()) { sub = trf("Refreshing the index ... {} items so far (searching the earlier one)", index_.scanned()); }
        else { sub = trf("{} items indexed, {} min ago", index_.entries(), static_cast<long long>(index_.age_seconds() / 60.0)); }
        p.text_left(rect{{r.bounds.min.x, r.bounds.min.y + 28.0f}, {r.bounds.max.x, r.bounds.max.y}}, sub, pal.text_dim, f.body);
        ui.same_line();
        if (kit.icon_button("##se_close", glyph::close, tr("Close  (Esc)"), true, true, 32.0f)) { ui.close_modal(); }
    }
    kit.vgap(8.0f);

    // the query box
    const float h = 38.0f;
    {
        const item_result anchor = ui.custom_item("##se_anchor", {1.0f, h});
        shape_style fs;
        fs.radius       = radii(8.0f);
        fs.fill_top     = fs.fill_bottom = pal.raised;
        fs.border       = pal.accent;
        fs.border_width = 1.0f;
        p.dl().shape(rect::from_size(anchor.bounds.min, {ui.content_width(), h}), fs);
        ui.same_line();
        ui.set_next_item_width(ui.content_width());
        if (age_ < 3) { ui.request_text_focus("##se_text"); }
        ++age_;
        ui.push_var(style_var::frame_padding_y, (h - p.line_height(f.body)) * 0.5f);
        ui.push_color(style_color::widget_bg, color{0, 0, 0, 0});
        (void)ui.input_text("##se_text", text_, tr("Type a name, *.ext, ext:pdf, size:>100mb, modified:7d ..."));
        ui.pop_color();
        ui.pop_var();
    }
    if (text_ != asked_) {
        asked_ = text_;
        index_.search(core::Query::parse(text_));
        sel_ = 0;
        follow_ = true;
    }
    if (auto r = index_.result(); r && r != shown_) { shown_ = std::move(r); }
    const core::SearchResult empty;
    const core::SearchResult& res = shown_ && shown_->text == asked_ ? *shown_ : (shown_ ? *shown_ : empty);
    const bool stale = !shown_ || shown_->text != asked_;

    // the line under the box
    {
        const item_result r = ui.custom_item("##se_status", {ui.content_width(), 28.0f});
        std::string line;
        if (text_.empty()) { line = tr("Names only. Words are all required; | means or; -word leaves it out."); }
        else if (stale && index_.searching()) { line = tr("Searching ..."); }
        else if (res.total == 0) { line = tr("Nothing found"); }
        else { line = trf("{} found{}  ({:.0f} ms)", res.total, res.total > res.hits.size() ? trf(", the first {} are listed", res.hits.size()) : std::string{}, res.milliseconds); }
        p.text_left(r.bounds, line, pal.text_dim, f.small, 4.0f);
    }

    const std::size_t rows = res.hits.size();
    const float rowh = 38.0f;
    const float list_h = std::clamp(ui.display_size().y - 330.0f, 160.0f, 560.0f);
    if (rows == 0) {
        kit.vgap(list_h * 0.5f);
        return;
    }
    const bool typing_nav = ui.field_focused("##se_text");
    if (typing_nav) {
        if (ui.key_pressed(key::down)) { sel_ = std::min<int>(sel_ + 1, static_cast<int>(rows) - 1); follow_ = true; }
        if (ui.key_pressed(key::up)) { sel_ = std::max(sel_ - 1, 0); follow_ = true; }
        if (ui.key_pressed(key::page_down)) { sel_ = std::min<int>(sel_ + 10, static_cast<int>(rows) - 1); follow_ = true; }
        if (ui.key_pressed(key::page_up)) { sel_ = std::max(sel_ - 10, 0); follow_ = true; }
    }
    sel_ = std::clamp(sel_, 0, static_cast<int>(rows) - 1);

    int  open_row = -1;
    {
        auto list = ui.child("##se_list", {ui.content_width() + 16.0f, list_h}, child_flags::no_padding);
        if (list) {
            const rect vp = ui.content_rect();
            const float total = rowh * static_cast<float>(rows);
            const item_result cv = ui.custom_item("##se_canvas", {ui.content_width(), std::max(total, vp.height() - 1.0f)});
            const bool dbl = ui.item_double_clicked();
            const vec2 origin = cv.bounds.min;
            if (follow_) {
                follow_ = false;
                const float top = rowh * static_cast<float>(sel_);
                const float scroll = ui.scroll_y();
                if (top < scroll) { ui.set_scroll_y(top); }
                else if (top + rowh > scroll + vp.height()) { ui.set_scroll_y(top + rowh - vp.height()); }
            }
            const vec2 mp = ui.mouse_pos();
            int hover = -1;
            if (cv.hovered && vp.contains(mp)) {
                hover = static_cast<int>(std::floor((mp.y - origin.y) / rowh));
                if (hover < 0 || hover >= static_cast<int>(rows)) { hover = -1; }
            }
            if (hover >= 0 && ui.mouse_clicked(mouse_button::left)) { sel_ = hover; }
            if (hover >= 0 && dbl) { open_row = hover; }
            if (hover >= 0) { env_.cmd.set_cursor(cursor_kind::hand); }

            const auto first = static_cast<std::size_t>(std::max(0.0f, std::floor((vp.min.y - origin.y) / rowh)));
            const auto last  = std::min(rows, static_cast<std::size_t>(std::max(0.0f, std::ceil((vp.max.y - origin.y) / rowh))) + 1);
            const float w = cv.bounds.width();
            for (std::size_t i = first; i < last; ++i) {
                const core::IndexHit& hit = res.hits[i];
                const float y = origin.y + static_cast<float>(i) * rowh;
                const rect row{{origin.x + 4.0f, y + 1.0f}, {origin.x + w - 4.0f, y + rowh - 1.0f}};
                const bool selected = static_cast<int>(i) == sel_;
                if (selected) { p.dl().rect_filled(row, pal.selection, 8.0f); }
                else if (static_cast<int>(i) == hover) { p.dl().rect_filled(row, pal.hover, 8.0f); }
                const std::string name = core::path::leaf(hit.path);
                const std::size_t dot = name.rfind('.');
                const core::FileKind kind = hit.is_dir ? core::FileKind::folder
                                          : (dot == std::string::npos || dot == 0 ? core::FileKind::other : core::classify_extension(core::text::lower_ascii(std::string_view{name}.substr(dot + 1))));
                p.kind_icon(rect::from_size({row.min.x + 10.0f, row.center().y - 10.0f}, {20.0f, 20.0f}), kind);
                p.text_left(rect{{row.min.x + 40.0f, row.min.y}, {row.min.x + w * 0.36f, row.max.y}}, name, pal.text, f.body);
                p.text_left(rect{{row.min.x + w * 0.36f, row.min.y}, {row.max.x - 110.0f, row.max.y}}, core::path::parent(hit.path), pal.text_dim, f.small);
                if (!hit.is_dir) { p.text_right(rect{{row.max.x - 110.0f, row.min.y}, {row.max.x, row.max.y}}, core::path::format_size(hit.size), pal.text_faint, f.small, 12.0f); }
            }
            if (hover >= 0 && static_cast<std::size_t>(hover) < rows) { ui.tooltip(res.hits[static_cast<std::size_t>(hover)].path); }
        }
    }
    if (typing_nav && ui.key_pressed(key::enter)) { open_row = sel_; }
    if (open_row >= 0 && static_cast<std::size_t>(open_row) < rows) {
        const core::IndexHit hit = res.hits[static_cast<std::size_t>(open_row)];
        activate(hit);
        open_ = false;
        ui.close_modal();
    }
}

} // namespace prism::ui
