#include "model/listing.hpp"

#include "core/text.hpp"

#include <algorithm>

namespace prism::model {

using core::FileEntry;

namespace {

// search results are relative paths: they sort by the file's own name first, the folder breaks ties
[[nodiscard]] int compare_names(std::string_view a, std::string_view b) noexcept
{
    const auto leaf = [](std::string_view s) { const std::size_t i = s.rfind('\\'); return i == std::string_view::npos ? s : s.substr(i + 1); };
    const int c = core::text::compare_natural(leaf(a), leaf(b));
    return c != 0 ? c : core::text::compare_natural(a, b);
}

} // namespace

void Listing::clear()
{
    entries_.clear();
    sel_.clear();
    order_.clear();
    row_of_.clear();
    folder_count_ = 0;
    sel_count_    = 0;
    sel_bytes_    = 0;
    focus_        = npos;
    anchor_       = npos;
    finished_     = false;
}

bool Listing::visible(const FileEntry& e) const noexcept
{
    if (!show_hidden_ && e.is_hidden() && !query_.includes_hidden()) { return false; }
    return query_.matches(e);
}

void Listing::append(std::vector<FileEntry>&& more)
{
    const std::size_t first = entries_.size();
    if (entries_.empty()) {
        entries_ = std::move(more);
    } else {
        entries_.insert(entries_.end(), std::make_move_iterator(more.begin()), std::make_move_iterator(more.end()));
    }
    sel_.resize(entries_.size(), 0);
    row_of_.resize(entries_.size(), npos);
    for (std::size_t i = first; i < entries_.size(); ++i) {
        core::text::note_codepoints(entries_[i].name);
        if (!visible(entries_[i])) { continue; }
        row_of_[i] = static_cast<std::uint32_t>(order_.size());
        order_.push_back(static_cast<std::uint32_t>(i));
        if (entries_[i].is_dir()) { ++folder_count_; }
    }
}

void Listing::finish()
{
    finished_ = true;
    rebuild();
}

void Listing::set_sort(SortSpec s)
{
    if (s == sort_) { return; }
    sort_ = s;
    if (finished_) { rebuild(); }
}

void Listing::set_show_hidden(bool on)
{
    if (on == show_hidden_) { return; }
    show_hidden_ = on;
    rebuild();
}

void Listing::set_query(core::Query q)
{
    if (q.empty() && query_.empty() && q.error() == query_.error()) { return; }
    query_ = std::move(q);
    rebuild();
}

bool Listing::less(std::uint32_t ia, std::uint32_t ib) const noexcept
{
    const FileEntry& a = entries_[ia];
    const FileEntry& b = entries_[ib];
    if (a.is_dir() != b.is_dir()) { return a.is_dir(); } // folders first, whatever the direction

    int c = 0;
    switch (sort_.key) {
    case SortKey::name:     c = compare_names(a.name, b.name); break;
    case SortKey::modified: c = a.modified < b.modified ? -1 : (a.modified > b.modified ? 1 : 0); break;
    case SortKey::size:     c = a.size < b.size ? -1 : (a.size > b.size ? 1 : 0); break;
    case SortKey::type:
        if (a.kind != b.kind) {
            c = a.kind < b.kind ? -1 : 1;
        } else {
            c = core::text::compare_natural(a.ext(), b.ext());
        }
        break;
    }
    if (c == 0 && sort_.key != SortKey::name) { c = compare_names(a.name, b.name); return c < 0; } // ties by name, always ascending
    return sort_.ascending ? c < 0 : c > 0;
}

void Listing::rebuild()
{
    order_.clear();
    order_.reserve(entries_.size());
    folder_count_ = 0;
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (!visible(entries_[i])) { continue; }
        order_.push_back(static_cast<std::uint32_t>(i));
        if (entries_[i].is_dir()) { ++folder_count_; }
    }
    // (selection of entries that just became hidden is dropped: it could no longer be seen or acted on)
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (sel_[i] != 0 && !visible(entries_[i])) { sel_[i] = 0; }
    }
    if (finished_) {
        std::sort(order_.begin(), order_.end(), [this](std::uint32_t a, std::uint32_t b) { return less(a, b); });
    }
    index_rows();
    recount();
}

