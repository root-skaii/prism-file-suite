#include "ui/empty_folders_dialog.hpp"
#include "ui/tr.hpp"

#include "core/path.hpp"
#include "ui/tr.hpp"

#include <algorithm>
#include <format>

namespace prism::ui {

using namespace strata;

void EmptyFoldersDialog::open(std::string folder)
{
    if (folder.empty()) { return; }
    folder_    = std::move(folder);
    loaded_    = false;
    want_open_ = true;
    found_.clear();
    marked_.clear();
    scan_.start(folder_, env_.window.waker());
}

void EmptyFoldersDialog::frame(Painter& p)
{
    context& ui = p.ui();
    if (want_open_) {
        want_open_ = false;
        open_      = true;
        Kit::reset_focus();
        ui.open_modal("Empty folders");
    }
    if (!open_) { return; }
    const Palette&    pal = p.pal();
    const FontSet     f   = p.fonts();
    const DialogStyle style{ui, pal};
    auto modal = ui.modal("Empty folders", {std::min(720.0f, ui.display_size().x - 40.0f), 0.0f}, modal_flags::esc_closes | modal_flags::no_title_bar);
    if (!modal) {
        open_ = false;
        scan_.cancel();
        Kit::reset_focus();
        return;
    }
    if (scan_.done() && !loaded_) {
        found_  = scan_.result();
        marked_ = std::set<std::string>(found_.begin(), found_.end());
        loaded_ = true;
    }

    Kit kit{p, env_.cmd};
    kit.begin_frame();
    {
        const item_result r = ui.custom_item("##ef_head", {ui.content_width() - 36.0f, 48.0f});
        p.text_left(rect{{r.bounds.min.x, r.bounds.min.y + 2.0f}, {r.bounds.max.x, r.bounds.min.y + 30.0f}}, tr("Empty folders"), pal.text, f.title);
        p.text_left(rect{{r.bounds.min.x, r.bounds.min.y + 28.0f}, {r.bounds.max.x, r.bounds.max.y}}, folder_ + "  " + std::string{tr("(a folder with only empty folders in it counts)")}, pal.text_dim, f.body);
        ui.same_line();
        if (kit.icon_button("##ef_close", glyph::close, tr("Close  (Esc)"), true, true, 32.0f)) { ui.close_modal(); }
    }
    kit.vgap(10.0f);

    if (!loaded_) {
        const item_result r = ui.custom_item("##ef_run", {ui.content_width(), 34.0f});
        p.text_left(r.bounds, trf("Looking ... {} folders so far", scan_.folders_seen()), pal.text_dim, f.body, 4.0f);
        kit.bar("##ef_bar", 0.0f, Tone::accent, 6.0f, 4.0f);
        kit.vgap(14.0f);
        if (kit.button({"##ef_cancel", tr("Cancel")})) { ui.close_modal(); }
        return;
    }
    if (found_.empty()) {
        const item_result r = ui.custom_item("##ef_none", {ui.content_width(), 64.0f});
        p.text_center(r.bounds, tr("No empty folders here."), pal.text_dim, f.body);
        return;
    }

    const float rowh   = 32.0f;
    const std::size_t shown = std::min<std::size_t>(found_.size(), 1000);
    const float list_h = std::min(rowh * static_cast<float>(shown), std::clamp(ui.display_size().y - 340.0f, 140.0f, 420.0f));
    std::string toggle;
    {
        auto list = ui.child("##ef_list", {ui.content_width() + (rowh * static_cast<float>(shown) > list_h ? 16.0f : 0.0f), list_h}, child_flags::no_padding);
        if (list) {
            for (std::size_t i = 0; i < shown; ++i) {
                const std::string& path = found_[i];
                const bool mark = marked_.contains(path);
                const item_result r = ui.custom_item("##ef_row" + std::to_string(i), {ui.content_width(), rowh});
                if (r.hovered) { p.dl().rect_filled(r.bounds, pal.hover, 6.0f); env_.cmd.set_cursor(cursor_kind::hand); ui.tooltip(path); }
                const rect box = rect::from_size({r.bounds.min.x + 12.0f, r.bounds.center().y - 9.0f}, {18.0f, 18.0f});
                if (mark) { p.dl().rect_filled(box, pal.accent, 4.0f); p.glyph_center(box, glyph::check, pal.window, f.tile); }
                else { p.dl().rect_outline(box, pal.text_faint, 4.0f, 1.5f); }
                std::string rel = path.size() > folder_.size() + 1 && path.compare(0, folder_.size(), folder_) == 0 ? path.substr(folder_.size() + (folder_.back() == '\\' ? 0 : 1)) : path;
                p.text_left(rect{{box.max.x + 10.0f, r.bounds.min.y}, r.bounds.max}, rel, mark ? pal.text : pal.text_dim, f.body);
                if (r.hovered && ui.mouse_released(mouse_button::left)) { toggle = path; }
            }
        }
    }
    if (!toggle.empty()) {
        if (marked_.contains(toggle)) { marked_.erase(toggle); } else { marked_.insert(toggle); }
        ui.invalidate();
    }
    kit.vgap(12.0f);

    const float bw_cancel = kit.button_width(tr("Cancel"), false);
    const float bw_ok     = kit.button_width(trf("Recycle {} folders", 1000), false, Kit::ButtonKind::primary);
    const item_result sr  = ui.custom_item("##ef_sum", {ui.content_width() - bw_cancel - bw_ok - 24.0f, kit.control_h()});
    p.text_left(sr.bounds, trf("{} empty folders found{}", found_.size(), found_.size() > shown ? tr(" (the first 1000 are listed)") : std::string_view{}), pal.text_dim, f.body, 4.0f);
    ui.same_line();
    if (kit.button({"##ef_no", tr("Cancel")})) { ui.close_modal(); }
    ui.same_line();
    (void)ui.custom_item("##ef_gap", {8.0f, kit.control_h()});
    ui.same_line();
    std::size_t count = 0;
    for (std::size_t i = 0; i < shown; ++i) { count += marked_.contains(found_[i]) ? 1 : 0; }
    const std::string label = trf_n("Recycle {} folder", "Recycle {} folders", static_cast<long long>(count), count);
    if (kit.button({"##ef_go", label, 0, Kit::ButtonKind::primary, count > 0, {}, bw_ok}) && count > 0) {
        std::vector<std::string> paths;
        for (std::size_t i = 0; i < shown; ++i) {
            if (marked_.contains(found_[i])) { paths.push_back(found_[i]); }
        }
        env_.ops.start(core::OpKind::recycle, std::move(paths), {});
        ui.close_modal();
    }
}

} // namespace prism::ui
