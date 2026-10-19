#pragma once

// rename the selection by one rule: a name pattern ([N] name, [E] extension, [C] counter), find / replace and a case
// change, with a live before / after list. nothing touches the disk until Rename; the rename is one undo step

#include "core/batch_rename.hpp"
#include "ui/env.hpp"
#include "ui/painter.hpp"
#include "ui/widgets.hpp"

#include <string>
#include <vector>

namespace prism::ui {

class BatchRenameDialog {
public:
    explicit BatchRenameDialog(Env& env) : env_{env} {}

    // `dir` holds the items; `others` are the other names in it (a new name must not collide with them)
    void open(std::string dir, std::vector<core::RenameItem> items, std::vector<std::string> others);
    void frame(Painter& p);

private:
    void field(Painter& p, std::string_view id, std::string_view label, std::string& text, std::string_view hint, float width);
    [[nodiscard]] core::RenameRule rule() const;
    void apply();

    Env&                         env_;
    bool                         want_open_{};
    bool                         shown_{};
    int                          age_{};
    std::string                  dir_;
    std::vector<core::RenameItem> items_;
    std::vector<std::string>     others_;
    std::string                  pattern_{"[N]"}, find_, replace_, start_{"1"};
    int                          casing_{0};
    int                          digits_{0};   // index into 1..4
};

} // namespace prism::ui
