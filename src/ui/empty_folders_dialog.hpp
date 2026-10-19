#pragma once

// finds the empty folders below the current one (core::EmptyFolderScan) and moves the ones you leave marked to the Recycle
// Bin (through FileOps, so Undo brings them back)

#include "core/empty_folders.hpp"
#include "ui/env.hpp"
#include "ui/painter.hpp"
#include "ui/widgets.hpp"

#include <set>
#include <string>
#include <vector>

namespace prism::ui {

class EmptyFoldersDialog {
public:
    explicit EmptyFoldersDialog(Env& env) : env_{env} {}
    ~EmptyFoldersDialog() { scan_.cancel(); }

    void open(std::string folder);
    void frame(Painter& p);

private:
    Env&                      env_;
    core::EmptyFolderScan     scan_;
    std::string               folder_;
    std::vector<std::string>  found_;
    std::set<std::string>     marked_;
    bool                      loaded_{};
    bool                      want_open_{};
    bool                      open_{};
};

} // namespace prism::ui
