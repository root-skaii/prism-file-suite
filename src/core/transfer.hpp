#pragma once

// the built-in copy / move engine: jobs run one after the other on a worker, with byte progress, speed, pause, cancel and
// a conflict prompt the ui answers (replace / skip / keep both, for this file or all of them). copies keep dates and
// attributes (CopyFileEx); a move on the same volume is a rename, across volumes copy then delete

#include "core/op_types.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace prism::core {

enum class ConflictChoice { replace, skip, keep_both, cancel };

struct ConflictInfo {
    std::uint64_t serial{};                // one per question: an answer to an older one is ignored
    std::string   source, target;          // full paths
    std::uint64_t source_size{}, target_size{};
    std::uint64_t source_modified{}, target_modified{}; // FILETIME
    bool          is_dir{};                // both are folders: they are merged, not replaced
};

struct TransferProgress {
    bool          active{};          // a job is running (or paused)
    bool          paused{};
    bool          scanning{};        // still counting what there is to copy
    OpKind        kind{OpKind::copy};
    std::string   title;             // "Copying 12 items to Documents"
    std::string   current;           // the file in progress
    std::uint64_t bytes_done{}, bytes_total{};
    std::uint64_t items_done{}, items_total{};
    double        bytes_per_second{};
    std::size_t   waiting{};         // jobs queued behind this one
};

class TransferQueue {
public:
    explicit TransferQueue(std::function<void()> wake) : wake_{std::move(wake)} {}
    ~TransferQueue();
    TransferQueue(const TransferQueue&)            = delete;
    TransferQueue& operator=(const TransferQueue&) = delete;

    // queues a copy or move of `sources` into the folder `dest` (a copy into the folder it came from makes "name - Copy")
    void add(OpKind kind, std::vector<std::string> sources, std::string dest, bool silent);

    [[nodiscard]] TransferProgress progress() const;
    [[nodiscard]] bool busy() const;
    void set_paused(bool paused);
    void cancel();                       // the running job (what it already copied stays) and everything waiting
    // the question the worker is waiting on, if any
    [[nodiscard]] std::optional<ConflictInfo> conflict() const;
    void resolve(std::uint64_t serial, ConflictChoice choice, bool for_all);
    // finished jobs, in order
    [[nodiscard]] bool poll(OpResult& out);

    struct Run; // (the state of the job being run; transfer.cpp)

private:
    struct Job {
        OpKind                   kind{};
        std::vector<std::string> sources;
        std::string              dest;
        bool                     silent{};
    };
    void work();

    std::function<void()>      wake_;
    mutable std::mutex         lock_;
    std::condition_variable    cv_;
    std::vector<Job>           queue_;
    std::vector<OpResult>      results_;
    TransferProgress           shown_;
    std::optional<ConflictInfo> asking_;
    std::optional<ConflictChoice> answer_;
    bool                       answer_all_{};
    std::uint64_t              serial_{};
    std::atomic<bool>          paused_{};   // (read by the copy callback without the lock)
    std::atomic<bool>          cancel_{};
    std::atomic<bool>          quit_{};
    bool                       running_{};
    std::thread                thread_;
};

} // namespace prism::core
