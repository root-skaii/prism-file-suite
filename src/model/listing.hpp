#pragma once

// the entries of one folder as the file view shows them: filtered, sorted, with a selection that survives re-sorting.
// rows are positions in the visible order; entries are positions in the raw list (stable)

#include "core/file_entry.hpp"
#include "core/query.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace prism::model {

enum class SortKey : std::uint8_t { name, modified, type, size };
// where folders go: above the files (the explorer way), among them, or below
enum class FolderPlacement : std::uint8_t { first, mixed, last };

struct SortSpec {
    SortKey key{SortKey::name};
    bool    ascending{true};
    FolderPlacement folders{FolderPlacement::first};
    bool    natural{true};       // "file2" before "file10"
    bool    hidden_last{false};  // hidden and dot files after the others (inside their group)
    friend bool operator==(const SortSpec&, const SortSpec&) = default;
};

// the details view can cut the list into titled groups; they come before the sort (which orders inside each group)
enum class GroupBy : std::uint8_t { none, name, type, modified, size };

class Listing {
public:
    static constexpr std::uint32_t npos = 0xffffffffu;

    void clear();
    // while the folder is still being read: entries join the end of the view unsorted (the file system's own order
    // is nearly sorted already); finish() sorts once at the end
    void append(std::vector<core::FileEntry>&& more);
    void finish();
    // swaps in a fresh read of the same folder without a reload flash: selection, focus and anchor follow the names
    void replace_entries(std::vector<core::FileEntry>&& fresh);
    // takes entries out at once (a delete just started); a later read of the folder corrects it if the delete failed
    void remove_names(const std::unordered_set<std::string>& names);
    // patches a single added / modified entry in place (avoids re-enumerating and re-sorting 200k-file folders)
    bool upsert_entry(core::FileEntry&& entry);
    // removes one entry by name (case-insensitive) in O(N) without full rebuild
    bool remove_name(std::string_view name);
    [[nodiscard]] bool finished() const noexcept { return finished_; }

    void set_sort(SortSpec s);
    void set_show_hidden(bool on);
    // colour labels: copies each entry's labels from `lookup` into FileEntry::tags, when `token` (what the labels depend on:
    // the store's revision, the folder, how many entries are loaded) differs from the last call. a `tag:` filter is re-applied
    void sync_tags(std::uint64_t token, const std::function<std::uint16_t(const core::FileEntry&)>& lookup);
    [[nodiscard]] std::uint64_t epoch() const noexcept { return epoch_; } // changes with every clear(): a folder (re)load
    void set_group(GroupBy g);
    [[nodiscard]] GroupBy group() const noexcept { return group_; }
    // rows that begin a group, ascending (empty when not grouped or still loading); group_title(row) names the group of a row
    [[nodiscard]] const std::vector<std::uint32_t>& group_starts() const noexcept { return group_starts_; }
    [[nodiscard]] std::string group_title(std::size_t row) const;
    // the live filter: only entries the query matches are shown (an empty or invalid query shows everything)
    void set_query(core::Query q);
    [[nodiscard]] const SortSpec& sort() const noexcept { return sort_; }
    [[nodiscard]] bool show_hidden() const noexcept { return show_hidden_; }
    [[nodiscard]] const core::Query& query() const noexcept { return query_; }

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
    void invert_selection();
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
    std::vector<std::uint8_t>    rank_;     // per entry: its group's place (only while grouped)
    std::vector<std::uint32_t>   group_starts_;
    std::uint64_t                epoch_{};
    std::uint64_t                tag_token_{~std::uint64_t{0}};
    GroupBy                      group_{GroupBy::none};
    bool                         group_flip_{}; // groups run in the opposite order (sorted descending by the key they are made of)
    SortSpec                     sort_{};
    core::Query                  query_;
    bool                         show_hidden_{false};
    bool                         finished_{true};    std::size_t                  folder_count_{};
    std::size_t                  sel_count_{};
    std::uint64_t                sel_bytes_{};
    std::uint32_t                focus_{npos};  // entry
    std::uint32_t                anchor_{npos}; // entry
};

} // namespace prism::model
