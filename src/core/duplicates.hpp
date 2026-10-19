#pragma once

// finds files with identical content below a folder, on a worker thread: files are bucketed by size, the buckets that
// have company are compared by the hash of their first 64 KB, and only the survivors are read completely (SHA-256).
// the ui polls the counters while it runs and reads the groups when it is done

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace prism::core {

struct DuplicateGroup {
    std::uint64_t            size{};   // of each file
    std::vector<std::string> paths;    // oldest modification first: the first one is the likely original
};

class DuplicateScan {
public:
    enum class Stage { idle, listing, comparing, done };

    DuplicateScan() = default;
    ~DuplicateScan() { cancel(); }
    DuplicateScan(const DuplicateScan&)            = delete;
    DuplicateScan& operator=(const DuplicateScan&) = delete;

    // files smaller than `min_size` bytes are ignored (empty files are all "equal" and never interesting)
    void start(const std::string& folder, std::uint64_t min_size = 1);
    void cancel() noexcept;

    [[nodiscard]] Stage         stage() const noexcept { return s_ ? static_cast<Stage>(s_->stage.load()) : Stage::idle; }
    [[nodiscard]] std::uint64_t files_seen() const noexcept { return s_ ? s_->files.load() : 0; }
    [[nodiscard]] float         progress() const noexcept; // of the comparing stage, 0..1
    // biggest wasted space first; complete once stage() == done
    [[nodiscard]] std::vector<DuplicateGroup> groups() const;
    // bytes that would be freed by keeping one file per group
    [[nodiscard]] std::uint64_t reclaimable() const;

private:
    struct State {
        std::atomic<int>            stage{0};
        std::atomic<bool>           cancel{};
        std::atomic<std::uint64_t>  files{}, to_read{}, read{};
        mutable std::mutex          lock;
        std::vector<DuplicateGroup> groups;
    };
    std::shared_ptr<State> s_;
};

} // namespace prism::core
