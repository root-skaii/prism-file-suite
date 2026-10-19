#pragma once

// compares two folder trees by relative path, size and modified time (a quick compare: contents are not read).
// a folder that exists on one side only is one entry: copying it copies everything below. runs on a worker

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace prism::core {

enum class DiffKind : std::uint8_t {
    only_left,
    only_right,
    left_newer,     // same name, the left file was changed more recently
    right_newer,
    size_differs,   // same modified time, different size
    type_differs,   // a file on one side, a folder on the other
    same,
};

struct DiffItem {
    std::string   rel;          // relative path, as spelled on the side it exists on (left when on both)
    DiffKind      kind{DiffKind::same};
    bool          is_dir{};     // the left side's entry when it has one, else the right's
    std::uint64_t left_size{}, right_size{};
    std::uint64_t left_modified{}, right_modified{}; // FILETIME
};

class FolderCompare {
public:
    FolderCompare() = default;
    ~FolderCompare() { cancel(); }
    FolderCompare(const FolderCompare&)            = delete;
    FolderCompare& operator=(const FolderCompare&) = delete;

    // `wake` runs on the worker when it finishes (and now and then while it runs)
    void start(const std::string& left, const std::string& right, bool recursive, std::function<void()> wake = {});
    void cancel() noexcept;

    [[nodiscard]] bool          running() const noexcept { return s_ && !s_->done.load(); }
    [[nodiscard]] std::uint64_t seen() const noexcept { return s_ ? s_->seen.load() : 0; }
    // empty until done. sorted by path, folders before their contents
    [[nodiscard]] std::shared_ptr<const std::vector<DiffItem>> result() const;

private:
    struct State {
        std::atomic<std::uint64_t> seen{};
        std::atomic<bool>          done{}, cancel{};
        mutable std::mutex         lock;
        std::shared_ptr<const std::vector<DiffItem>> items;
    };
    std::shared_ptr<State> s_;
};

} // namespace prism::core
