#include "model/listing.hpp"

#include "core/text.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <chrono>
#include <ctime>
#endif

#include <algorithm>
#include <iterator>
#include <unordered_set>

namespace prism::model {

using core::FileEntry;

namespace {

// search results are relative paths: they sort by the file's own name first, the folder breaks ties
[[nodiscard]] int compare_names(std::string_view a, std::string_view b, bool natural) noexcept
{
    const auto leaf = [](std::string_view s) { const std::size_t i = s.rfind('\\'); return i == std::string_view::npos ? s : s.substr(i + 1); };
    const auto cmp = natural ? core::text::compare_natural : core::text::compare_plain;
    const int c = cmp(leaf(a), leaf(b));
    return c != 0 ? c : cmp(a, b);
}

constexpr std::uint64_t k_ticks_per_day = 864000000000ull; // FILETIME: 100 ns

constexpr std::string_view k_modified_titles[] = {"Today", "Yesterday", "Earlier this week", "Earlier this month", "Earlier this year", "A long time ago"};
constexpr std::string_view k_size_titles[]     = {"Folders", "Empty", "Tiny (under 16 KB)", "Small (under 1 MB)", "Medium (under 128 MB)", "Large (under 1 GB)", "Huge (1 GB and more)"};
constexpr std::string_view k_kind_titles[]     = {"Folders", "Images", "Videos", "Audio", "Archives", "Documents", "Spreadsheets", "Presentations", "PDF files", "Code",
                                                  "Text files", "Programs", "Libraries", "Settings files", "Fonts", "Disk images", "Shortcuts", "Drives", "Other files"};
static_assert(std::size(k_kind_titles) == static_cast<std::size_t>(core::FileKind::count_));

// the place of an entry among the groups of `by`; `today` is the local day number of now
[[nodiscard]] std::uint8_t rank_of(GroupBy by, const FileEntry& e, std::uint64_t today, std::int64_t utc_to_local) noexcept
{
    switch (by) {
    case GroupBy::name: {
        const std::string_view leaf = [&] { const std::string_view n = e.display(); const std::size_t i = n.rfind('\\'); return n.substr(i == std::string_view::npos ? 0 : i + 1); }();
        const char c = leaf.empty() ? '\0' : leaf.front();
        if (c >= 'a' && c <= 'z') { return static_cast<std::uint8_t>(c - 'a'); }
        if (c >= 'A' && c <= 'Z') { return static_cast<std::uint8_t>(c - 'A'); }
        return 26; // digits, symbols, non-latin
    }
    case GroupBy::type:
        return static_cast<std::uint8_t>(e.is_dir() ? core::FileKind::folder : e.kind);
    case GroupBy::modified: {
        const std::int64_t local = static_cast<std::int64_t>(e.modified) + utc_to_local;
        const std::uint64_t day  = local < 0 ? 0 : static_cast<std::uint64_t>(local) / k_ticks_per_day;
        const std::uint64_t age  = day >= today ? 0 : today - day;
        if (age == 0) { return 0; }
        if (age == 1) { return 1; }
        if (age < 7)  { return 2; }
        if (age < 31) { return 3; }
        if (age < 366) { return 4; }
        return 5;
    }
    case GroupBy::size:
        if (e.is_dir()) { return 0; }
        if (e.size == 0) { return 1; }
        if (e.size < (16ull << 10)) { return 2; }
        if (e.size < (1ull << 20)) { return 3; }
        if (e.size < (128ull << 20)) { return 4; }
        if (e.size < (1ull << 30)) { return 5; }
        return 6;
    case GroupBy::none: break;
    }
    return 0;
}

} // namespace

void Listing::clear()
{
    entries_.clear();
    sel_.clear();
    order_.clear();
    row_of_.clear();
    rank_.clear();
    group_starts_.clear();
    ++epoch_;
    tag_token_    = ~std::uint64_t{0};
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

void Listing::replace_entries(std::vector<FileEntry>&& fresh)
{
    std::unordered_set<std::string> chosen;
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (sel_[i] != 0) { chosen.insert(entries_[i].name); }
    }
    const std::string focus_name  = focus_ != npos ? entries_[focus_].name : std::string{};
    const std::string anchor_name = anchor_ != npos ? entries_[anchor_].name : std::string{};

    entries_ = std::move(fresh);
    sel_.assign(entries_.size(), 0);
    focus_  = npos;
    anchor_ = npos;
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (chosen.count(entries_[i].name) != 0) { sel_[i] = 1; }
        if (!focus_name.empty() && entries_[i].name == focus_name) { focus_ = static_cast<std::uint32_t>(i); }
        if (!anchor_name.empty() && entries_[i].name == anchor_name) { anchor_ = static_cast<std::uint32_t>(i); }
    }
    tag_token_ = ~std::uint64_t{0};
    finished_  = true;
    rebuild();
}

