#pragma once

// what the user did to files, so Ctrl+Z / Ctrl+Y can walk back and forth. each entry carries its own inverse: the stack
// knows nothing about files

#include <functional>
#include <string>
#include <vector>

namespace prism::model {

struct UndoEntry {
    std::string           label;                // "Rename report.txt", "Move 3 items"
    std::function<bool()> undo;                 // false: could not (the item moved on, a conflict ...)
    std::function<bool()> redo;
};

class UndoStack {
public:
    void push(UndoEntry e)
    {
        undo_.push_back(std::move(e));
        if (undo_.size() > max_entries) { undo_.erase(undo_.begin()); }
        redo_.clear(); // a new action ends the redo branch
    }

    [[nodiscard]] bool can_undo() const noexcept { return !undo_.empty(); }
    [[nodiscard]] bool can_redo() const noexcept { return !redo_.empty(); }
    [[nodiscard]] const std::string& undo_label() const { return undo_.back().label; }
    [[nodiscard]] const std::string& redo_label() const { return redo_.back().label; }

    // runs the newest entry's inverse; it moves to the redo side whether or not it worked (a failed one is dropped)
    bool undo()
    {
        if (undo_.empty()) { return false; }
        UndoEntry e = std::move(undo_.back());
        undo_.pop_back();
        const bool ok = e.undo ? e.undo() : false;
        if (ok) { redo_.push_back(std::move(e)); }
        return ok;
    }
    bool redo()
    {
        if (redo_.empty()) { return false; }
        UndoEntry e = std::move(redo_.back());
        redo_.pop_back();
        const bool ok = e.redo ? e.redo() : false;
        if (ok) { undo_.push_back(std::move(e)); }
        return ok;
    }

private:
    static constexpr std::size_t max_entries = 100;
    std::vector<UndoEntry> undo_;
    std::vector<UndoEntry> redo_;
};

} // namespace prism::model
