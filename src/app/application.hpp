#pragma once

// one window: the OS window, its gpu, tabs and ui context, and the frame it draws. the process (app::Host) owns the
// message loop, the shared settings and all the windows

#include "core/file_ops.hpp"
#include "model/tab_set.hpp"
#include "model/undo_stack.hpp"
#include "strata/keybinds.hpp"
#include "ui/settings.hpp"
#include "platform/gpu.hpp"
#include "platform/image_cache.hpp"
#include "platform/popup_surface.hpp"
#include "platform/window.hpp"
#include "ui/shell.hpp"
#include "ui/theme.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace prism::app {

class Application {
public:
    struct Options {
        std::string path;   // where the first tab opens (empty: the home folder)
        std::string second; // a folder for the right pane: opens two panes
        bool        light{};
        bool        compact{};
        bool        settings{};
        bool        palette{};
        std::string properties;  // open the Properties dialog for this path at start
        int         properties_tab{};
        std::string usage;       // open Disk usage for this folder at start
        bool        grid{};
        bool        preview{};
        bool        split{};
        bool        log{};      // write the troubleshooting log from the very start (also a setting)
        bool        has_at{};   // window top-left in screen pixels (a window started by another one); not remembered
        int         at_x{}, at_y{};
        int         width{}, height{}; // logical client size for this window only (0: the remembered one)
        bool        crash_test{};        // --crash-test: the process faults on purpose, to check the report
        bool        crash_test_window{}; // --crash-test-window: this window faults on purpose (the others must survive)
        std::string crashed;             // --crashed <report>: not a window, the notice that a PrismFS process crashed (core/crash)
        bool        window_notice{};     // --window (with --crashed): only one window was lost
    };

    // `open_window`: how this window asks for another one (the host makes it)
    Application(ui::Settings& settings, strata::keybinds& binds, std::function<void(const Options&)> open_window)
        : binds_{binds}, settings_{settings}, open_window_{std::move(open_window)} {}

    // opens the troubleshooting log and writes what a bug report needs (version, machine, paths)
    static void start_log();

    // starts the gpu driver load on its own thread, ahead of init() (it overlaps the fonts)
    void prewarm();
    [[nodiscard]] bool init(const Options& opts);

    // runs a frame if this window has something to do: input, a worker's wake-up, a timer or an animation that is due
    void tick();
    // 0: it wants a frame right now (animating); else seconds until a timer needs one (a large number: only input will)
    [[nodiscard]] double seconds_until_due() const;
    [[nodiscard]] bool   animating() const noexcept { return animating_; }
    void                 pace() const noexcept { gpu_.pace_to_display(); } // blocks until the next screen refresh

    [[nodiscard]] bool closed() const noexcept { return window_.closed(); }
    [[nodiscard]] platform::Window& window() noexcept { return window_; }
    void request_close() noexcept { window_.request_close(); }
    // the window is closing: remember its size and tabs
    void shutdown();
    // after a fault: the OS window goes, the object is left alone (its state is not to be trusted)
    void abandon() noexcept { window_.abandon(); }

private:
    // one iteration: platform input -> ui -> (gpu). true if something was presented
    bool step(bool force_draw);
    void apply_theme();

    platform::Window               window_;
    platform::Gpu                  gpu_;
    platform::PopupSurface         popup_;                 // menus drawn past the window edge (Settings::detached_menus)
    bool                           popup_failed_{};        // it could not be made or drawn: menus stay inside the window
    strata::rect                   popup_bounds_{};        // the monitor the menu opened on, in ui pixels (kept while it is open)
    void prepare_popups();                                 // before the frame: where menus may open
    bool present_frame();                                  // the frame, and the popup layers wherever they belong (false: device lost)
    ui::Theme                      theme_;
    bool                           strings_dirty_{true}; // strata's own words need setting again (language changed, or a new context)
    unsigned                       lang_revision_{0}; // core::i18n::revision() the action descriptions were registered for
    strata::keybinds&              binds_;      // shared by all windows
    model::UndoStack               history_;
    ui::Settings&                  settings_;   // shared by all windows: a change in one shows in the others
    std::function<void(const Options&)> open_window_;
    std::unique_ptr<core::FileOps> ops_;
    std::unique_ptr<platform::ImageCache> images_;
    std::unique_ptr<model::TabSet> tabs_;
    int                            scale_pct_{100};
    unsigned                       baked_revision_{};   // core::text::demand_revision() the font atlas was built for
    std::optional<strata::context> ui_;
    std::unique_ptr<ui::Shell>     shell_;
    bool                           presented_{};
    bool                           animating_{true};
    std::chrono::steady_clock::time_point due_{};       // when a timer needs the next frame
    std::jthread                   device_thread_;
    bool                           device_ok_{};
    std::atomic<bool>              device_ready_{false};
    int                            frames_{};           // (for --crash-test-window)
    bool                           crash_test_window_{};
};

} // namespace prism::app
