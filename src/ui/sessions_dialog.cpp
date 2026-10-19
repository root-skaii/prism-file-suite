#include "ui/sessions_dialog.hpp"

#include "core/path.hpp"

#include <algorithm>
#include <format>

namespace prism::ui {

using namespace strata;

void SessionsDialog::frame(Painter& p)
{
    context& ui = p.ui();
    if (want_open_) {
        want_open_ = false;
        shown_     = true;
        Kit::reset_focus();
        ui.open_modal("Sessions");
    }
    if (!shown_) { return; }
    const Palette&    pal = p.pal();
    const FontSet     f   = p.fonts();
    const DialogStyle style{ui, pal};
    const float width = std::min(560.0f, ui.display_size().x - 40.0f);
    auto modal = ui.modal("Sessions", {width, 0.0f}, modal_flags::esc_closes | modal_flags::backdrop_closes | modal_flags::no_title_bar);
    if (!modal) {
        shown_ = false;
        Kit::reset_focus();
        return;
    }

    Kit kit{p, env_.cmd};
    kit.begin_frame();
    {
        const item_result r = ui.custom_item("##ss_head", {ui.content_width() - 36.0f, 48.0f});
        p.text_left(rect{{r.bounds.min.x, r.bounds.min.y + 2.0f}, {r.bounds.max.x, r.bounds.min.y + 30.0f}}, "Sessions", pal.text, f.title);
        p.text_left(rect{{r.bounds.min.x, r.bounds.min.y + 28.0f}, {r.bounds.max.x, r.bounds.max.y}}, "The tabs and panes you have open, saved under a name", pal.text_dim, f.body);
        ui.same_line();
        if (kit.icon_button("##ss_close", glyph::close, "Close  (Esc)", true, true, 32.0f)) { ui.close_modal(); }
    }
    kit.vgap(10.0f);

    // save the current set: a name box and a button
    const float h        = 36.0f;
    const float bw_save  = kit.button_width("Save current", false, Kit::ButtonKind::primary);
    const float field_w  = ui.content_width() - bw_save - 8.0f - 16.0f;
    shape_style fs;
    fs.radius       = radii(8.0f);
    fs.fill_top     = fs.fill_bottom = pal.raised;
    fs.border       = pal.border;
    fs.border_width = 1.0f;
    {
        const item_result anchor = ui.custom_item("##ss_anchor", {1.0f, h});
        p.dl().shape(rect::from_size(anchor.bounds.min, {field_w, h}), fs);
        ui.same_line();
        ui.set_next_item_width(field_w);
        if (age_ < 3) { ui.request_text_focus("##ss_name"); }
        ++age_;
        ui.push_var(style_var::frame_padding_y, (h - p.line_height(f.body)) * 0.5f);
        ui.push_color(style_color::widget_bg, color{0, 0, 0, 0});
        (void)ui.input_text("##ss_name", name_, "Name for the current tabs");
        ui.pop_color();
        ui.pop_var();
    }
    ui.same_line();
    (void)ui.custom_item("##ss_gap", {8.0f, h});
    ui.same_line();
    const bool can_save = !name_.empty();
    const bool enter    = can_save && ui.field_focused("##ss_name") && ui.key_pressed(key::enter);
    if ((kit.button({"##ss_save", "Save current", 0, Kit::ButtonKind::primary, can_save}) && can_save) || enter) {
        const bool again = store_.find(name_).has_value();
        if (store_.save(capture_(name_))) {
            env_.cmd.toast(std::format("{} \"{}\"", again ? "Updated" : "Saved", name_), toast_kind::success);
            name_.clear();
            ui.invalidate(); // (the box and the list change under an idle ui)
        }
    }
    kit.vgap(14.0f);

    // the saved sessions
    const auto& all = store_.all();
    if (all.empty()) {
        const item_result r = ui.custom_item("##ss_none", {ui.content_width(), 64.0f});
        p.text_center(r.bounds, "No sessions yet", pal.text_faint, f.body);
        return;
    }
    const float rowh   = 52.0f;
    const float list_h = std::min(rowh * static_cast<float>(all.size()), std::clamp(ui.display_size().y - 360.0f, 120.0f, 360.0f));
    std::string open_name, remove_name;
    {
        auto list = ui.child("##ss_list", {ui.content_width() + (rowh * static_cast<float>(all.size()) > list_h ? 16.0f : 0.0f), list_h}, child_flags::no_padding);
        if (list) {
            for (std::size_t i = 0; i < all.size(); ++i) {
                const model::Session& s = all[i];
                const std::string     id = std::to_string(i);
                const float bw_open = kit.button_width("Open", false);
                const item_result r = ui.custom_item("##ss_row" + id, {ui.content_width() - bw_open - 40.0f - 16.0f, rowh});
                p.text_left(rect{{r.bounds.min.x + 4.0f, r.bounds.min.y + 6.0f}, {r.bounds.max.x, r.bounds.min.y + 28.0f}}, s.name, pal.text, f.bold);
                std::string sub = std::format("{} {}", s.tabs.size(), s.tabs.size() == 1 ? "tab" : "tabs");
                if (!s.right.empty()) { sub += ", two panes"; }
                sub += "   " + core::path::leaf(s.tabs[static_cast<std::size_t>(s.active)]);
                p.text_left(rect{{r.bounds.min.x + 4.0f, r.bounds.min.y + 28.0f}, r.bounds.max}, sub, pal.text_dim, f.small);
                ui.same_line();
                if (kit.button({("##ss_open" + id).c_str(), "Open"})) { open_name = s.name; }
                ui.same_line();
                if (kit.icon_button("##ss_del" + id, glyph::delete_, "Delete this session", true, true, kit.control_h())) { remove_name = s.name; }
            }
        }
    }
    if (!remove_name.empty() && store_.remove(remove_name)) { ui.invalidate(); }
    if (!open_name.empty()) {
        if (const auto s = store_.find(open_name)) {
            restore_(*s);
            ui.close_modal();
        }
    }
}

} // namespace prism::ui
