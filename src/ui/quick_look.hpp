#pragma once

// Space: a large floating look at the selected file. Arrow keys still move the selection in the list behind it and the
// look follows; Space or Esc closes it.

#include "core/file_info.hpp"
#include "ui/env.hpp"
#include "ui/painter.hpp"

#include <string>
#include <vector>

namespace prism::ui {

class QuickLook {
public:
    explicit QuickLook(Env& env) : env_{env} {}

    void toggle() noexcept { open_ = !open_; }
    void close() noexcept { open_ = false; }
    [[nodiscard]] bool open() const noexcept { return open_; }

    // draws over `area` (the file area); closes itself when there is not exactly one item selected
    void frame(Painter& p, const strata::rect& area);

private:
    void load(const std::string& path);

    Env&                     env_;
    bool                     open_{};
    std::string              shown_;
    core::FileInfo           info_;
    core::FileKind           kind_{core::FileKind::other};
    std::vector<std::string> lines_;
};

} // namespace prism::ui
