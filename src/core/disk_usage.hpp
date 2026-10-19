#pragma once

// where the space goes: a background scan of a folder into a size tree. folders are kept five levels deep (deeper ones only
// add to their ancestor's total: scan the folder itself to look inside), and each folder keeps its biggest files while the
// rest are lumped into one entry, so the tree stays small however large the disk is. links are not followed

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace prism::core {

struct UsageNode {
    std::string  name;
    std::uint64_t bytes{};
    std::uint64_t files{};       // files below (1 for a file)
    bool         is_dir{};
    bool         lumped{};       // "N smaller files": no single item behind it
    std::vector<UsageNode> children; // biggest first; empty for a file, a lump, or a folder past the depth limit
};

class DiskUsage {
public:
    DiskUsage() = default;
    ~DiskUsage() { cancel(); }
    DiskUsage(const DiskUsage&)            = delete;
    DiskUsage& operator=(const DiskUsage&) = delete;

    // `wake` runs on the worker when the scan finishes
    void start(const std::string& root, std::function<void()> wake = {});
    void cancel() noexcept;

    [[nodiscard]] bool          running() const noexcept { return s_ && !s_->done.load(); }
    [[nodiscard]] std::uint64_t files() const noexcept { return s_ ? s_->files.load() : 0; }
    [[nodiscard]] std::uint64_t bytes() const noexcept { return s_ ? s_->bytes.load() : 0; }
    // null until the scan is done (or when it was cancelled)
    [[nodiscard]] std::shared_ptr<const UsageNode> result() const;

    static constexpr int         depth_limit = 5;   // levels of folders kept below the root
    static constexpr std::size_t keep_files  = 24;  // individual files kept per folder

private:
    struct State {
        std::atomic<std::uint64_t> files{}, bytes{};
        std::atomic<bool>          done{}, cancel{};
        mutable std::mutex         lock;
        std::shared_ptr<const UsageNode> root;
    };
    std::shared_ptr<State> s_;
};

} // namespace prism::core
