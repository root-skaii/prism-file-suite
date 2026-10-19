#pragma once

// colour labels per path (core::tag bits), kept in one text file beside the settings. keyed by lower-case full path, so
// the app moves a label along when it renames or moves the item itself (rename_prefix). pure data, no ui

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace strata { class config; }

namespace prism::model {

class Tags {
public:
    [[nodiscard]] std::uint16_t get(std::string_view path) const;
    [[nodiscard]] bool any() const noexcept { return !map_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return map_.size(); }
    [[nodiscard]] std::size_t revision() const noexcept { return revision_; }

    // false: nothing changed. mask 0 removes the entry
    bool set(std::string_view path, std::uint16_t mask);
    // an item and everything below it moved from `from` to `to` (a rename or move done by the app)
    void rename_prefix(std::string_view from, std::string_view to);
    // an item and everything below it is gone
    void remove_prefix(std::string_view path);

    void store(strata::config& c) const;
    bool load(const strata::config& c);
    bool load_file(const std::string& path);
    bool save_file(const std::string& path) const;

private:
    static constexpr std::size_t max_entries = 50000;
    std::unordered_map<std::string, std::uint16_t> map_; // lower-case path -> mask
    std::size_t                                    revision_{};
};

} // namespace prism::model
