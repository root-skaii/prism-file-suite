#pragma once

// the whole window content: title bar with tabs, toolbar, sidebar, file view, status bar. built every frame.
// also the app's command handler and keyboard shortcuts.

#include "ui/env.hpp"
#include "ui/file_list.hpp"
#include "ui/sidebar.hpp"
#include "ui/status_bar.hpp"
#include "ui/title_bar.hpp"
#include "ui/toolbar.hpp"

#include <optional>

namespace prism::ui {

class Shell final : private Commands {
public:
    Shell(platform::Window& window, Theme& theme, FontSet fonts, model::TabSet& tabs);

    void frame(strata::context& ui, const FrameInput& in);
    [[nodiscard]] platform::HitZone hit_test(strata::vec2 logical) const { return title_bar_.hit_test(logical); }
    // the theme / density changed: ask the host to re-apply strata's style
    [[nodiscard]] bool take_restyle() noexcept { return std::exchange(restyle_, false); }

private:
    // Commands
    void open_tab(const std::string& path, bool activate) override;
    void close_tab(std::size_t index) override;
    void toggle_theme() override;
    void toggle_density() override;
    void toggle_hidden() override;
    void focus_address() override;
    void focus_filter() override;

    void shortcuts(strata::context& ui);
    void apply_deferred();

    Env         env_;
    TitleBar    title_bar_;
    Toolbar     toolbar_;
    Sidebar     sidebar_;
    FileList    file_list_;
    StatusBar   status_bar_;

    bool        show_hidden_{false};
    bool        restyle_{false};
    // tab changes asked for mid-frame run at its end, when no panel still holds a reference into the tab list
    struct Open { std::string path; bool activate; };
    std::vector<Open>        pending_open_;
    std::vector<std::size_t> pending_close_;
};

} // namespace prism::ui
