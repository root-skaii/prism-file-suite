#pragma once

// the whole window content: title bar with tabs, toolbar, sidebar, one or two file views, preview pane, status bar.
// built every frame. also the app's command handler, keyboard shortcuts and the owner of the settings state the ui follows.

#include "core/archive.hpp"
#include "core/file_index.hpp"
#include "core/image_tools.hpp"
#include "model/folder_sorts.hpp"
#include "model/sessions.hpp"
#include "ui/batch_rename_dialog.hpp"
#include "ui/compare_dialog.hpp"
#include "ui/duplicates_dialog.hpp"
#include "ui/empty_folders_dialog.hpp"
#include "ui/env.hpp"
#include "ui/file_list.hpp"
#include "ui/preview_pane.hpp"
#include "ui/quick_look.hpp"
#include "ui/properties_dialog.hpp"
#include "ui/search_dialog.hpp"
#include "ui/sessions_dialog.hpp"
#include "ui/settings_dialog.hpp"
#include "ui/sidebar.hpp"
#include "ui/status_bar.hpp"
#include "ui/title_bar.hpp"
#include "ui/toolbar.hpp"
#include "ui/transfers_ui.hpp"
#include "ui/usage_dialog.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace prism::ui {

class Shell final : private Commands {
public:
    Shell(platform::Window& window, Theme& theme, FontSet fonts, model::TabSet& tabs, core::FileOps& ops, Settings& settings,
          platform::ImageCache& images, strata::keybinds& binds, model::UndoStack& history);
    ~Shell();

    void frame(strata::context& ui, const FrameInput& in);
    [[nodiscard]] platform::HitZone hit_test(strata::vec2 logical) const { return title_bar_.hit_test(logical); }

    // the theme / density / accent changed: the host re-applies strata's style
    [[nodiscard]] bool take_restyle() noexcept { return std::exchange(restyle_, false); }
    // the interface size changed (percent, 0 = no change): the host rebuilds the font atlas
    [[nodiscard]] int take_scale_request() noexcept { return std::exchange(scale_request_, 0); }
    // pointer shape the panels want over strata's (arrow = none)
    [[nodiscard]] strata::cursor_kind cursor() const noexcept { return cursor_; }
    // a drag the ui wants started (taken after the frame)
    [[nodiscard]] std::vector<std::string> take_drag() noexcept { return std::exchange(drag_paths_, {}); }

    // drag and drop onto the window
    [[nodiscard]] std::string resolve_drop(strata::vec2 logical) const;
    void set_drop_point(std::optional<strata::vec2> p);
    // the paths of the drag in progress: drawn as a ghost at the pointer (with what a drop would do) while it is over the window
    void set_dragging(std::vector<std::string> paths) { ghost_paths_ = std::move(paths); }
    void refresh_all();
    void drop_files(std::vector<std::string> paths, const std::string& dest, bool move);

    // writes tabs and window size for the next start
    void save_session(int logical_w, int logical_h);
    void show_palette() { palette_.open(); } // (--palette, for screenshots)
    void show_settings() { settings_dialog_.open(); }
    // the folder the second pane opens with when it appears (command line)
    void set_second_folder(std::string folder) { pending_right_ = std::move(folder); }
    void show_properties(std::vector<std::string> paths, int tab = 0) { properties_dialog_.open(std::move(paths), tab); }
    void show_usage(std::string folder) { usage_dialog_.open(std::move(folder)); }

    // asks the host for another window: folder, whether `x, y` (screen pixels) place it, and its logical size
    std::function<void(const std::string& folder, bool has_at, int x, int y, int w, int h)> open_window_hook;

private:
    // Commands
    void open_tab(const std::string& path, bool activate) override;
    void open_window(const std::string& folder) override { spawn_window(folder); }
    void close_tab(std::size_t index) override;
    void move_tab_out(std::size_t index, int screen_x, int screen_y) override;
    void restart_elevated() override;
    void restart_unelevated() override;
    void spawn_window(const std::string& folder, std::optional<std::pair<int, int>> pointer = {});
    void toggle_theme() override;
    void toggle_density() override;
    void toggle_hidden() override;
    void focus_address() override;
    void focus_filter() override;
    void open_settings() override;
    void settings_changed() override { settings_dirty_ = true; }
    void open_properties(std::vector<std::string> paths) override;
    void toggle_view() override;
    void toggle_preview() override;
    void toggle_split() override;
    void set_sort(const std::string& folder, model::SortKey key, bool ascending) override;
    void begin_drag(std::vector<std::string> paths) override { drag_paths_ = std::move(paths); }
    void transfer(std::vector<std::string> paths, const std::string& dest, bool move) override;
    void request_delete(std::vector<std::string> paths, bool permanent) override;
    void restore_from_bin(std::vector<std::string> items) override;
    void empty_recycle_bin(std::string drive_root) override;
    void request_remove_section(std::size_t section) override;
    void toggle_pin(const std::string& path) override;
    void copy_sidebar() override;
    void paste_sidebar() override;
    bool edit_sidebar(std::string_view label, const std::function<void(model::SidebarLayout&)>& edit) override;
    void request_action(std::string name) override { pending_actions_.push_back(std::move(name)); }
    void toggle_tag(const std::vector<std::string>& paths, int index) override;
    void clear_tags(const std::vector<std::string>& paths) override;
    void open_archive(const std::string& path) override;
    void go_up(model::Tab& tab) override;
    void toast(std::string_view text, strata::toast_kind kind) override;
    void set_cursor(strata::cursor_kind kind) override { cursor_ = kind; }

