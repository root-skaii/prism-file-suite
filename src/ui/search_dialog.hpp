#pragma once

// search everywhere (Ctrl+Shift+F): type, and the names of every indexed drive that match appear at once (core::FileIndex).
// the same query language as the filter box. Enter / double click goes to the item

#include "core/file_index.hpp"
#include "ui/env.hpp"
#include "ui/painter.hpp"
#include "ui/widgets.hpp"

#include <memory>
#include <string>

namespace prism::ui {

class SearchDialog {
public:
    SearchDialog(Env& env, core::FileIndex& index, std::function<void()> rebuild) : env_{env}, index_{index}, rebuild_{std::move(rebuild)} {}

    void open();
    void frame(Painter& p);

private:
    void activate(const core::IndexHit& hit);

    Env&                env_;
    core::FileIndex&    index_;
    std::function<void()> rebuild_;     // (re)starts the crawl of the configured drives
    bool                want_open_{};
    bool                open_{};
    int                 age_{};
    std::string         text_, asked_;
    std::shared_ptr<const core::SearchResult> shown_;
    int                 sel_{};
    bool                follow_{};      // scroll the selection into view
};

} // namespace prism::ui
