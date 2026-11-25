#pragma once

// the process: one message loop for every window, the settings and keybinds they share, the tray icon and the hand-off
// of launches from other processes (platform/instance). a window that faults is dropped on its own (core/crash)

#include "app/application.hpp"
#include "core/file_index.hpp"
#include "core/folder_sizes.hpp"
#include "core/git_status.hpp"
#include "platform/instance.hpp"
#include "strata/keybinds.hpp"
#include "ui/settings.hpp"

#include <memory>
#include <mutex>
#include <vector>

namespace prism::app {

// prismfs.exe [folder [second folder]] [flags]
[[nodiscard]] Application::Options parse_command_line(const wchar_t* command_line);

class Host {
public:
    Host();
    ~Host();

    // opens the first window and runs until the last one is closed (or Exit in the tray menu). `serve`: this is the one
    // instance, so launches from other processes come here. returns the exit code
    int run(const Application::Options& first, bool serve);

private:
    struct WakeHub {
        std::mutex         lock;
        std::vector<void*> hwnds;
        void add(void* h);
        void remove(void* h);
        void wake_all();
    };

    struct PendingWindow {
        Application::Options        opts;
        std::unique_ptr<model::Tab> initial_tab;
    };

    void open_window(const Application::Options& o) { pending_.push_back({o, {}}); }
    bool transfer_tab(std::unique_ptr<model::Tab>& tab, void* target_hwnd, int x, int y, int w, int h);
    bool make_window(const Application::Options& o, std::unique_ptr<model::Tab> initial_tab = {});
    void close_window(std::size_t index);
    void apply_background(); // the tray icon and keeping the process alive follow the settings
    [[nodiscard]] bool keep_alive() const { return serving_ && settings_.background; }

    ui::Settings                              settings_;
    strata::keybinds                          binds_;
    std::shared_ptr<WakeHub>                  wake_hub_;
    std::unique_ptr<core::FolderSizes>        sizes_;
    std::unique_ptr<core::GitStatus>          git_;
    std::unique_ptr<core::FileIndex>          index_;
    bool                                      index_started_{};
    std::shared_ptr<platform::Gpu::SharedDevice> shared_gpu_;
    std::vector<std::unique_ptr<Application>> windows_;
    std::vector<PendingWindow>                pending_;
    platform::instance::Server                server_;
    bool                                      serving_{};
    bool                                      exit_requested_{};
    bool                                      tray_shown_{};
    unsigned                                  tray_language_{};
};

} // namespace prism::app
