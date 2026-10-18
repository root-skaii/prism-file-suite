#pragma once

// owns the window, the gpu, the tabs and the ui context, and runs the frame loop

#include "model/tab_set.hpp"
#include "platform/gpu.hpp"
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
        bool        light{};
        bool        compact{};
    };

    [[nodiscard]] bool init(const Options& opts);
    int run();

private:
    // one iteration: platform input -> ui -> (gpu). true if something was presented
    bool step(bool force_draw);
    void apply_theme();

    platform::Window               window_;
    platform::Gpu                  gpu_;
    ui::Theme                      theme_;
    std::unique_ptr<model::TabSet> tabs_;
    std::optional<strata::context> ui_;
    std::unique_ptr<ui::Shell>     shell_;
    bool                           presented_{};
};

} // namespace prism::app
