#pragma once

// finds folders that hold no files at any depth (only other empty folders, or nothing). reports the outermost ones, so a
// whole empty tree is one entry. links are not followed. runs on a worker

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace prism::core {

class EmptyFolderScan {
public:
    EmptyFolderScan() = default;
    ~EmptyFolderScan() { cancel(); }
    EmptyFolderScan(const EmptyFolderScan&)            = delete;
    EmptyFolderScan& operator=(const EmptyFolderScan&) = delete;

    void start(const std::string& root, std::function<void()> wake = {});
    void cancel() noexcept;

    [[nodiscard]] bool          done() const noexcept { return s_ && s_->done.load(); }
    [[nodiscard]] std::uint64_t folders_seen() const noexcept { return s_ ? s_->seen.load() : 0; }
    // the empty folders found (the root itself is never listed); valid once done()
    [[nodiscard]] std::vector<std::string> result() const;

private:
    struct State {
        std::atomic<std::uint64_t> seen{};
        std::atomic<bool>          done{}, cancel{};
        mutable std::mutex         lock;
        std::vector<std::string>   found;
    };
    std::shared_ptr<State> s_;
};

} // namespace prism::core
