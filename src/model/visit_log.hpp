#pragma once

// the folders you have been to, with how often and how recently: what the sidebar's Recent and Frequent lists show.
// pure data; revision() changes when something did, so the owner knows to save

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace prism::model {

class VisitLog {
public:
    // one more visit (a drive root is not worth listing: drives have their own section)
    void note(std::string_view path);
    void forget(std::string_view path);
    void clear();

    // newest first / most visited first (ties: newer first). at most `n`
    [[nodiscard]] std::vector<std::string> recent(std::size_t n) const;
    [[nodiscard]] std::vector<std::string> frequent(std::size_t n) const;

    [[nodiscard]] std::size_t size() const noexcept { return visits_.size(); }
    [[nodiscard]] std::size_t revision() const noexcept { return revision_; }

    [[nodiscard]] bool load_file(const std::string& path);
    [[nodiscard]] bool save_file(const std::string& path) const;

private:
    struct Visit {
        std::string   path;
        std::uint32_t count{};
        std::uint64_t last{}; // a counter, not a time: it only has to order visits
    };
    static constexpr std::size_t k_max = 200;

    std::vector<Visit> visits_;
    std::uint64_t      clock_{};
    std::size_t        revision_{};
};

} // namespace prism::model