void Listing::remove_names(const std::unordered_set<std::string>& names)
{
    if (!finished_ || names.empty()) { return; }
    std::vector<FileEntry> kept;
    kept.reserve(entries_.size());
    for (FileEntry& e : entries_) {
        if (names.count(e.name) == 0) { kept.push_back(std::move(e)); }
    }
    if (kept.size() != entries_.size()) { replace_entries(std::move(kept)); }
}

bool Listing::upsert_entry(FileEntry&& entry)
{
    if (!finished_) { return false; }
    // locate existing entry (case-insensitive)
    std::size_t idx = entries_.size();
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (core::text::compare_plain(entries_[i].name, entry.name) == 0) {
            idx = i;
            break;
        }
    }
    if (idx < entries_.size()) {
        // update existing entry in place, preserving its selection and tags
        const bool          was_vis = row_of_[idx] != npos;
        const bool          was_dir = entries_[idx].is_dir();
        const std::uint64_t old_sz  = entries_[idx].size;
        const std::uint16_t tags    = entries_[idx].tags;
        entry.tags                  = tags;
        entries_[idx]               = std::move(entry);
        const bool now_vis          = visible(entries_[idx]);
        if (sel_[idx] != 0) {
            if (!now_vis) {
                sel_[idx] = 0;
                --sel_count_;
                sel_bytes_ -= old_sz;
            } else {
                sel_bytes_ = sel_bytes_ - old_sz + entries_[idx].size;
            }
        }
        if (group_ != GroupBy::none || was_vis != now_vis) {
            rebuild();
            return true;
        }
        if (!now_vis) { return true; }
        if (was_dir != entries_[idx].is_dir()) { folder_count_ += entries_[idx].is_dir() ? 1 : static_cast<std::size_t>(-1); }
        // check if the updated entry is still in sorted position relative to its neighbors
        const std::size_t r = row_of_[idx];
        const bool ok_left  = r == 0 || !less(static_cast<std::uint32_t>(idx), order_[r - 1]);
        const bool ok_right = r + 1 >= order_.size() || !less(order_[r + 1], static_cast<std::uint32_t>(idx));
        if (!ok_left || !ok_right) {
            const auto id = static_cast<std::uint32_t>(idx);
            order_.erase(order_.begin() + static_cast<std::ptrdiff_t>(r));
            const auto pos = std::lower_bound(order_.begin(), order_.end(), id, [this](std::uint32_t a, std::uint32_t b) { return less(a, b); });
            const std::size_t nr = static_cast<std::size_t>(pos - order_.begin());
            order_.insert(pos, id);
            const std::size_t lo = std::min(r, nr), hi = std::max(r, nr);
            for (std::size_t k = lo; k <= hi; ++k) { row_of_[order_[k]] = static_cast<std::uint32_t>(k); }
        }
        return true;
    }

    // new entry: append to entries_, insert into sorted order_ in O(log N) comparison + O(N) shift
    const auto id = static_cast<std::uint32_t>(entries_.size());
    entries_.push_back(std::move(entry));
    sel_.push_back(0);
    row_of_.push_back(npos);
    tag_token_ = ~std::uint64_t{0};
    if (group_ != GroupBy::none) {
        rebuild();
        return true;
    }
    if (!visible(entries_[id])) { return true; }
    if (entries_[id].is_dir()) { ++folder_count_; }
    const auto pos = std::lower_bound(order_.begin(), order_.end(), id, [this](std::uint32_t a, std::uint32_t b) { return less(a, b); });
    const std::size_t r = static_cast<std::size_t>(pos - order_.begin());
    order_.insert(pos, id);
    for (std::size_t k = r; k < order_.size(); ++k) { row_of_[order_[k]] = static_cast<std::uint32_t>(k); }
    return true;
}

