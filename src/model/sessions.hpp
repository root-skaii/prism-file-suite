#pragma once

// named sessions: the tabs that were open, which one was active and the second pane's folder (if split). stored as text
// beside the settings. pure data, no ui

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace strata { class config; }

namespace prism::model {

struct Session {
    std::string              name;
    std::vector<std::string> tabs;
    int                      active{};
    std::string              right;   // the second pane's folder; "" = not split
};

class Sessions {
public:
    // replaces a session of the same name (case-insensitive); false when the name is empty or there are no tabs
    bool save(Session s);
    bool remove(std::string_view name);
    [[nodiscard]] std::optional<Session> find(std::string_view name) const;
    [[nodiscard]] const std::vector<Session>& all() const noexcept { return list_; }
    [[nodiscard]] std::size_t revision() const noexcept { return revision_; }

    void store(strata::config& c) const;
    bool load(const strata::config& c);
    bool load_file(const std::string& path);
    bool save_file(const std::string& path) const;

private:
    static constexpr std::size_t max_sessions = 50;
    static constexpr std::size_t max_tabs     = 32;
    std::vector<Session> list_;
    std::size_t          revision_{};
};

} // namespace prism::model
