#include "model/tags.hpp"

#include "core/file_entry.hpp"
#include "core/path.hpp"
#include "core/text.hpp"
#include "strata/config.hpp"

#include <algorithm>
#include <format>

namespace prism::model {

namespace {

[[nodiscard]] std::string key_of(std::string_view path) { return core::text::lower_ascii(core::path::normalize(path)); }

// `key` is `prefix` itself or something below it
[[nodiscard]] bool under(std::string_view key, std::string_view prefix) noexcept
{
    return key.starts_with(prefix) && (key.size() == prefix.size() || key[prefix.size()] == '\\' || prefix.back() == '\\');
}

} // namespace

std::uint16_t Tags::get(std::string_view path) const
{
    if (map_.empty()) { return 0; }
    const auto it = map_.find(key_of(path));
    return it == map_.end() ? std::uint16_t{0} : it->second;
}

bool Tags::set(std::string_view path, std::uint16_t mask)
{
    mask &= core::tag::all;
    const std::string k = key_of(path);
    if (k.empty()) { return false; }
    const auto it = map_.find(k);
    if (mask == 0) {
        if (it == map_.end()) { return false; }
        map_.erase(it);
    } else if (it != map_.end()) {
        if (it->second == mask) { return false; }
        it->second = mask;
    } else {
        if (map_.size() >= max_entries) { return false; }
        map_.emplace(k, mask);
    }
    ++revision_;
    return true;
}

void Tags::rename_prefix(std::string_view from, std::string_view to)
{
    if (map_.empty()) { return; }
    const std::string f = key_of(from);
    const std::string t = key_of(to);
    if (f.empty() || t.empty() || f == t) { return; }
    std::vector<std::pair<std::string, std::uint16_t>> moved;
    for (auto it = map_.begin(); it != map_.end();) {
        if (under(it->first, f)) {
            moved.emplace_back(t + it->first.substr(f.size()), it->second);
            it = map_.erase(it);
        } else {
            ++it;
        }
    }
    if (moved.empty()) { return; }
    for (auto& [k, m] : moved) { map_[std::move(k)] = m; }
    ++revision_;
}

void Tags::remove_prefix(std::string_view path)
{
    if (map_.empty()) { return; }
    const std::string p = key_of(path);
    if (p.empty()) { return; }
    const std::size_t before = map_.size();
    std::erase_if(map_, [&](const auto& kv) { return under(kv.first, p); });
    if (map_.size() != before) { ++revision_; }
}

void Tags::store(strata::config& c) const
{
    // sorted: the file is stable between saves
    std::vector<std::pair<std::string, std::uint16_t>> all(map_.begin(), map_.end());
    std::ranges::sort(all);
    c.set_int("tags", "count", static_cast<int>(all.size()));
    for (std::size_t i = 0; i < all.size(); ++i) {
        c.set("tags", std::format("p{}", i), all[i].first);
        c.set_int("tags", std::format("m{}", i), all[i].second);
    }
}

bool Tags::load(const strata::config& c)
{
    if (!c.has("tags", "count")) { return false; }
    map_.clear();
    const int n = std::clamp(c.get_int("tags", "count", 0), 0, static_cast<int>(max_entries));
    for (int i = 0; i < n; ++i) {
        const std::string_view p = c.get("tags", std::format("p{}", i));
        const auto mask = static_cast<std::uint16_t>(c.get_int("tags", std::format("m{}", i), 0) & core::tag::all);
        if (!p.empty() && mask != 0) { map_[std::string{p}] = mask; }
    }
    ++revision_;
    return true;
}

bool Tags::load_file(const std::string& path)
{
    strata::config c;
    return c.load_file(path) && load(c);
}

bool Tags::save_file(const std::string& path) const
{
    strata::config c;
    store(c);
    return c.save_file(path);
}

} // namespace prism::model
