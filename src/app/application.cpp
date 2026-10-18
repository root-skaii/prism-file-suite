#include "app/application.hpp"

#include "core/default_manager.hpp"
#include "core/path.hpp"
#include "core/places.hpp"
#include "ui/actions.hpp"
#include "core/text.hpp"
#include "platform/dnd.hpp"
#include "strata/frame.hpp"

#include <windows.h>
#include <objbase.h>
#include <ole2.h>

#include <algorithm>
#include <cmath>

namespace prism::app {

bool Application::init(const Options& opts)
{
    const std::string& start_path = opts.path;
    ui::register_actions(binds_);
    settings_.binds = &binds_;
    settings_.load();
    if (opts.light) { settings_.dark = false; }
    if (opts.compact) { settings_.compact = true; }
    if (opts.grid) { settings_.grid_view = true; }
    if (opts.preview) { settings_.preview = true; }
    if (opts.split) { settings_.split = true; }
    theme_.set_density(settings_.compact ? ui::Density::compact : ui::Density::comfortable);
    theme_.set_accent(strata::color{static_cast<strata::u8>(settings_.accent >> 16), static_cast<strata::u8>(settings_.accent >> 8), static_cast<strata::u8>(settings_.accent), 255});
    theme_.set_dark_style(static_cast<ui::DarkStyle>(settings_.dark_style));
    theme_.set_mode(settings_.dark ? ui::ThemeMode::dark : ui::ThemeMode::light);
    scale_pct_ = settings_.ui_scale;
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    (void)::OleInitialize(nullptr); // (a single-threaded apartment, plus what drag and drop needs)
    core::refresh_default_manager(); // (the folder was moved: opening folders for other apps must still find this exe)

    if (!window_.create({.title = "PrismFS", .width = settings_.window_w > 0 ? settings_.window_w : 1360, .height = settings_.window_h > 0 ? settings_.window_h : 840})) { return false; }

    auto created = ui::create_context(theme_.style(), window_.dpi_scale() * static_cast<float>(scale_pct_) / 100.0f);
    if (!created) { return false; }
    ui_.emplace(std::move(*created));
    ui_->set_clipboard(window_.input().clipboard());

    if (!gpu_.create(window_.handle(), *ui_)) { return false; }

    ops_  = std::make_unique<core::FileOps>(window_.waker(), window_.handle());
    tabs_ = std::make_unique<model::TabSet>(window_.waker());
    if (!start_path.empty()) {
        const std::string first = core::path::normalize(start_path);
        tabs_->open(core::path::is_directory(first) ? first : "C:\\");
    } else if (settings_.restore_tabs && !settings_.tabs.empty()) {
        for (const std::string& t : settings_.tabs) {
            if (core::path::is_directory(t)) { tabs_->open(t); }
        }
        tabs_->activate(static_cast<std::size_t>(std::max(0, settings_.active_tab)));
    }
    if (tabs_->size() == 0) {
        const std::string home = core::home_folder();
        tabs_->open(core::path::is_directory(home) ? home : "C:\\");
    }

    images_ = std::make_unique<platform::ImageCache>(window_.waker());
    shell_ = std::make_unique<ui::Shell>(window_, theme_, ui::FontSet{}, *tabs_, *ops_, settings_, *images_, binds_, history_);
    window_.set_logical_scale(ui_->scale());
    window_.hit_test       = [this](strata::vec2 p) { return shell_->hit_test(p); };
    platform::DropCallbacks drop;
    drop.resolve = [this](strata::vec2 p) { return shell_->resolve_drop(p); };
    drop.hover   = [this](std::optional<strata::vec2> p) { shell_->set_drop_point(p); };
    drop.link    = [](const std::string& dest) { return ui::Sidebar::is_pin_target(dest); };
    drop.dropped = [this](std::vector<std::string> paths, std::string dest, bool move) { shell_->drop_files(std::move(paths), dest, move); };
    drop.redraw  = [this] { (void)step(true); };
    drop.scale   = [this] { return window_.logical_scale(); };
    (void)platform::attach_drop_target(window_.handle(), std::move(drop));
    window_.on_live_resize = [this] {
        // the window is being dragged: if a newer pointer position is already waiting, this size is about to be stale and
        // drawing it would only hold the next one back (on a slow gpu that is the "keeps resizing after I let go" lag).
        // never skip more than a few in a row, and the last size always draws
        static int skipped = 0;
        if (skipped < 3 && HIWORD(::GetQueueStatus(QS_MOUSEMOVE)) != 0) {
            ++skipped;
            return;
        }
        skipped = 0;
        (void)window_.take_resized();
        gpu_.resize();
        gpu_.set_immediate(true);
        (void)step(true);
        gpu_.set_immediate(false);
    };
    if (opts.settings) { shell_->show_settings(); }
    if (!opts.properties.empty()) { shell_->show_properties({opts.properties}, opts.properties_tab); }
    apply_theme();
    return true;
}

void Application::apply_theme()
{
    ui_->theme() = theme_.style();
    window_.set_dark_frame(theme_.dark());
    ui_->invalidate();
}

bool Application::step(bool force_draw)
{
    // names with characters the fonts do not have (han, kana, hangul, arabic, emoji ...): bake exactly those glyphs. the
    // fonts are rebuilt as a whole, which takes a few tens of ms, so it happens only when a new character first appears
    if (const unsigned rev = core::text::demand_revision(); rev != baked_revision_) {
        baked_revision_ = rev; // (also on failure: no retry loop)
        auto created = ui::create_context(theme_.style(), ui_->scale(), core::text::demanded_codepoints());
        if (created) {
            ui_.emplace(std::move(*created));
            ui_->set_clipboard(window_.input().clipboard());
            ui_->theme() = theme_.style();
            images_->forget();
            if (!gpu_.recreate(*ui_)) {
                window_.request_close();
                return false;
            }
        }
    }

    const auto apply_scale = [&](float dpi) {
        const float s = dpi * static_cast<float>(scale_pct_) / 100.0f;
        if (s != ui_->scale() && ui_->set_scale(s)) { gpu_.update_atlas(*ui_); }
    };
    if (const float s = window_.take_dpi_change(); s > 0.0f) { apply_scale(s); }

    if (images_->poll(gpu_.renderer())) { ui_->invalidate(); } // (a thumbnail arrived: draw it this frame)
    window_.set_logical_scale(ui_->scale());

    strata::input_state input = window_.input().new_frame();
    const ui::FrameInput frame_in{std::string_view{input.typed.data(), input.typed_len}, input.delta_time};
    ui_->begin_frame(input);
    shell_->frame(*ui_, frame_in);
    strata::end_frame(*ui_, window_.input());
    if (shell_->take_restyle()) { apply_theme(); }
    if (const int pct = shell_->take_scale_request(); pct > 0) {
        scale_pct_ = pct;
        apply_scale(window_.dpi_scale());
    }
    if (shell_->cursor() != strata::cursor_kind::arrow) { window_.input().set_cursor(shell_->cursor()); }

    bool presented = false;
    if (force_draw || !ui_->frame_unchanged()) {
        presented = true;
        if (!gpu_.present(*ui_, theme_.pal().window)) {
            images_->forget(); // (every texture died with the device)
            if (!gpu_.recreate(*ui_)) {
                window_.request_close();
                return false;
            }
        }
    }

    // an OS drag of files starts after the frame: it runs its own message loop until the button is released
    if (std::vector<std::string> drag = shell_->take_drag(); !drag.empty()) {
        shell_->set_dragging(drag);
        (void)platform::run_drag(drag);
        shell_->set_dragging({});
        shell_->set_drop_point(std::nullopt);
        shell_->refresh_all();
        ui_->invalidate();
    }
    return presented;
}

int Application::run()
{
    window_.show();
    while (window_.pump()) {
        if (window_.minimized()) {
            window_.wait(0xffffffffu);
            continue;
        }
        if (window_.take_resized()) {
            gpu_.resize();
            ui_->invalidate();
        }
        if (presented_) { gpu_.wait_for_frame(); }
        presented_ = step(false);

        // sleep until input arrives or the ui needs a frame (animation, caret, tooltip delay). a directory loader
        // posts a message when it has rows, which ends the sleep
        const double wait = ui_->next_wake_seconds();
        if (wait > 0.0) {
            window_.wait(wait >= 3600.0 ? 0xffffffffu : static_cast<std::uint32_t>(std::ceil(wait * 1000.0)));
        } else {
            gpu_.pace_to_display(); // animating: one frame per refresh, not a busy loop
        }
    }
    platform::detach_drop_target(window_.handle());
    const strata::vec2 logical = ui_->display_size();
    shell_->save_session(static_cast<int>(logical.x * static_cast<float>(scale_pct_) / 100.0f), static_cast<int>(logical.y * static_cast<float>(scale_pct_) / 100.0f));
    return 0;
}

} // namespace prism::app
