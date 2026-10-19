#include "model/folder_sorts.hpp"

#include "core/path.hpp"
#include "core/text.hpp"
#include "strata/config.hpp"

#include <algorithm>
#include <format>

namespace prism::model {

namespace {

[[nodiscard]] std::string key_of(std::string_view folder) { return core::text::lower_ascii(core::path::normalize(folder)); }

} // namespace

std::optional<FolderSorts::Entry> FolderSorts::get(std::string_view folder) const
{
    const std::string k = key_of(folder);
    for (const auto& [path, e] : list_) {
        if (path == k) { return e; }
    }
    return std::nullopt;
}

bool FolderSorts::set(std::string_view folder, Entry e)
{
    const std::string k = key_of(folder);
    if (k.empty()) { return false; }
    const auto it = std::ranges::find_if(list_, [&](const auto& p) { return p.first == k; });
    if (it != list_.end()) {
        if (it->second == e) { return false; }
        list_.erase(it);
    }
    list_.emplace_back(k, e);
    if (list_.size() > max_entries) { list_.erase(list_.begin()); }
    ++revision_;
    return true;
}

void FolderSorts::clear()
{
    if (list_.empty()) { return; }
    list_.clear();
    ++revision_;
}

void FolderSorts::store(strata::config& c) const
{
    c.set_int("foldersort", "count", static_cast<int>(list_.size()));
    for (std::size_t i = 0; i < list_.size(); ++i) {
        c.set("foldersort", std::format("p{}", i), list_[i].first);
        c.set_int("foldersort", std::format("k{}", i), static_cast<int>(list_[i].second.key));
        c.set_bool("foldersort", std::format("a{}", i), list_[i].second.ascending);
    }
}

bool FolderSorts::load(const strata::config& c)
{
    if (!c.has("foldersort", "count")) { return false; }
    list_.clear();
    const int n = std::clamp(c.get_int("foldersort", "count", 0), 0, static_cast<int>(max_entries));
    for (int i = 0; i < n; ++i) {
        const std::string_view p = c.get("foldersort", std::format("p{}", i));
        if (p.empty()) { continue; }
        Entry e;
        e.key       = static_cast<SortKey>(std::clamp(c.get_int("foldersort", std::format("k{}", i), 0), 0, 3));
        e.ascending = c.get_bool("foldersort", std::format("a{}", i), true);
        list_.emplace_back(std::string{p}, e);
    }
    ++revision_;
    return true;
}

bool FolderSorts::load_file(const std::string& path)
{
    strata::config c;
    return c.load_file(path) && load(c);
}

bool FolderSorts::save_file(const std::string& path) const
{
    strata::config c;
    store(c);
    return c.save_file(path);
}

} // namespace prism::model
