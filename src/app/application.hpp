#pragma once

// owns the window, the gpu, the tabs and the ui context, and runs the frame loop

#include "core/file_ops.hpp"
#include "model/tab_set.hpp"
#include "model/undo_stack.hpp"
#include "strata/keybinds.hpp"
#include "ui/settings.hpp"
#include "platform/gpu.hpp"
#include "platform/image_cache.hpp"
#include "platform/window.hpp"
#include "ui/shell.hpp"
#include "ui/theme.hpp"

#include <memory>
#include <optional>
#include <string>

namespace prism::app {

class Application {
public:
    Application() = default;

    struct Options {
        std::string path;   // where the first tab opens (empty: the home folder)
        std::string second; // a folder for the right pane: opens two panes
        bool        light{};
        bool        compact{};
        bool        settings{};
        bool        palette{};
        std::string properties;  // open the Properties dialog for this path at start
        int         properties_tab{};
        bool        grid{};
        bool        preview{};
        bool        split{};
        bool        log{};      // write the troubleshooting log from the very start (also a setting)
        bool        has_at{};   // window top-left in screen pixels (a window started by another one); not remembered
        int         at_x{}, at_y{};
        int         width{}, height{}; // logical client size for this window only (0: the remembered one)
    };

    [[nodiscard]] bool init(const Options& opts);
    int run();

private:
    // one iteration: platform input -> ui -> (gpu). true if something was presented
    bool step(bool force_draw);
    void apply_theme();
    void start_log();   // opens the troubleshooting log and writes what a bug report needs (version, machine, paths)

    platform::Window               window_;
    platform::Gpu                  gpu_;
    ui::Theme                      theme_;
    bool                           strings_dirty_{true}; // strata's own words need setting again (language changed, or a new context)
    unsigned                       lang_revision_{0}; // core::i18n::revision() the action descriptions were registered for
    strata::keybinds               binds_;      // declared before settings_: it loads the chords
    model::UndoStack               history_;
    ui::Settings                   settings_;
    std::unique_ptr<core::FileOps> ops_;
    std::unique_ptr<platform::ImageCache> images_;
    std::unique_ptr<model::TabSet> tabs_;
    int                            scale_pct_{100};
    unsigned                       baked_revision_{};   // core::text::demand_revision() the font atlas was built for
    std::optional<strata::context> ui_;
    std::unique_ptr<ui::Shell>     shell_;
    bool                           presented_{};
};

} // namespace prism::app
