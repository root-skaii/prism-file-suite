#pragma once

// PrismFS's own Properties: general facts and editable attributes, NTFS / version details, and file hashes.
// (the system's dialog stays available next to it). built from the widget kit: own header, pill tabs, cards.

#include "core/file_info.hpp"
#include "core/recycle.hpp"
#include "ui/env.hpp"
#include "ui/painter.hpp"
#include "ui/widgets.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace prism::ui {

class PropertiesDialog {
public:
    explicit PropertiesDialog(Env& env) : env_{env} {}
    ~PropertiesDialog() { release(); }

    void open(std::vector<std::string> paths, int tab = 0);
    void frame(Painter& p);
    [[nodiscard]] bool active() const noexcept { return want_open_ || shown_; }
    // shown in a window of its own (filling it) instead of as a modal over the main window
    void set_embedded(bool on) noexcept { embedded_ = on; }
    // where the window may be dragged from (logical pixels): the header, not its buttons
    [[nodiscard]] const strata::rect& drag_zone() const noexcept { return drag_zone_; }

private:
    void release();
    void content(Painter& p);
    void close(strata::context& ui);
    void reload(bool sync = false);
    void poll_info();
    void header(Kit& kit);
    void general(Kit& kit);
    void details(Kit& kit);
    void hashes(Kit& kit);
    void note(Kit& kit, std::string_view id, std::string_view text);

    Env&                    env_;
    bool                    want_open_{};
    bool                    shown_{};
    bool                    embedded_{};
    strata::rect            drag_zone_{};
    int                     tab_{0};
    int                     fade_gen_{};    // tab switches fade the body in: a fresh animation key per switch
    int                     fade_frames_{};
    float                   content_h_{440.0f}; // the body's content height, measured last frame
    std::vector<std::string> paths_;
    std::vector<core::FileInfo> infos_;
    core::FileInfoJob       info_job_;
    bool                    loading_{};
    std::optional<core::recycle::Record> bin_; // a single Recycle Bin item: where and when it was deleted
    std::vector<std::unique_ptr<core::FolderScan>> scans_;
    core::HashJob           hash_;
    bool                    hash_started_{};
    std::string             compare_;
    // inline rename in the header (single regular file / folder)
    bool                    renaming_{};
    int                     rename_age_{};
    std::string             rename_text_;
    // deferred mutations applied at end of frame so nothing is half-drawn
    std::uint32_t           attr_flag_{};
    bool                    attr_on_{};
    bool                    want_unblock_{};
};

} // namespace prism::ui
