#pragma once

// git status for the folder on screen, so the file list can badge changed files: modified, added, deleted, untracked,
// conflicted. git itself does the work (`git status --porcelain`) on a worker thread; nothing runs on the ui thread.
// no git installed, or a folder outside any repository: no badges, silently

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace prism::core {

// one repository's state. paths are relative to `root`, lower case, backslashes
struct GitRepo {
    std::string root;     // the working tree, normalized ("C:\\code\\prism")
    std::string branch;   // "main", "HEAD (detached)"
    int         ahead{}, behind{};
    std::unordered_map<std::string, char> files;      // path -> code
    std::unordered_map<std::string, char> dirs;       // a folder with changes below it -> the strongest code
    std::unordered_set<std::string>       untracked_dirs;

    // 'M' modified, 'A' added, 'D' deleted, 'R' renamed, '?' untracked, '!' conflict, 0 clean
    [[nodiscard]] char code_for(std::string_view rel, bool is_dir) const;
    // `path` (absolute) relative to the root, lower case; empty for the root itself or a path outside it
    [[nodiscard]] std::string relative(std::string_view path) const;
    [[nodiscard]] bool dirty() const noexcept { return !files.empty() || !untracked_dirs.empty(); }
};

// the output of `git status --porcelain=v1 -z --branch` (pure; selftested)
[[nodiscard]] GitRepo parse_git_status(std::string_view output);

class GitStatus {
public:
    explicit GitStatus(std::function<void()> wake);
    ~GitStatus();
    GitStatus(const GitStatus&)            = delete;
    GitStatus& operator=(const GitStatus&) = delete;

    // the repository that holds `folder` (null: none, git missing or not known yet). Asks for a refresh when the answer is
    // older than `max_age`: the old one keeps being returned until the new one lands
    [[nodiscard]] std::shared_ptr<const GitRepo> lookup(const std::string& folder);
    // something changed on disk (a file operation finished, F5): everything is looked at again on the next lookup
    void invalidate();

    std::chrono::milliseconds max_age{4000};

private:
    struct FolderInfo { std::string root; bool resolved{}; bool queued{}; };
    struct RepoInfo { std::shared_ptr<const GitRepo> repo; std::chrono::steady_clock::time_point at{}; bool queued{}; };
    void run(std::stop_token st);

    std::function<void()>                 wake_;
    std::mutex                            mutex_;
    std::condition_variable_any           cv_;
    std::map<std::string, FolderInfo>     folders_;
    std::map<std::string, RepoInfo>       repos_;
    std::deque<std::string>               queue_;   // folders to resolve / look at
    std::atomic<bool>                     quit_{};
    std::jthread                          thread_;
};

} // namespace prism::core
