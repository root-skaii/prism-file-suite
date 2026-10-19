#include "ui/transfers_ui.hpp"
#include "ui/tr.hpp"

#include "core/path.hpp"
#include "ui/tr.hpp"
#include "ui/panel.hpp"
#include "ui/tr.hpp"

#include <algorithm>
#include <format>

namespace prism::ui {

using namespace strata;

namespace {

[[nodiscard]] std::string clock_text(double seconds)
{
    const auto s = static_cast<long long>(seconds + 0.5);
    if (s >= 3600) { return std::format("{}:{:02}:{:02}", s / 3600, (s / 60) % 60, s % 60); }
    return std::format("{}:{:02}", s / 60, s % 60);
}

} // namespace

void TransfersUi::frame_card(Painter& p, const rect& area)
{
    const core::TransferProgress t = env_.ops.transfers().progress();
    if (!t.active) { return; }
    context&       ui  = p.ui();
    const Palette& pal = p.pal();
    const FontSet& f   = env_.fonts;

    const float w = std::min(420.0f, area.width() - 24.0f);
    const float h = 96.0f;
    // bottom centre: the toasts stack in the corner
    const rect  card = rect::from_size({area.center().x - w * 0.5f, area.max.y - h - 12.0f}, {w, h});

    PanelScope panel{ui, "##transfers", card};
    auto fg = ui.layer(layer::foreground);
    p.dl().rect_filled(card.expanded(2.0f), color{0, 0, 0, 70}, 14.0f);
    shape_style bg;
    bg.radius       = radii(12.0f);
    bg.fill_top     = bg.fill_bottom = pal.raised;
    bg.border       = pal.border;
    bg.border_width = 1.0f;
    p.dl().shape(card, bg);

    const float pad = 14.0f;
    const float btn = 28.0f;
    const rect  cancel_r = rect::from_size({card.max.x - pad - btn, card.min.y + 10.0f}, {btn, btn});
    const rect  pause_r  = rect::from_size({cancel_r.min.x - 4.0f - btn, card.min.y + 10.0f}, {btn, btn});
    const vec2  mp = ui.mouse_pos();
    const auto  button = [&](const rect& r, char32_t g, std::string_view tip) {
        const bool over = r.contains(mp);
        if (over) {
            p.dl().rect_filled(r, pal.hover, 7.0f);
            env_.cmd.set_cursor(cursor_kind::hand);
            ui.tooltip(tip);
        }
        p.glyph_center(r, g, pal.text, f.icons);
        return over && ui.mouse_released(mouse_button::left);
    };

    p.text_left(rect{{card.min.x + pad, card.min.y + 8.0f}, {pause_r.min.x - 6.0f, card.min.y + 34.0f}}, t.title, pal.text, f.bold);

    // the bar: filled by bytes; a pulse while still counting
    const rect track = rect::from_size({card.min.x + pad, card.min.y + 42.0f}, {w - pad * 2.0f, 6.0f});
    p.dl().rect_filled(track, pal.window, 3.0f);
    float fraction = t.bytes_total > 0 ? std::clamp(static_cast<float>(static_cast<double>(t.bytes_done) / static_cast<double>(t.bytes_total)), 0.0f, 1.0f) : 0.0f;
    fraction = ui.animate("transfer_bar", fraction, 14.0f);
    if (t.scanning) {
        const float x = std::fmod(static_cast<float>(ui.time()) * 0.9f, 1.0f);
        const float len = track.width() * 0.25f;
        p.dl().push_clip(track);
        p.dl().rect_filled(rect::from_size({track.min.x + (track.width() + len) * x - len, track.min.y}, {len, track.height()}), pal.accent, 3.0f);
        p.dl().pop_clip();
        ui.invalidate(); // (the pulse keeps moving while it counts)
    } else if (fraction > 0.0f) {
        p.dl().rect_filled(rect::from_size(track.min, {track.width() * fraction, track.height()}), pal.accent, 3.0f);
    }

    std::string detail;
    if (t.scanning) {
        detail = tr("Counting what to copy ...");
    } else if (t.paused) {
        detail = tr("Paused");
    } else {
        detail = std::format("{} of {}", core::path::format_size(t.bytes_done), core::path::format_size(t.bytes_total));
        if (t.bytes_per_second > 1.0) {
            detail += std::format("  -  {}/s", core::path::format_size(static_cast<std::uint64_t>(t.bytes_per_second)));
            if (t.bytes_total > t.bytes_done) { detail += trf("  -  {} left", clock_text(static_cast<double>(t.bytes_total - t.bytes_done) / t.bytes_per_second)); }
        }
    }
    p.text_left(rect{{card.min.x + pad, card.min.y + 52.0f}, {card.max.x - pad, card.min.y + 72.0f}}, detail, pal.text_dim, f.small);
    std::string foot = t.current;
    if (t.waiting > 0) { foot = trf("{} more waiting  -  {}", t.waiting, t.current); }
    p.text_left(rect{{card.min.x + pad, card.min.y + 72.0f}, {card.max.x - pad, card.max.y - 6.0f}}, foot, pal.text_faint, f.small);

    if (button(pause_r, t.paused ? glyph::play : glyph::pause, t.paused ? tr("Resume") : tr("Pause"))) { env_.ops.transfers().set_paused(!t.paused); ui.invalidate(); }
    if (button(cancel_r, glyph::close, tr("Cancel the copy"))) { env_.ops.transfers().cancel(); ui.invalidate(); }
}

void TransfersUi::frame_conflict(Painter& p)
{
    context& ui = p.ui();
    core::TransferQueue& q = env_.ops.transfers();
    const auto info = q.conflict();
    const bool fresh = info && info->serial != answered_;
    if (fresh && !open_) {
        open_    = true;
        for_all_ = false;
        Kit::reset_focus();
        ui.open_modal("File exists");
    }
    if (!open_) { return; }

    const Palette&    pal = p.pal();
    const FontSet     f   = p.fonts();
    const DialogStyle style{ui, pal};
    auto modal = ui.modal("File exists", {std::min(560.0f, ui.display_size().x - 40.0f), 0.0f}, modal_flags::esc_closes | modal_flags::no_title_bar);
    if (!modal) { // (Esc: stop the whole copy)
        open_ = false;
        Kit::reset_focus();
        if (info && info->serial != answered_) { answered_ = info->serial; q.resolve(info->serial, core::ConflictChoice::cancel, false); }
        return;
    }
    if (!fresh) { // (answered or cancelled from elsewhere)
        ui.close_modal();
        return;
    }

    Kit kit{p, env_.cmd};
    kit.begin_frame();
    {
        const item_result r = ui.custom_item("##tc_head", {ui.content_width(), 50.0f});
        p.text_left(rect{{r.bounds.min.x, r.bounds.min.y + 2.0f}, {r.bounds.max.x, r.bounds.min.y + 30.0f}}, tr("This file already exists"), pal.text, f.title);
        p.text_left(rect{{r.bounds.min.x, r.bounds.min.y + 30.0f}, {r.bounds.max.x, r.bounds.max.y}}, trf("{}  in  {}", core::path::leaf(info->target), core::path::leaf(core::path::parent(info->target))), pal.text_dim, f.body);
    }
    kit.vgap(8.0f);
    const auto describe = [&](const char* id, std::string_view title, std::uint64_t size, std::uint64_t modified, bool newer) {
        const item_result r = ui.custom_item(id, {ui.content_width(), 44.0f});
        shape_style bx;
        bx.radius       = radii(8.0f);
        bx.fill_top     = bx.fill_bottom = pal.raised;
        bx.border       = newer ? pal.accent : pal.border;
        bx.border_width = 1.0f;
        p.dl().shape(r.bounds, bx);
        p.text_left(rect{{r.bounds.min.x, r.bounds.min.y}, {r.bounds.min.x + 160.0f, r.bounds.max.y}}, title, pal.text, f.bold, 14.0f);
        p.text_left(rect{{r.bounds.min.x + 160.0f, r.bounds.min.y}, {r.bounds.max.x, r.bounds.max.y}},
                    std::format("{}   {}{}", core::path::format_size(size), core::path::format_time(modified), newer ? tr("   (newer)") : std::string_view{}), pal.text_dim, f.body);
    };
    describe("##tc_new", tr("The file being copied"), info->source_size, info->source_modified, info->source_modified > info->target_modified);
    kit.vgap(6.0f);
    describe("##tc_old", tr("The file already there"), info->target_size, info->target_modified, info->target_modified > info->source_modified);
    kit.vgap(10.0f);

    bool all = for_all_;
    {
        const item_result r = ui.custom_item("##tc_all_l", {ui.content_width() - 60.0f, 34.0f});
        p.text_left(r.bounds, tr("Do this for every other file that exists"), pal.text_dim, f.body, 4.0f);
        ui.same_line();
        if (kit.toggle_switch("##tc_all", all)) { for_all_ = all; }
    }
    kit.vgap(10.0f);

    core::ConflictChoice pick = core::ConflictChoice::cancel;
    bool chose = false;
    const float bw_cancel = kit.button_width(tr("Stop"), false);
    const float bw_skip   = kit.button_width(tr("Skip"), false);
    const float bw_both   = kit.button_width(tr("Keep both"), false);
    const float bw_rep    = kit.button_width(tr("Replace"), false, Kit::ButtonKind::primary);
    const item_result sp  = ui.custom_item("##tc_sp", {ui.content_width() - bw_cancel - bw_skip - bw_both - bw_rep - 4.0f * 8.0f - 8.0f, kit.control_h()});
    (void)sp;
    ui.same_line();
    if (kit.button({"##tc_stop", tr("Stop")})) { pick = core::ConflictChoice::cancel; chose = true; }
    ui.same_line();
    (void)ui.custom_item("##tc_g1", {8.0f, kit.control_h()});
    ui.same_line();
    if (kit.button({"##tc_skip", tr("Skip")})) { pick = core::ConflictChoice::skip; chose = true; }
    ui.same_line();
    (void)ui.custom_item("##tc_g2", {8.0f, kit.control_h()});
    ui.same_line();
    if (kit.button({"##tc_both", tr("Keep both")})) { pick = core::ConflictChoice::keep_both; chose = true; }
    ui.same_line();
    (void)ui.custom_item("##tc_g3", {8.0f, kit.control_h()});
    ui.same_line();
    if (kit.button({"##tc_rep", tr("Replace"), 0, Kit::ButtonKind::primary})) { pick = core::ConflictChoice::replace; chose = true; }

    if (chose) {
        answered_ = info->serial;
        q.resolve(info->serial, pick, for_all_ && pick != core::ConflictChoice::cancel);
        open_ = false;
        ui.close_modal();
    }
}

} // namespace prism::ui
