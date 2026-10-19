#pragma once

// finds files with identical content below the current folder and lets you move the extra copies to the Recycle Bin.
// the scan runs on a worker (core::DuplicateScan); in every group the oldest file is kept by default and the others are
// marked, a click on a row flips its mark. a group can never lose all of its files

#include "core/duplicates.hpp"
#include "ui/env.hpp"
#include "ui/painter.hpp"
#include "ui/widgets.hpp"

#include <set>
#include <string>
#include <vector>

namespace prism::ui {

class DuplicatesDialog {
public:
    explicit DuplicatesDialog(Env& env) : env_{env} {}
    ~DuplicatesDialog() { scan_.cancel(); }

    void open(std::string folder);
    void frame(Painter& p);

private:
    void load_groups();

    Env&                        env_;
    core::DuplicateScan         scan_;
    bool                        want_open_{};
    bool                        shown_{};
    bool                        loaded_{};
    std::string                 folder_;
    std::vector<core::DuplicateGroup> groups_;
    std::set<std::string>       marked_;
};

} // namespace prism::ui