    void shortcuts(strata::context& ui);
    void run_action(const std::string& name, strata::context& ui);
    void record_undo(const core::OpResult& r);
    void do_undo();
    void do_redo();
    void open_goto();
    void open_batch_rename();
    void sync_settings();
    void service_ops();
    void service_archives();
    void service_images();
    [[nodiscard]] model::SortSpec sort_for(const std::string& folder) const;
    bool apply_sorts(); // true: a pane's order changed
    void archive_action(const std::string& name);
    void service_delete(strata::context& ui);
    void service_remove_section(strata::context& ui);
    [[nodiscard]] bool dialog_has_keyboard(strata::context& ui) const;
    void splitter(Painter& p, const strata::rect& area);
    void apply_deferred();
    void draw_drag_ghost(Painter& p);
    void apply_tags();
    void rebuild_index();
    void set_tag_masks(const std::vector<std::pair<std::string, std::uint16_t>>& masks);
    [[nodiscard]] model::Session capture_session(std::string name) const;
    void restore_session(const model::Session& s) { pending_session_ = s; }

    core::FolderSizes    sizes_;  // (before env_)
    core::GitStatus      git_;
    core::ArchiveOps     archives_;
    core::ArchiveViews   views_;   // (before env_)
    core::FileIndex      index_;
    bool                 index_started_{};
    core::ImageJobs      image_jobs_;
    model::SidebarLayout layout_; // (before env_, which refers to it)
    std::size_t          saved_layout_{};
    model::FolderSorts   folder_sorts_;
    std::size_t          saved_sorts_{};
    model::Sessions      sessions_;
    std::size_t          saved_sessions_{};
    model::Tags          tags_;  // (before env_, which refers to it)
    std::size_t          saved_tags_{};
    std::optional<model::Session> pending_session_; // a session to switch to at the end of the frame
    std::string          pending_right_;            // the second pane's folder once the pane exists
    model::VisitLog      visits_;
    std::size_t          saved_visits_{};
    const model::Tab*    visit_tab_{};      // the tab and folder last seen: a change inside the same tab is a visit
    std::string          visit_path_;
    Env            env_;
    core::FileOps& ops_;
    Settings&      settings_;
    TitleBar       title_bar_;
    Toolbar        toolbar_;
    Sidebar        sidebar_;
    FileList       file_list_;
    FileList       file_list_r_;
    PreviewPane    preview_;
    QuickLook      quick_look_;
    StatusBar      status_bar_;
    SettingsDialog settings_dialog_;
    PropertiesDialog properties_dialog_;
    BatchRenameDialog batch_rename_dialog_;
    DuplicatesDialog  duplicates_dialog_;
    SessionsDialog    sessions_dialog_;
    UsageDialog       usage_dialog_;
    CompareDialog     compare_dialog_;
    TransfersUi       transfers_ui_;
    SearchDialog      search_dialog_;
    EmptyFoldersDialog empty_folders_dialog_;

    strata::command_palette palette_;
    strata::command_palette goto_palette_;    // "Jump to a folder": the pinned folders and drives, by name
    strata::keybinds        goto_binds_;      // (its entries; rebuilt each time it opens)
    std::vector<std::pair<std::string, std::string>> goto_targets_; // entry name -> folder
    std::vector<std::string> closed_tabs_;    // newest last: ctrl+shift+T reopens
    std::unique_ptr<model::Tab> right_tab_;   // the second pane while split
    float             sidebar_w_{0.0f};
    bool              split_drag_{};
    float             split_grab_{};
    bool              show_hidden_{false};
    bool              restyle_{false};
    int               scale_request_{0};
    int               applied_scale_{100};
    bool              settings_dirty_{false};
    strata::cursor_kind cursor_{strata::cursor_kind::arrow};
    std::vector<std::string> drag_paths_;
    std::vector<std::string> ghost_paths_;
    std::optional<strata::vec2> ghost_at_;
    // a delete waiting for its confirmation. strata modals must open outside every window, so the panels only
    // record the request and service_delete() shows the dialog at the end of the frame
    std::vector<std::string> delete_paths_;
    bool                     delete_permanent_{};
    std::shared_ptr<std::atomic<int>> bin_empty_{std::make_shared<std::atomic<int>>(0)}; // 0 idle, 3 running, 1 / 2 finished (ok / failed): the worker's answer
    bool                     delete_ask_{};
    // a sidebar section removal waiting for its answer (same reason); npos: none
    std::size_t              remove_section_{std::string::npos};
    bool                     remove_ask_{};

    // tab changes asked for mid-frame run at its end, when no panel still holds a reference into the tab list
    struct Open { std::string path; bool activate; };
    std::vector<Open>        pending_open_;
    std::vector<std::size_t> pending_close_;
    std::vector<std::string> pending_actions_;
    struct Toast { std::string text; strata::toast_kind kind; };
    std::vector<Toast>       pending_toasts_;
};

} // namespace prism::ui
