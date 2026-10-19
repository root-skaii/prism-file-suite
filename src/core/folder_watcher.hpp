#pragma once

// tells when something inside one folder changed (another program added, removed or renamed a file). Uses the thread pool,
// so there is no thread of our own: the callback only sets a flag and wakes the ui. Folders that cannot be watched
// (This PC, some virtual or network paths) simply never report.

#include "core/text.hpp"

#include <atomic>
#include <functional>
#include <string>

#include <windows.h>

namespace prism::core {

class FolderWatcher {
public:
    explicit FolderWatcher(std::function<void()> wake) : state_{new State{std::move(wake)}} {}
    ~FolderWatcher() { stop(); delete state_; }
    FolderWatcher(const FolderWatcher&)            = delete;
    FolderWatcher& operator=(const FolderWatcher&) = delete;

    void watch(const std::string& utf8_path)
    {
        stop();
        state_->changed.store(false);
        if (utf8_path.empty()) { return; }
        constexpr DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE;
        state_->handle = ::FindFirstChangeNotificationW(text::widen(utf8_path).c_str(), FALSE, filter);
        if (state_->handle == INVALID_HANDLE_VALUE) { state_->handle = nullptr; return; }
        if (::RegisterWaitForSingleObject(&wait_, state_->handle, &FolderWatcher::fired, state_, INFINITE, WT_EXECUTEDEFAULT) == FALSE) {
            ::FindCloseChangeNotification(state_->handle);
            state_->handle = nullptr;
            wait_ = nullptr;
        }
    }

    // true once per change (or burst of them)
    [[nodiscard]] bool take_changed() noexcept { return state_->changed.exchange(false); }

private:
    struct State {
        std::function<void()> wake;
        std::atomic<bool>     changed{false};
        HANDLE                handle{};
    };

    static void CALLBACK fired(void* ctx, BOOLEAN) noexcept
    {
        auto* s = static_cast<State*>(ctx);
        s->changed.store(true);
        ::FindNextChangeNotification(s->handle); // (re-arm: the wait fires again on the next change)
        if (s->wake) { s->wake(); }
    }

    void stop() noexcept
    {
        if (wait_ != nullptr) { ::UnregisterWaitEx(wait_, INVALID_HANDLE_VALUE); wait_ = nullptr; } // (blocks until a running callback returns)
        if (state_->handle != nullptr) { ::FindCloseChangeNotification(state_->handle); state_->handle = nullptr; }
    }

    State* state_;
    HANDLE wait_{};
};

} // namespace prism::core
