#pragma once

// one browsing context: where it is, how it got there (back / forward), and the listing of the current folder

#include "core/directory_loader.hpp"
#include "model/listing.hpp"

#include <functional>
#include <string>
#include <vector>

namespace prism::model {

class Tab {
public:
    explicit Tab(std::function<void()> wake);

    // history ---------------------------------------------------------------
    void navigate(const std::string& path);
    bool back();
    bool forward();
    bool up();
    void refresh();
    [[nodiscard]] bool can_back() const noexcept { return index_ > 0; }
    [[nodiscard]] bool can_forward() const noexcept { return index_ + 1 < history_.size(); }
    [[nodiscard]] bool can_up() const;

    [[nodiscard]] const std::string& path() const noexcept; // empty until the first navigate()
    [[nodiscard]] std::string        title() const;

    // listing -----------------------------------------------------------------
    [[nodiscard]] Listing&       listing() noexcept { return listing_; }
    [[nodiscard]] const Listing& listing() const noexcept { return listing_; }
    [[nodiscard]] bool           loading() const noexcept { return loader_.running(); }
    // win32 error of the last listing (0 = fine): access denied, path gone ...
    [[nodiscard]] std::uint32_t  error() const noexcept { return error_; }

    // pulls what the loader produced; true when the listing changed (the ui must redraw)
    bool poll();

    // view state the file view keeps per tab, restored when going back
    float scroll_y{};
    // set by navigation: the file view selects this name once the listing has it ("up" re-selects the folder you left)
    std::string reveal_name;
    // bumped whenever the visible rows are replaced (navigation, refresh); the view resets transient state on it
    std::uint32_t generation{};

private:
    struct Place {
        std::string path;
        float       scroll{};
        std::string focus_name;
    };

    void load(bool keep_scroll);
    void remember_view();

    std::vector<Place>       history_;
    std::size_t              index_{};
    Listing                  listing_;
    core::DirectoryLoader    loader_;
    std::uint32_t            error_{};
    float                    restore_scroll_{};
    bool                     restore_pending_{};
};

} // namespace prism::model
