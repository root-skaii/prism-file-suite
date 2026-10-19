#pragma once

// PrismFS's own Properties: general facts and editable attributes, NTFS / version details, and file hashes.
// (the system's dialog stays available next to it). built from the widget kit: own header, pill tabs, cards.

#include "core/file_info.hpp"
#include "ui/env.hpp"
#include "ui/painter.hpp"
#include "ui/widgets.hpp"

#include <memory>
#include <string>
#include <vector>

namespace prism::ui {

class PropertiesDialog {
public:
    explicit PropertiesDialog(Env& env) : env_{env} {}
    ~PropertiesDialog() { release(); }

    void open(std::vector<std::string> paths, int tab = 0);
    void frame(Painter& p);

private:
    void release();
    void reload();
    void header(Kit& kit);
    void general(Kit& kit);
    void details(Kit& kit);
    void hashes(Kit& kit);
    void note(Kit& kit, std::string_view id, std::string_view text);

    Env&                    env_;
    bool                    want_open_{};
    bool                    shown_{};
    int                     tab_{0};
    int                     fade_gen_{};    // tab switches fade the body in: a fresh animation key per switch
    int                     fade_frames_{};
    float                   content_h_{420.0f}; // the body's content height, measured last frame
    std::vector<std::string> paths_;
    std::vector<core::FileInfo> infos_;
    std::vector<std::unique_ptr<core::FolderScan>> scans_;
    core::HashJob           hash_;
    bool                    hash_started_{};
    std::string             compare_;
    // an attribute switch flipped this frame: applied once the frame has drawn (it reloads the data the cards read)
    std::uint32_t           attr_flag_{};
    bool                    attr_on_{};
};

} // namespace prism::ui
