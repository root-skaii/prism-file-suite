#include "ui/shell.hpp"

#include "core/path.hpp"
#include "core/places.hpp"

#include <algorithm>

namespace prism::ui {

using namespace strata;

Shell::Shell(platform::Window& window, Theme& theme, FontSet fonts, model::TabSet& tabs)
    : env_{window, theme, fonts, tabs, *this},
      title_bar_{env_}, toolbar_{env_}, sidebar_{env_}, file_list_{env_}, status_bar_{env_}
{
}

void Shell::open_tab(const std::string& path, bool activate)
{
    pending_open_.push_back({path, activate});
}

void Shell::close_tab(std::size_t index)
{
    pending_close_.push_back(index);
}

void Shell::toggle_theme()
{
    env_.theme.toggle();
    restyle_ = true;
}

void Shell::toggle_density()
{
    env_.theme.set_density(env_.theme.density() == Density::compact ? Density::comfortable : Density::compact);
    restyle_ = true;
}

void Shell::toggle_hidden() { show_hidden_ = !show_hidden_; }
void Shell::focus_address() { toolbar_.begin_edit(); }
void Shell::focus_filter() { toolbar_.focus_filter(); }

void Shell::apply_deferred()
{
    for (const Open& o : pending_open_) { env_.tabs.open(o.path, o.activate); }
    pending_open_.clear();
    for (const std::size_t i : pending_close_) {
        if (i < env_.tabs.size() && !env_.tabs.close(i)) { env_.window.request_close(); }
    }
    pending_close_.clear();
}

void Shell::shortcuts(context& ui)
{
    model::TabSet& tabs = env_.tabs;
    const bool typing = ui.want_text_input();

    if (ui.key_pressed(key::t, true)) { open_tab(tabs.active().path(), true); }
    if (ui.key_pressed(key::w, true)) { close_tab(tabs.active_index()); }
    if (ui.key_pressed(key::tab, true)) { tabs.next(1); }
    if (ui.key_pressed(key::tab, true, true)) { tabs.next(-1); }
    for (int i = 0; i < 9; ++i) {
        if (ui.key_pressed(static_cast<key>(static_cast<int>(key::d1) + i), true) && static_cast<std::size_t>(i) < tabs.size()) { tabs.activate(static_cast<std::size_t>(i)); }
    }
    if (ui.key_pressed(key::l, true) || ui.key_pressed(key::f4) || ui.key_pressed(key::d, false, false, true)) { focus_address(); }
    if (ui.key_pressed(key::f, true) || ui.key_pressed(key::e, true)) { focus_filter(); }
    if (ui.key_pressed(key::h, true)) { toggle_hidden(); }
    if (ui.key_pressed(key::f5) || ui.key_pressed(key::r, true)) { tabs.active().refresh(); }

    model::Tab& tab = tabs.active();
    if (ui.key_pressed(key::left, false, false, true)) { (void)tab.back(); }
    if (ui.key_pressed(key::right, false, false, true)) { (void)tab.forward(); }
    if (ui.key_pressed(key::up, false, false, true)) { (void)tab.up(); }
    if (!typing && ui.key_pressed(key::backspace)) { (void)tab.back(); }
    // mouse side buttons
    if (ui.key_pressed(key::mouse_x1)) { (void)tab.back(); }
    if (ui.key_pressed(key::mouse_x2)) { (void)tab.forward(); }
}

void Shell::frame(context& ui, const FrameInput& in)
{
    const Metrics& m   = env_.theme.metrics();
    const Palette& pal = env_.theme.pal();
    Painter p{ui, env_.theme, env_.fonts};

    env_.tabs.poll();
    for (std::size_t i = 0; i < env_.tabs.size(); ++i) { env_.tabs.at(i).listing().set_show_hidden(show_hidden_); }
    shortcuts(ui);

    const vec2  size = ui.display_size();
    const float w = size.x;
    const float h = size.y;

    {   // the window background: what shows between panels
        auto bg = ui.layer(layer::background);
        ui.draw().rect_filled({{0, 0}, size}, pal.window);
    }

    const rect title{{0, 0}, {w, m.title_bar}};
    const rect toolbar{{0, m.title_bar}, {w, m.title_bar + m.toolbar}};
    const float body_top    = m.title_bar + m.toolbar;
    const float body_bottom = h - m.status_bar;
    const rect sidebar{{0, body_top}, {m.sidebar_width, body_bottom}};
    const rect card{{m.sidebar_width, body_top}, {w - 8.0f, body_bottom - 2.0f}};
    const rect status{{0, body_bottom}, {w, h}};

    title_bar_.frame(p, title);
    toolbar_.frame(p, toolbar, show_hidden_);
    sidebar_.frame(p, sidebar, in);
    file_list_.frame(p, card, in);
    status_bar_.frame(p, status);

    apply_deferred();
}

} // namespace prism::ui
