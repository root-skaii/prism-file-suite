#pragma once

// Space: a large floating look at the selected file. Arrow keys still move the selection in the list behind it and the
// look follows; Space or Esc closes it.

#include "core/file_info.hpp"
#include "core/syntax.hpp"
#include "ui/env.hpp"
#include "ui/painter.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace prism::ui {

class QuickLook {
public:
    explicit QuickLook(Env& env) : env_{env} {}
    ~QuickLook() { cancel_load(); }

    void toggle() noexcept { open_ = !open_; scroll_ = 0; zoom_ = 1.0f; pan_ = {}; dragging_pan_ = false; }
    void close() noexcept { open_ = false; }
    [[nodiscard]] bool open() const noexcept { return open_; }

    // draws over `area` (the file area); closes itself when there is not exactly one item selected
    void frame(Painter& p, const strata::rect& area);

private:
    struct LoadResult {
        std::string                        path;
        core::FileInfo                     info;
        core::FileKind                     kind{core::FileKind::other};
        std::vector<core::HighlightedLine> lines;
        std::vector<std::string>           hex_lines;
        std::atomic<bool>                  done{false};
        std::atomic<bool>                  cancel{false};
    };

    void load(const std::string& path);
    void cancel_load() noexcept;
    void poll_load();

    Env&                               env_;
    bool                               open_{};
    std::string                        shown_;
    core::FileInfo                     info_;
    core::FileKind                     kind_{core::FileKind::other};
    std::vector<core::HighlightedLine> lines_;
    std::vector<std::string>           hex_lines_;
    std::shared_ptr<LoadResult>        pending_;
    int                                scroll_{}; // first visible line in text / hex view
    float                              zoom_{1.0f};
    strata::vec2                       pan_{};
    bool                               dragging_pan_{};
    strata::vec2                       pan_anchor_{};
    strata::vec2                       pan_start_{};
};

} // namespace prism::ui
