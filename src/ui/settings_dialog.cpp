#include "ui/settings_dialog.hpp"

#include <algorithm>
#include <array>

namespace prism::ui {

using namespace strata;

namespace {

constexpr std::array<int, 5> k_scales{90, 100, 110, 125, 150};
constexpr std::array<unsigned, 8> k_accents{0x6d8cff, 0x8b5cf6, 0xec4899, 0xef4444, 0xf59e0b, 0x22c55e, 0x14b8a6, 0x0ea5e9};

// a settings row: the label (and a hint) on the left, then whatever `control` draws on the same line. `control_w` is
// the width it will take, so the label gets the rest
template <class Fn>
bool setting_row(Kit& kit, std::string_view id, std::string_view label, std::string_view hint, float control_w, bool separator, Fn control)
{
    Painter&       p   = kit.painter();
    context&       ui  = p.ui();
    const Palette& pal = p.pal();
    const float h = kit.row_h() + 12.0f;
    const float w = ui.content_width();
    const item_result r = ui.custom_item(id, {std::max(60.0f, w - control_w - Kit::pad() * 2.0f), h});
    const bool hinted = !hint.empty();
    p.text_left(rect{{r.bounds.min.x, r.bounds.min.y + (hinted ? 6.0f : 0.0f)}, {r.bounds.max.x, r.bounds.min.y + (hinted ? h * 0.55f : h)}}, label, pal.text, p.fonts().body, Kit::pad());
    if (hinted) { p.text_left(rect{{r.bounds.min.x, r.bounds.min.y + h * 0.5f}, {r.bounds.max.x, r.bounds.max.y - 4.0f}}, hint, pal.text_dim, p.fonts().small, Kit::pad()); }
    if (separator) { p.dl().rect_filled(rect{{r.bounds.min.x + Kit::pad(), r.bounds.max.y - 1.0f}, {r.bounds.min.x + w - Kit::pad(), r.bounds.max.y}}, pal.divider); }
    ui.same_line();
    const bool changed = control(h);
    ui.same_line();
    (void)ui.custom_item(std::string{id} + "_pad", {Kit::pad(), h});
    return changed;
}

} // namespace

bool SettingsDialog::frame(Painter& p)
{
    context& ui = p.ui();
    Settings& s = env_.settings;
    bool changed = false;

    if (want_open_) {
        want_open_ = false;
        shown_     = true;
        Kit::reset_focus();
        ui.open_modal("Settings");
    }
    if (!shown_) { return false; }
    const Palette& pal = p.pal();
    const FontSet  f   = p.fonts();
    const DialogStyle style{ui, pal};
    const float width = std::min(620.0f, ui.display_size().x - 40.0f);
    auto modal = ui.modal("Settings", {width, 0.0f}, modal_flags::esc_closes | modal_flags::backdrop_closes | modal_flags::no_title_bar);
    if (!modal) {
        shown_ = false;
        Kit::reset_focus();
        return false;
    }

    Kit kit{p, env_.cmd};
    kit.begin_frame();

    // header
    {
        const item_result r = ui.custom_item("##set_head", {ui.content_width() - 36.0f, 48.0f});
        p.text_left(rect{{r.bounds.min.x, r.bounds.min.y + 2.0f}, {r.bounds.max.x, r.bounds.min.y + 30.0f}}, "Settings", pal.text, f.title);
        p.text_left(rect{{r.bounds.min.x, r.bounds.min.y + 28.0f}, {r.bounds.max.x, r.bounds.max.y}}, "Changes apply and save at once", pal.text_dim, f.body);
        ui.same_line();
        if (kit.icon_button("##set_close", glyph::close, "Close  (Esc)", true, true, 32.0f)) { ui.close_modal(); }
    }
    kit.vgap(10.0f);

    const float max_h  = std::clamp(ui.display_size().y - 250.0f, 260.0f, 640.0f);
    const float body_h = std::min(max_h, content_h_ + 2.0f);
    {
        auto body = ui.child("##set_body", {ui.content_width(), body_h}, child_flags::no_padding);
        if (body) {
            const float rh = kit.row_h() + 12.0f;

            // appearance
            kit.card_begin("##card_look", "Appearance", rh * 6.0f);
            int theme = s.dark ? 0 : 1;
            static constexpr std::array<std::string_view, 2> themes{"Dark", "Light"};
            if (setting_row(kit, "##r_theme", "Theme", {}, 180.0f, true, [&](float h) { return kit.pill_tabs("##t_theme", themes, theme, 180.0f, h); })) {
                s.dark = theme == 0;
                changed = true;
            }
            int style_i = s.dark_style;
            static constexpr std::array<std::string_view, 3> styles{"Neutral", "Graphite", "Slate"};
            if (setting_row(kit, "##r_dstyle", "Dark style", s.dark ? std::string_view{} : std::string_view{"Used by the dark theme"}, 270.0f, true,
                            [&](float h) { return kit.pill_tabs("##t_dstyle", styles, style_i, 270.0f, h); })) {
                s.dark_style = style_i;
                changed = true;
            }
            int density = s.compact ? 1 : 0;
            static constexpr std::array<std::string_view, 2> densities{"Comfortable", "Compact"};
            if (setting_row(kit, "##r_density", "Density", {}, 240.0f, true, [&](float h) { return kit.pill_tabs("##t_density", densities, density, 240.0f, h); })) {
                s.compact = density == 1;
                changed = true;
            }
            int scale = 1;
            for (std::size_t i = 0; i < k_scales.size(); ++i) { if (k_scales[i] == s.ui_scale) { scale = static_cast<int>(i); } }
            static constexpr std::array<std::string_view, 5> scales{"90 %", "100 %", "110 %", "125 %", "150 %"};
            if (setting_row(kit, "##r_scale", "Interface size", {}, 330.0f, true, [&](float h) { return kit.pill_tabs("##t_scale", scales, scale, 330.0f, h); })) {
                s.ui_scale = k_scales[static_cast<std::size_t>(scale)];
                changed = true;
            }
            const float sw_total = 28.0f * static_cast<float>(k_accents.size());
            setting_row(kit, "##r_accent", "Accent colour", {}, sw_total, true, [&](float h) {
                for (std::size_t i = 0; i < k_accents.size(); ++i) {
                    const unsigned rgb = k_accents[i];
                    if (i > 0) { ui.same_line(); }
                    const color c{static_cast<u8>(rgb >> 16), static_cast<u8>(rgb >> 8), static_cast<u8>(rgb), 255};
                    if (kit.swatch("##acc" + std::to_string(i), c, s.accent == rgb, h)) {
                        s.accent = rgb;
                        changed  = true;
                    }
                }
                return false;
            });
            // any other colour: the swatch opens a full picker
            setting_row(kit, "##r_custom", "Custom colour", {}, 150.0f, false, [&](float h) {
                color c{static_cast<u8>(s.accent >> 16), static_cast<u8>(s.accent >> 8), static_cast<u8>(s.accent), 255};
                ui.set_next_item_width(150.0f);
                ui.push_var(style_var::frame_padding_y, std::max(2.0f, (kit.control_h() - p.line_height(f.body)) * 0.5f));
                const bool edited = ui.color_edit("##accent", c, color_flags::no_alpha);
                ui.pop_var();
                if (edited) {
                    s.accent = (static_cast<unsigned>(c.r) << 16) | (static_cast<unsigned>(c.g) << 8) | c.b;
                    changed  = true;
                }
                (void)h;
                return false;
            });
            kit.card_end("##card_look");

            // behaviour
            kit.card_begin("##card_beh", "Behavior", (kit.row_h() + 16.0f) * 3.0f);
            if (kit.switch_row("##sw_hidden", "Show hidden files", "Hidden and system items appear in every folder", s.show_hidden, true)) { changed = true; }
            if (kit.switch_row("##sw_confirm", "Ask before moving to the Recycle Bin", "Deleting for good always asks", s.confirm_delete, true)) { changed = true; }
            if (kit.switch_row("##sw_tabs", "Reopen tabs on start", "Pick up where you left off", s.restore_tabs, false)) { changed = true; }
            kit.card_end("##card_beh");

            // shortcuts
            // the table is laid out inline (height 0): a scroller inside the page's scroller would take the wheel whenever
            // the pointer crossed it and stall the page. its height is measured and the card follows one frame behind
            kit.card_begin("##card_keys", "Keyboard shortcuts", 32.0f + keys_h_ + 8.0f);
            const item_result hint = ui.custom_item("##keys_hint", {ui.content_width(), 32.0f});
            p.text_left(hint.bounds, "Click a field and press the new keys.  Ctrl+Shift+P searches every command.", pal.text_dim, f.small, Kit::pad());
            (void)ui.custom_item("##keys_lead", {Kit::pad(), 1.0f});
            ui.same_line();
            ui.set_next_item_width(ui.content_width() - Kit::pad() * 2.0f);
            if (strata::keybind_editor(ui, env_.binds, "pfs_keys")) { changed = true; }
            const item_result keys_end = ui.custom_item("##keys_end", {1.0f, 8.0f});
            keys_h_ = keys_end.bounds.min.y - hint.bounds.max.y;
            kit.card_end("##card_keys");

            // about
            const std::string file = Settings::file_path();
            kit.card_begin("##card_about", "About", kit.row_h() * 2.0f);
            kit.key_value({"##ab_app", "PrismFS", "A fast file explorer, built on the strata UI framework", false, false, true, 90.0f});
            kit.key_value({"##ab_file", "Settings file", file, false, true, false, 90.0f});
            kit.card_end("##card_about");
            content_h_ = kit.cards_height();
        }
    }
    kit.vgap(12.0f);
    const float close_w = 96.0f;
    (void)ui.custom_item("##set_foot", {ui.content_width() - close_w, kit.control_h()});
    ui.same_line();
    if (kit.button({"##set_done", "Close", 0, Kit::ButtonKind::primary, true, {}, close_w})) { ui.close_modal(); }
    return changed;
}

} // namespace prism::ui
