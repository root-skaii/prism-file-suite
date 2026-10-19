#pragma once

// compare the two panes' folders and sync them: a list of what differs (only on one side, newer on one side ...) with a
// direction per row, applied as ordinary copy jobs (so undo and the shell's conflict prompts work). nothing is ever deleted

#include "core/compare.hpp"
#include "ui/env.hpp"
#include "ui/painter.hpp"
#include "ui/widgets.hpp"

#include <memory>
#include <string>
#include <vector>

namespace prism::ui {

class CompareDialog {
public:
    explicit CompareDialog(Env& env) : env_{env} {}
    ~CompareDialog() { scan_.cancel(); }

    void open(std::string left, std::string right);
    void frame(Painter& p);

private:
    enum class Act : unsigned char { skip, to_right, to_left };
    void rescan();
    void apply();
    [[nodiscard]] static Act default_act(const core::DiffItem& d);

    Env&                                                env_;
    core::FolderCompare                                 scan_;
    std::string                                         left_, right_;
    std::shared_ptr<const std::vector<core::DiffItem>>  items_;
    std::vector<Act>                                    acts_;
    std::vector<std::size_t>                            shown_;   // indices into items_ (folders that are only containers and identical files are left out)
    bool                                                recursive_{true};
    bool                                                show_same_{};
    bool                                                want_open_{};
    bool                                                open_{};
};

} // namespace prism::ui
