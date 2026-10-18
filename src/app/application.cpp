#include "app/application.hpp"

#include "core/path.hpp"
#include "core/places.hpp"
#include "strata/frame.hpp"

#include <windows.h>
#include <objbase.h>

#include <cmath>

namespace prism::app {

bool Application::init(const Options& opts)
{
    const std::string& start_path = opts.path;
    if (opts.light) { theme_.set_mode(ui::ThemeMode::light); }
    if (opts.compact) { theme_.set_density(ui::Density::compact); }
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    (void)::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    if (!window_.create({.title = "PrismFS"})) { return false; }

    auto created = ui::create_context(theme_.style(), window_.dpi_scale());
    if (!created) { return false; }
    ui_.emplace(std::move(*created));
    ui_->set_clipboard(window_.input().clipboard());

    if (!gpu_.create(window_.handle(), *ui_)) { return false; }

    tabs_ = std::make_unique<model::TabSet>(window_.waker());
    std::string first = start_path.empty() ? core::home_folder() : core::path::normalize(start_path);
    if (first.empty() || !core::path::is_directory(first)) { first = "C:\\"; }
    tabs_->open(first);

    shell_ = std::make_unique<ui::Shell>(window_, theme_, ui::FontSet{}, *tabs_);
    window_.hit_test       = [this](strata::vec2 p) { return shell_->hit_test(p); };
    window_.on_live_resize = [this] {
        (void)window_.take_resized();
        gpu_.resize();
        (void)step(true);
    };
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
    if (const float s = window_.take_dpi_change(); s > 0.0f && s != ui_->scale() && ui_->set_scale(s)) {
        gpu_.update_atlas(*ui_);
    }

    strata::input_state input = window_.input().new_frame();
    const ui::FrameInput frame_in{std::string_view{input.typed.data(), input.typed_len}, input.delta_time};
    ui_->begin_frame(input);
    shell_->frame(*ui_, frame_in);
    strata::end_frame(*ui_, window_.input());
    if (shell_->take_restyle()) { apply_theme(); }

    if (!force_draw && ui_->frame_unchanged()) { return false; }
    if (!gpu_.present(*ui_, theme_.pal().window) && !gpu_.recreate(*ui_)) {
        window_.request_close();
        return false;
    }
    return true;
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
        }
    }
    return 0;
}

} // namespace prism::app
