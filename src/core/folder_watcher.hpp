#pragma once

// watches one directory using overlapped ReadDirectoryChangesW on the Windows thread pool:
// - opening the directory handle happens off the ui thread (never blocks on slow/dead UNC shares)
// - one-shot CreateThreadpoolWait never spins if the watched folder is deleted or a volume disappears
// - small bursts of changes carry pre-stat'd FileEntry deltas (built on the threadpool thread, zero ui I/O) so a
//   200k-file folder patches in O(1)/O(log N) instead of re-enumerating; overflows fall back to a full quiet reload.

#include "core/directory_loader.hpp"
#include "core/file_entry.hpp"
#include "core/path.hpp"
#include "core/text.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <poll.h>
#include <sys/inotify.h>
#include <thread>
#include <unistd.h>
#endif

namespace prism::core {

struct FolderDelta {
    enum class Kind : std::uint8_t { upsert, remove };
    Kind        kind{Kind::upsert};
    std::string name;  // leaf filename (utf-8)
    FileEntry   entry; // populated when kind == upsert
};

struct FolderChanges {
    bool                     any{};
    bool                     overflow{}; // too many events / buffer overflow: caller should run a full quiet reload
    std::vector<FolderDelta> deltas;
};

class FolderWatcher {
public:
    explicit FolderWatcher(std::function<void()> wake) : wake_{std::move(wake)} {}
    ~FolderWatcher() { stop(); }
    FolderWatcher(const FolderWatcher&)            = delete;
    FolderWatcher& operator=(const FolderWatcher&) = delete;

#ifdef _WIN32

    void set_waker(std::function<void()> wake)
    {
        wake_ = std::move(wake);
        if (state_ != nullptr) {
            std::lock_guard g{state_->lock};
            state_->wake = wake_;
        }
    }