void Listing::index_rows() noexcept
{
    row_of_.assign(entries_.size(), npos);
    for (std::size_t r = 0; r < order_.size(); ++r) { row_of_[order_[r]] = static_cast<std::uint32_t>(r); }
    if (focus_ != npos && row_of_[focus_] == npos) { focus_ = npos; }
    if (anchor_ != npos && row_of_[anchor_] == npos) { anchor_ = npos; }
}

void Listing::recount() noexcept
{
    sel_count_ = 0;
    sel_bytes_ = 0;
    for (const std::uint32_t e : order_) {
        if (sel_[e] != 0) {
            ++sel_count_;
            sel_bytes_ += entries_[e].size;
        }
    }
}

std::optional<std::size_t> Listing::find_name(std::string_view name) const
{
    for (std::size_t r = 0; r < order_.size(); ++r) {
        if (core::text::compare_natural(entries_[order_[r]].name, name) == 0) { return r; }
    }
    return std::nullopt;
}

std::optional<std::size_t> Listing::find_prefix(std::string_view prefix, std::size_t from) const
{
    const std::size_t n = order_.size();
    if (n == 0 || prefix.empty()) { return std::nullopt; }
    const std::string p = core::text::lower_ascii(prefix);
    for (std::size_t k = 0; k < n; ++k) {
        const std::size_t r = (from + k) % n;
        const std::string_view name = entries_[order_[r]].name;
        if (name.size() < p.size()) { continue; }
        std::size_t i = 0;
        for (; i < p.size(); ++i) {
            char c = name[i];
            if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c + 32); }
            if (c != p[i]) { break; }
        }
        if (i == p.size()) { return r; }
    }
    return std::nullopt;
}

std::vector<std::size_t> Listing::selected_rows() const
{
    std::vector<std::size_t> rows;
    rows.reserve(sel_count_);
    for (std::size_t r = 0; r < order_.size(); ++r) {
        if (sel_[order_[r]] != 0) { rows.push_back(r); }
    }
    return rows;
}

void Listing::select_only(std::size_t row)
{
    std::fill(sel_.begin(), sel_.end(), std::uint8_t{0});
    if (row < order_.size()) { sel_[order_[row]] = 1; }
    recount();
}

void Listing::toggle(std::size_t row)
{
    if (row >= order_.size()) { return; }
    sel_[order_[row]] ^= 1;
    recount();
}

void Listing::set_selected(std::size_t row, bool on)
{
    if (row >= order_.size()) { return; }
    sel_[order_[row]] = on ? 1 : 0;
    recount();
}

void Listing::select_range(std::size_t a, std::size_t b, bool keep_existing)
{
    if (order_.empty()) { return; }
    if (!keep_existing) { std::fill(sel_.begin(), sel_.end(), std::uint8_t{0}); }
    const std::size_t lo = std::min(std::min(a, b), order_.size() - 1);
    const std::size_t hi = std::min(std::max(a, b), order_.size() - 1);
    for (std::size_t r = lo; r <= hi; ++r) { sel_[order_[r]] = 1; }
    recount();
}

void Listing::select_all()
{
    for (const std::uint32_t e : order_) { sel_[e] = 1; }
    recount();
}

void Listing::clear_selection()
{
    std::fill(sel_.begin(), sel_.end(), std::uint8_t{0});
    sel_count_ = 0;
    sel_bytes_ = 0;
}

std::size_t Listing::focus() const noexcept
{
    return focus_ == npos || focus_ >= row_of_.size() ? npos : row_of_[focus_];
}

std::size_t Listing::anchor() const noexcept
{
    return anchor_ == npos || anchor_ >= row_of_.size() ? npos : row_of_[anchor_];
}

void Listing::set_focus(std::size_t row, bool move_anchor)
{
    if (row >= order_.size()) { return; }
    focus_ = order_[row];
    if (move_anchor) { anchor_ = focus_; }
}

} // namespace prism::model