bool Listing::remove_name(std::string_view name)
{
    if (!finished_) { return false; }
    std::size_t idx = entries_.size();
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (core::text::compare_plain(entries_[i].name, name) == 0) {
            idx = i;
            break;
        }
    }
    if (idx >= entries_.size()) { return false; }

    const std::uint32_t r = row_of_[idx];
    if (r != npos) {
        if (entries_[idx].is_dir()) { --folder_count_; }
        if (sel_[idx] != 0) {
            --sel_count_;
            sel_bytes_ -= entries_[idx].size;
        }
        order_.erase(order_.begin() + static_cast<std::ptrdiff_t>(r));
        for (std::size_t k = r; k < order_.size(); ++k) { row_of_[order_[k]] = static_cast<std::uint32_t>(k); }
    }
    if (focus_ == idx)  { focus_  = npos; }
    if (anchor_ == idx) { anchor_ = npos; }

    // swap-pop from entries_ so removing one entry never shifts 200k FileEntry structs
    const std::size_t last = entries_.size() - 1;
    if (idx != last) {
        entries_[idx] = std::move(entries_[last]);
        sel_[idx]     = sel_[last];
        row_of_[idx]  = row_of_[last];
        if (!rank_.empty() && last < rank_.size()) { rank_[idx] = rank_[last]; }
        if (row_of_[idx] != npos) { order_[row_of_[idx]] = static_cast<std::uint32_t>(idx); }
        if (focus_ == last)  { focus_  = static_cast<std::uint32_t>(idx); }
        if (anchor_ == last) { anchor_ = static_cast<std::uint32_t>(idx); }
    }
    entries_.pop_back();
    sel_.pop_back();
    row_of_.pop_back();
    if (!rank_.empty()) { rank_.pop_back(); }
    if (group_ != GroupBy::none) { index_rows(); }
    return true;
}

void Listing::set_sort(SortSpec s)
{
    if (s == sort_) { return; }
    sort_ = s;
    if (finished_) { rebuild(); }
}

void Listing::sync_tags(std::uint64_t token, const std::function<std::uint16_t(const FileEntry&)>& lookup)
{
    if (token == tag_token_) { return; }
    tag_token_ = token;
    bool changed = false;
    for (FileEntry& e : entries_) {
        const std::uint16_t now = lookup ? lookup(e) : std::uint16_t{0};
        if (now != e.tags) { e.tags = now; changed = true; }
    }
    if (changed && finished_ && query_.uses_tags()) { rebuild(); }
}

void Listing::set_group(GroupBy g)
{
    if (g == group_) { return; }
    group_ = g;
    if (finished_) { rebuild(); }
}

std::string Listing::group_title(std::size_t row) const
{
    if (group_ == GroupBy::none || row >= order_.size()) { return {}; }
    const std::uint8_t r = rank_[order_[row]];
    switch (group_) {
    case GroupBy::name:     return r >= 26 ? "0-9 and symbols" : std::string(1, static_cast<char>('A' + r));
    case GroupBy::type:     return std::string{k_kind_titles[r]};
    case GroupBy::modified: return std::string{k_modified_titles[r]};
    case GroupBy::size:     return std::string{k_size_titles[r]};
    case GroupBy::none:     break;
    }
    return {};
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
    if (group_ != GroupBy::none && rank_[ia] != rank_[ib]) { return (rank_[ia] < rank_[ib]) != group_flip_; }
    // (the grouping ignores the direction: folders stay above, or below, whichever way the column is sorted)
    if (sort_.folders != FolderPlacement::mixed && a.is_dir() != b.is_dir()) { return (sort_.folders == FolderPlacement::first) == a.is_dir(); }
    if (sort_.hidden_last && a.is_hidden() != b.is_hidden()) { return !a.is_hidden(); }

    int c = 0;
    switch (sort_.key) {
    case SortKey::name:     c = compare_names(a.display(), b.display(), sort_.natural); break;
    case SortKey::modified: c = a.modified < b.modified ? -1 : (a.modified > b.modified ? 1 : 0); break;
    case SortKey::size:     c = a.size < b.size ? -1 : (a.size > b.size ? 1 : 0); break;
    case SortKey::type:
        if (!a.original_path().empty() || !b.original_path().empty()) {
            // Recycle Bin's third column displays Original location, so sort by parent folder instead of file type
            const auto dir_of = [](std::string_view p) noexcept {
                const std::size_t cut = p.rfind('\\');
                return cut == std::string_view::npos ? p : p.substr(0, cut);
            };
            c = core::text::compare_natural(dir_of(a.original_path()), dir_of(b.original_path()));
        } else if (a.kind != b.kind) {
            c = a.kind < b.kind ? -1 : 1;
        } else {
            c = core::text::compare_natural(a.ext(), b.ext());
        }
        break;
    }
    if (c == 0 && sort_.key != SortKey::name) { c = compare_names(a.display(), b.display(), sort_.natural); return c < 0; } // ties by name, always ascending
    return sort_.ascending ? c < 0 : c > 0;
}

