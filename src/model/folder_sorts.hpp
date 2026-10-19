#pragma once

// the sort order remembered per folder (Settings::sort_per_folder). most recently set last, capped, stored as text beside
// the settings. pure data, no ui

#include "model/listing.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace strata { class config; }

namespace prism::model {

class FolderSorts {
public:
    struct Entry {
        SortKey key{SortKey::name};
        bool    ascending{true};
        friend bool operator==(const Entry&, const Entry&) = default;
    };

    [[nodiscard]] std::optional<Entry> get(std::string_view folder) const;
    // false: the folder already had this order
    bool set(std::string_view folder, Entry e);
    void clear();
    [[nodiscard]] std::size_t size() const noexcept { return list_.size(); }
    [[nodiscard]] std::size_t revision() const noexcept { return revision_; }

    void store(strata::config& c) const;
    bool load(const strata::config& c);
    bool load_file(const std::string& path);
    bool save_file(const std::string& path) const;

private:
    static constexpr std::size_t max_entries = 500;
    std::vector<std::pair<std::string, Entry>> list_; // key: lower case path
    std::size_t                                revision_{};
};

} // namespace prism::model
