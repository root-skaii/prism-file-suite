#pragma once

// the right-hand pane: a large thumbnail (or icon) of the selected item, its facts, and the start of a text file

#include "core/file_info.hpp"
#include "ui/env.hpp"
#include "ui/painter.hpp"

#include <memory>
#include <string>
#include <vector>

namespace prism::ui {

class PreviewPane {
public:
    explicit PreviewPane(Env& env) : env_{env} {}
    void frame(Painter& p, const strata::rect& area);

private:
    void load(const std::string& path);

    Env&                        env_;
    std::string                 shown_;      // what the cached info below describes
    core::FileInfo              info_;
    core::FileKind              kind_{core::FileKind::other};
    std::vector<std::string>    lines_;      // the head of a text file
    std::unique_ptr<core::FolderScan> scan_;
};

} // namespace prism::ui
