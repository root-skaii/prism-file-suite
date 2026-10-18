#pragma once

// the entries of one folder as the file view shows them: filtered, sorted, with a selection that survives re-sorting.
// rows are positions in the visible order; entries are positions in the raw list (stable)

#include "core/file_entry.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace prism::model {

enum class SortKey : std::uint8_t { name, modified, type, size };

struct SortSpec {
    SortKey key{SortKey::name};
    bool    ascending{true};
    friend bool operator==(const SortSpec&, const SortSpec&) = default;
};

class Listing {
public:
    static constexpr std::uint32_t npos = 0xffffffffu;

    void clear();
    // while the folder is still being read: entries join the end of the view unsorted (the file system's own order
    // is nearly sorted already); finish() sorts once at the end
    void append(std::vector<core::FileEntry>&& more);
    void finish();
    [[nodiscard]] bool finished() const noexcept { return finished_; }

    void set_sort(SortSpec s);
    void set_show_hidden(bool on);
    void set_filter(std::string_view text);
    [[nodiscard]] const SortSpec& sort() const noexcept { return sort_; }
    [[nodiscard]] bool show_hidden() const noexcept { return show_hidden_; }
    [[nodiscard]] const std::string& filter() const noexcept { return filter_; }

    // view ----------------------------------------------------------------
    [[nodiscard]] std::size_t rows() const noexcept { return order_.size(); }
    [[nodiscard]] const core::FileEntry& at(std::size_t row) const noexcept { return entries_[order_[row]]; }
    [[nodiscard]] std::size_t total() const noexcept { return entries_.size(); } // before hiding / filtering
    [[nodiscard]] std::size_t folders() const noexcept { return folder_count_; }  // visible folders
    [[nodiscard]] std::optional<std::size_t> find_name(std::string_view name) const; // exact, case-insensitive
    // first row at or after `from` (wrapping) whose name starts with `prefix`, case-insensitive
    [[nodiscard]] std::optional<std::size_t> find_prefix(std::string_view prefix, std::size_t from) const;

    // selection -----------------------------------------------------------
    [[nodiscard]] bool selected(std::size_t row) const noexcept { return sel_[order_[row]] != 0; }
    [[nodiscard]] std::size_t selected_count() const noexcept { return sel_count_; }
    [[nodiscard]] std::uint64_t selected_bytes() const noexcept { return sel_bytes_; }
    [[nodiscard]] std::vector<std::size_t> selected_rows() const;

    void select_only(std::size_t row);
    void toggle(std::size_t row);
    void select_range(std::size_t a, std::size_t b, bool keep_existing); // inclusive, either order
    void select_all();
    void clear_selection();
    void set_selected(std::size_t row, bool on);

    // the row keyboard navigation moves from / the shift-click anchor; npos = none
    [[nodiscard]] std::size_t focus() const noexcept;
    [[nodiscard]] std::size_t anchor() const noexcept;
    void set_focus(std::size_t row, bool move_anchor);

private:
    [[nodiscard]] bool visible(const core::FileEntry& e) const noexcept;
    [[nodiscard]] bool less(std::uint32_t a, std::uint32_t b) const noexcept;
    void rebuild();                // filter + sort from the raw list
    void index_rows() noexcept;    // row_of_ from order_
    void recount() noexcept;

    std::vector<core::FileEntry> entries_;
    std::vector<std::uint8_t>    sel_;      // per entry
    std::vector<std::uint32_t>   order_;    // visible entries, in display order
    std::vector<std::uint32_t>   row_of_;   // per entry: its row, npos when hidden
    SortSpec                     sort_{};
    std::string                  filter_;   // lower-case
    bool                         show_hidden_{false};
    bool                         finished_{true};
    std::size_t                  folder_count_{};
    std::size_t                  sel_count_{};
    std::uint64_t                sel_bytes_{};
    std::uint32_t                focus_{npos};  // entry
    std::uint32_t                anchor_{npos}; // entry
};

} // namespace prism::model