void Listing::rebuild()
{
    order_.clear();
    order_.reserve(entries_.size());
    folder_count_ = 0;
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (!visible(entries_[i])) {
            sel_[i] = 0; // (selection of entries that just became hidden is dropped: it could no longer be seen or acted on)
            continue;
        }
        order_.push_back(static_cast<std::uint32_t>(i));
        if (entries_[i].is_dir()) { ++folder_count_; }
    }
    if (group_ != GroupBy::none) {
        // local day number of now, and the shift that turns an entry's UTC time into local time (current offset)
#ifdef _WIN32
        FILETIME ft{};
        ::GetSystemTimeAsFileTime(&ft);
        TIME_ZONE_INFORMATION tz{};
        const DWORD id = ::GetTimeZoneInformation(&tz);
        const LONG  bias = tz.Bias + (id == TIME_ZONE_ID_DAYLIGHT ? tz.DaylightBias : tz.StandardBias);
        const std::int64_t utc_to_local = -static_cast<std::int64_t>(bias) * 600000000ll;
        const std::uint64_t now = (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32 | ft.dwLowDateTime);
#else
        const std::time_t now_t = std::time(nullptr);
        std::tm local_tm{};
        ::localtime_r(&now_t, &local_tm);
        const std::int64_t utc_to_local = static_cast<std::int64_t>(local_tm.tm_gmtoff) * 10000000ll;
        const std::uint64_t now = (static_cast<std::uint64_t>(now_t) + 11644473600ull) * 10000000ull;
#endif
        const std::uint64_t today = static_cast<std::uint64_t>(static_cast<std::int64_t>(now) + utc_to_local) / k_ticks_per_day;
        rank_.assign(entries_.size(), 0);
        for (const std::uint32_t e : order_) { rank_[e] = rank_of(group_, entries_[e], today, utc_to_local); }
        // (sorting a column descending also turns the groups made of it around: newest day first, biggest first ...)
        const SortKey same = group_ == GroupBy::name ? SortKey::name : group_ == GroupBy::type ? SortKey::type : group_ == GroupBy::modified ? SortKey::modified : SortKey::size;
        group_flip_ = !sort_.ascending && sort_.key == same && group_ != GroupBy::modified;
        if (group_ == GroupBy::modified) { group_flip_ = sort_.key == SortKey::modified && sort_.ascending; } // (newest first is the natural order of "Today ... Older")
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

    group_starts_.clear();
    if (group_ != GroupBy::none && finished_ && rank_.size() == entries_.size()) {
        for (std::size_t r = 0; r < order_.size(); ++r) {
            if (r == 0 || rank_[order_[r]] != rank_[order_[r - 1]]) { group_starts_.push_back(static_cast<std::uint32_t>(r)); }
        }
    }
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
    for (std::size_t k = 0; k < n; ++k) {
        const std::size_t r = (from + k) % n;
        if (core::text::starts_with_icase(entries_[order_[r]].display(), prefix)) { return r; }
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
    sel_count_ = 0;
    sel_bytes_ = 0;
    if (row < order_.size()) {
        const std::uint32_t e = order_[row];
        sel_[e]    = 1;
        sel_count_ = 1;
        sel_bytes_ = entries_[e].size;
    }
}

void Listing::toggle(std::size_t row)
{
    if (row >= order_.size()) { return; }
    const std::uint32_t e = order_[row];
    if (sel_[e] != 0) {
        sel_[e] = 0;
        --sel_count_;
        sel_bytes_ -= entries_[e].size;
    } else {
        sel_[e] = 1;
        ++sel_count_;
        sel_bytes_ += entries_[e].size;
    }
}

void Listing::set_selected(std::size_t row, bool on)
{
    if (row >= order_.size()) { return; }
    const std::uint32_t e = order_[row];
    const std::uint8_t  v = on ? 1 : 0;
    if (sel_[e] == v) { return; }
    sel_[e] = v;
    if (on) {
        ++sel_count_;
        sel_bytes_ += entries_[e].size;
    } else {
        --sel_count_;
        sel_bytes_ -= entries_[e].size;
    }
}

void Listing::select_range(std::size_t a, std::size_t b, bool keep_existing)
{
    if (order_.empty()) { return; }
    if (!keep_existing) {
        std::fill(sel_.begin(), sel_.end(), std::uint8_t{0});
        sel_count_ = 0;
        sel_bytes_ = 0;
    }
    const std::size_t lo = std::min(std::min(a, b), order_.size() - 1);
    const std::size_t hi = std::min(std::max(a, b), order_.size() - 1);
    for (std::size_t r = lo; r <= hi; ++r) {
        const std::uint32_t e = order_[r];
        if (sel_[e] == 0) {
            sel_[e] = 1;
            ++sel_count_;
            sel_bytes_ += entries_[e].size;
        }
    }
}

void Listing::select_all()
{
    sel_count_ = 0;
    sel_bytes_ = 0;
    for (const std::uint32_t e : order_) {
        sel_[e] = 1;
        ++sel_count_;
        sel_bytes_ += entries_[e].size;
    }
}

void Listing::invert_selection()
{
    sel_count_ = 0;
    sel_bytes_ = 0;
    for (const std::uint32_t e : order_) {
        sel_[e] ^= 1;
        if (sel_[e] != 0) {
            ++sel_count_;
            sel_bytes_ += entries_[e].size;
        }
    }
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
