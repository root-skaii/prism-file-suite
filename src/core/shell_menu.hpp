#pragma once

// the entries Explorer's own context menu has beyond ours (Open with Code, Git Bash Here, 7-Zip, New ...), read from the
// shell's IContextMenu on a worker thread. shell extensions load their dlls and can take seconds: never on the ui thread

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace prism::core {

struct ShellIcon {
    std::string                key;         // same pixels, same key (they share one texture)
    int                        w{}, h{};
    std::vector<unsigned char> bgra;        // straight alpha
};

struct ShellMenuItem {
    unsigned                   id{};        // what invoke() takes
    std::shared_ptr<const ShellIcon> icon;  // what Explorer draws left of the label, when the entry has one
    std::string                label;       // utf-8, mnemonic markers removed
    bool                       enabled{true};
    bool                       separator{false};
    std::vector<ShellMenuItem> children;    // a submenu when not empty
};

class ShellMenu {
public:
    // `wake` runs on the worker when a result is ready (wake the ui there)
    explicit ShellMenu(std::function<void()> wake = {});
    ~ShellMenu();
    ShellMenu(const ShellMenu&)            = delete;
    ShellMenu& operator=(const ShellMenu&) = delete;

    // asks for the menu of these paths (one folder), or of the folder's own background (`paths` = {folder}). asking for
    // what was asked last does nothing, so it is cheap to call every frame while a menu is open
    void query(const std::vector<std::string>& paths, bool background);
    // the entries for the last query once the worker has them, else empty
    [[nodiscard]] std::vector<ShellMenuItem> items() const;
    [[nodiscard]] bool ready() const;   // the last query has finished (with or without entries)
    // runs an entry of the last query; `hwnd` owns whatever dialog it shows
    void invoke(unsigned id, void* hwnd);

private:
    struct Request {
        std::string              key;
        std::vector<std::string> paths;
        bool                     background{};
    };
    void run();

    std::function<void()> wake_;
    void*                 signal_{}; // event the worker sleeps on (it also pumps messages: out-of-process handlers call back)
    mutable std::mutex    mutex_;
    Request               pending_;
    bool                  has_pending_{};
    unsigned              invoke_id_{};
    void*                 invoke_hwnd_{};
    std::string           invoke_key_;  // guards invoke() against firing on a newly built menu
    bool                  has_invoke_{};
    std::string           asked_key_;   // the last query()
    std::string           done_key_;    // the query items_ belong to
    std::vector<ShellMenuItem> items_;
    std::atomic<bool>     stop_{};
    std::thread           thread_;
};

} // namespace prism::core
