#pragma once

// one browsing context: where it is, how it got there (back / forward), and the listing of the current folder

#include "core/directory_loader.hpp"
#include "core/folder_watcher.hpp"
#include "model/listing.hpp"

#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace prism::model {

class Tab {
public:
    explicit Tab(std::function<void()> wake);
    // rebinds loaders and watcher when a tab moves into another PrismFS window
    void set_waker(std::function<void()> wake);

    // history ---------------------------------------------------------------
    void navigate(const std::string& path);
    bool back();
    bool forward();
    bool up();
    void refresh();
    // re-reads in the background and swaps the entries in when done: selection and scroll stay, the list never empties
    void refresh_quiet();
    // paths whose deletion just started: the ones inside this folder leave the list now
    void forget(const std::vector<std::string>& paths);
    // recursive search below the current folder; results stream into the listing until clear_search(). an invalid or
    // empty query does nothing
    void search(core::Query query);
    void clear_search();
    [[nodiscard]] bool searching() const noexcept { return searching_; }
    [[nodiscard]] const std::string& search_query() const noexcept { return search_.text(); }
    [[nodiscard]] bool can_back() const noexcept { return index_ > 0; }
    [[nodiscard]] bool can_forward() const noexcept { return index_ + 1 < history_.size(); }
    [[nodiscard]] bool can_up() const;
    // back / forward history entries as (history_index, path), nearest step first
    [[nodiscard]] std::vector<std::pair<std::size_t, std::string>> back_history() const;
    [[nodiscard]] std::vector<std::pair<std::size_t, std::string>> forward_history() const;
    bool jump_history(std::size_t history_index);

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
    // seconds until a debounced watcher refresh is due (3600 = none pending)
    [[nodiscard]] double next_wake_seconds() const noexcept;

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
    // background re-read after another program changed the folder: replaces the entries in place (no clear, scroll stays)
    core::DirectoryLoader    quiet_loader_;
    core::FolderWatcher      watcher_;
    std::vector<core::FileEntry> quiet_;
    bool                     quiet_dirty_{};
    bool                     watch_pending_{};
    std::chrono::steady_clock::time_point watch_due_{};
    std::chrono::steady_clock::time_point watch_first_{};
    std::uint32_t            error_{};
    core::Query              search_;
    bool                     searching_{};
    float                    restore_scroll_{};
    bool                     restore_pending_{};
};

} // namespace prism::model