    void watch(const std::string& utf8_path)
    {
        stop();
        if (utf8_path.empty()) { return; }

        auto state  = std::make_shared<State>();
        state->wake = wake_;
        state->dir  = path::long_wide(utf8_path);
        if (!state->dir.empty() && state->dir.back() != L'\\') { state->dir.push_back(L'\\'); }
        state_ = state;

        struct OpenCtx {
            std::shared_ptr<State> state;
            std::wstring           dir_open;
        };
        std::wstring open_path = path::long_wide(utf8_path);
        auto* ctx = new OpenCtx{std::move(state), std::move(open_path)};
        if (!::TrySubmitThreadpoolCallback(
                [](PTP_CALLBACK_INSTANCE, PVOID raw) {
                    std::unique_ptr<OpenCtx> c{static_cast<OpenCtx*>(raw)};
                    HANDLE h = ::CreateFileW(c->dir_open.c_str(), FILE_LIST_DIRECTORY,
                                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                             FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
                    if (h == INVALID_HANDLE_VALUE) { return; }

                    HANDLE ev = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
                    if (ev == nullptr) {
                        ::CloseHandle(h);
                        return;
                    }

                    State& s = *c->state;
                    std::lock_guard g{s.lock};
                    if (s.stopped) {
                        ::CloseHandle(ev);
                        ::CloseHandle(h);
                        return;
                    }
                    PTP_WAIT w = ::CreateThreadpoolWait(&FolderWatcher::fired, &s, nullptr);
                    if (w == nullptr) {
                        ::CloseHandle(ev);
                        ::CloseHandle(h);
                        return;
                    }
                    s.handle  = h;
                    s.event   = ev;
                    s.tp_wait = w;
                    s.buf.resize(32768);
                    s.self = c->state;
                    if (!arm_locked(s)) {
                        s.self.reset();
                    }
                },
                ctx, nullptr)) {
            delete ctx;
            state_.reset();
        }
    }

    // drains pending incremental deltas (or overflow flag)
    [[nodiscard]] FolderChanges take_changes()
    {
        FolderChanges out;
        if (state_ == nullptr) { return out; }
        std::lock_guard g{state_->lock};
        if (!state_->overflow && state_->deltas.empty()) { return out; }
        out.any      = true;
        out.overflow = state_->overflow;
        out.deltas   = std::move(state_->deltas);
        state_->overflow = false;
        state_->deltas.clear();
        return out;
    }

    // legacy convenience: true once per change or burst
    [[nodiscard]] bool take_changed() { return take_changes().any; }

private:
    static constexpr DWORD k_filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                      FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE |
                                      FILE_NOTIFY_CHANGE_ATTRIBUTES;
    static constexpr std::size_t k_max_deltas = 32;

    struct State {
        std::mutex               lock;
        std::function<void()>    wake;
        std::wstring             dir; // ends with '\'
        bool                     stopped{false};
        bool                     overflow{false};
        std::vector<FolderDelta> deltas;
        HANDLE                   handle{};
        HANDLE                   event{};
        PTP_WAIT                 tp_wait{};
        OVERLAPPED               ov{};
        std::vector<std::uint8_t> buf;
        std::shared_ptr<State>   self;

        ~State()
        {
            if (tp_wait != nullptr) { ::CloseThreadpoolWait(tp_wait); }
            if (event != nullptr)   { ::CloseHandle(event); }
            if (handle != nullptr)  { ::CloseHandle(handle); }
        }
    };

    static bool arm_locked(State& s) noexcept
    {
        ::ResetEvent(s.event);
        s.ov        = {};
        s.ov.hEvent = s.event;
        const BOOL ok = ::ReadDirectoryChangesW(s.handle, s.buf.data(), static_cast<DWORD>(s.buf.size()),
                                                FALSE, k_filter, nullptr, &s.ov, nullptr);
        if (!ok && ::GetLastError() != ERROR_IO_PENDING) { return false; }
        ::SetThreadpoolWait(s.tp_wait, s.event, nullptr);
        return true;
    }

    static void CALLBACK fired(PTP_CALLBACK_INSTANCE, PVOID ctx, PTP_WAIT, TP_WAIT_RESULT) noexcept
    {
        auto* s = static_cast<State*>(ctx);
        std::shared_ptr<State> keep;
        std::function<void()>  wake;
        std::wstring           dir;
        std::vector<std::pair<DWORD, std::wstring>> raw_events;
        bool overflow = false;

        {
            std::lock_guard g{s->lock};
            keep = s->self;
            if (s->stopped || s->handle == nullptr) {
                s->self.reset();
                return;
            }
            DWORD bytes = 0;
            const BOOL ok = ::GetOverlappedResult(s->handle, &s->ov, &bytes, FALSE);
            if (!ok) {
                // directory deleted, volume unmounted, or transient SMB error: signal overflow once so the ui refreshes
                s->overflow = true;
                s->deltas.clear();
                wake = s->wake;
                s->self.reset();
                if (wake) { wake(); }
                return;
            }
            if (bytes == 0) {
                overflow = true;
            } else {
                const std::uint8_t* ptr = s->buf.data();
                const std::uint8_t* end = ptr + bytes;
                while (ptr + sizeof(FILE_NOTIFY_INFORMATION) <= end) {
                    const auto* fni = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(ptr);
                    const std::size_t chars = fni->FileNameLength / sizeof(wchar_t);
                    if (ptr + offsetof(FILE_NOTIFY_INFORMATION, FileName) + fni->FileNameLength <= end) {
                        raw_events.emplace_back(fni->Action, std::wstring{fni->FileName, chars});
                        if (raw_events.size() > k_max_deltas) {
                            overflow = true;
                            raw_events.clear();
                            break;
                        }
                    }
                    if (fni->NextEntryOffset == 0) { break; }
                    ptr += fni->NextEntryOffset;
                }
            }
            dir  = s->dir;
            wake = s->wake;
        }

        // stat added / modified / renamed_new files OFF the ui thread before re-arming so callbacks stay strictly ordered
        std::vector<FolderDelta> ready;
        if (!overflow) {
            ready.reserve(raw_events.size());
            for (const auto& [action, wname] : raw_events) {
                if (wname.empty() || wname.find(L'\\') != std::wstring::npos) { continue; }
                const std::string name = text::narrow(wname);
                if (action == FILE_ACTION_REMOVED || action == FILE_ACTION_RENAMED_OLD_NAME) {
                    ready.push_back({FolderDelta::Kind::remove, name, {}});
                } else {
                    WIN32_FIND_DATAW fd{};
                    HANDLE fh = ::FindFirstFileExW((dir + wname).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
                    if (fh != INVALID_HANDLE_VALUE) {
                        ::FindClose(fh);
                        FolderDelta d;
                        d.kind  = FolderDelta::Kind::upsert;
                        d.name  = name;
                        d.entry = entry_from_find_data(&fd);
                        ready.push_back(std::move(d));
                    } else {
                        // vanished between notification and stat: remove it
                        ready.push_back({FolderDelta::Kind::remove, name, {}});
                    }
                }
            }
        }

        {
            std::lock_guard g{s->lock};
            if (s->stopped) { return; }
            if (overflow || s->overflow || s->deltas.size() + ready.size() > k_max_deltas) {
                s->overflow = true;
                s->deltas.clear();
            } else {
                for (FolderDelta& d : ready) { s->deltas.push_back(std::move(d)); }
            }
            if (s->handle != nullptr && !arm_locked(*s)) {
                s->overflow = true;
                s->self.reset();
            }
        }
        if (wake) { wake(); }
    }

    void stop() noexcept
    {
        if (state_ == nullptr) { return; }
        std::shared_ptr<State> s = std::move(state_);
        PTP_WAIT               w{};
        HANDLE                 h{};
        HANDLE                 ev{};
        std::shared_ptr<State> self;
        {
            std::lock_guard g{s->lock};
            s->stopped = true;
            w          = std::exchange(s->tp_wait, nullptr);
            h          = std::exchange(s->handle, nullptr);
            ev         = std::exchange(s->event, nullptr);
            self       = std::move(s->self);
            if (h != nullptr) { ::CancelIoEx(h, &s->ov); }
        }
        if (w != nullptr) {
            ::SetThreadpoolWait(w, nullptr, nullptr);
            ::WaitForThreadpoolWaitCallbacks(w, TRUE);
            ::CloseThreadpoolWait(w);
        }
        if (ev != nullptr) { ::CloseHandle(ev); }
        if (h != nullptr)  { ::CloseHandle(h); }
    }

    std::function<void()>  wake_;
    std::shared_ptr<State> state_;
#else
    void set_waker(std::function<void()> wake)
    {
        std::lock_guard g{lock_};
        wake_ = std::move(wake);
    }

    void watch(const std::string& utf8_path)
    {
        stop();
        if (utf8_path.empty()) { return; }
        stop_flag_.store(false, std::memory_order_relaxed);
        thread_ = std::thread{[this, dir = utf8_path] {
            const int fd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
            if (fd < 0) { return; }
            const int wd = ::inotify_add_watch(fd, dir.c_str(), IN_CREATE | IN_DELETE | IN_MODIFY | IN_MOVED_FROM | IN_MOVED_TO | IN_ATTRIB | IN_DELETE_SELF);
            if (wd < 0) { ::close(fd); return; }
            alignas(inotify_event) char buf[4096];
            while (!stop_flag_.load(std::memory_order_relaxed)) {
                pollfd pfd{fd, POLLIN, 0};
                if (::poll(&pfd, 1, 200) <= 0) { continue; }
                const ssize_t n = ::read(fd, buf, sizeof buf);
                if (n <= 0) { continue; }
                std::function<void()> wake;
                {
                    std::lock_guard g{lock_};
                    overflow_ = true;
                    wake = wake_;
                }
                if (wake) { wake(); }
            }
            ::inotify_rm_watch(fd, wd);
            ::close(fd);
        }};
    }

    [[nodiscard]] FolderChanges take_changes()
    {
        FolderChanges out;
        std::lock_guard g{lock_};
        if (!overflow_) { return out; }
        out.any = true;
        out.overflow = true;
        overflow_ = false;
        return out;
    }

    [[nodiscard]] bool take_changed() { return take_changes().any; }

private:
    void stop() noexcept
    {
        stop_flag_.store(true, std::memory_order_relaxed);
        if (thread_.joinable()) { thread_.join(); }
        std::lock_guard g{lock_};
        overflow_ = false;
    }

    std::mutex            lock_;
    std::function<void()> wake_;
    std::atomic<bool>     stop_flag_{false};
    bool                  overflow_{false};
    std::thread           thread_;
#endif
};

} // namespace prism::core
