#include "ui/shell.hpp"
#include "ui/tr.hpp"

#include "core/links.hpp"
#include "ui/tr.hpp"
#include "core/path.hpp"
#include "ui/tr.hpp"
#include "core/places.hpp"
#include "ui/tr.hpp"
#include "core/recycle.hpp"
#include "core/shell_ops.hpp"
#include "ui/tr.hpp"
#include "core/text.hpp"
#include "ui/tr.hpp"
#include "ui/panel.hpp"
#include "ui/tr.hpp"
#include "strata/config.hpp"
#include "ui/tr.hpp"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <format>

namespace prism::ui {

using namespace strata;

Shell::Shell(platform::Window& window, Theme& theme, FontSet fonts, model::TabSet& tabs, core::FileOps& ops, Settings& settings,
             platform::ImageCache& images, strata::keybinds& binds, model::UndoStack& history)
    : sizes_{window.waker()}, git_{window.waker()}, archives_{window.waker()}, index_{window.waker()}, image_jobs_{window.waker()}, layout_{model::SidebarLayout::defaults()},
      env_{window, theme, fonts, tabs, *this, ops, settings, images, binds, history, layout_, visits_, sizes_, git_, tags_, views_},
      ops_{ops}, settings_{settings},
      title_bar_{env_}, toolbar_{env_}, sidebar_{env_}, file_list_{env_, false}, file_list_r_{env_, true}, preview_{env_}, quick_look_{env_},
      status_bar_{env_}, settings_dialog_{env_}, properties_dialog_{env_}, batch_rename_dialog_{env_}, duplicates_dialog_{env_},
      sessions_dialog_{env_, sessions_, [this](std::string name) { return capture_session(std::move(name)); }, [this](const model::Session& s) { restore_session(s); }},
      usage_dialog_{env_}, compare_dialog_{env_}, transfers_ui_{env_},
      search_dialog_{env_, index_, [this] { rebuild_index(); }}, empty_folders_dialog_{env_}
{
    sidebar_w_     = settings.sidebar_width > 0.0f ? settings.sidebar_width : theme.metrics().sidebar_width;
    applied_scale_ = settings.ui_scale;
    goto_palette_.shortcut = {}; // (opened by its action, not its own chord)
    (void)layout_.load_file(Settings::sidebar_path()); // (nothing stored yet keeps the defaults)
    saved_layout_ = layout_.revision();
    if (settings.track_history) { (void)visits_.load_file(Settings::visits_path()); }
    else { ::DeleteFileW(core::text::widen(Settings::visits_path()).c_str()); }
    saved_visits_ = visits_.revision();
    (void)folder_sorts_.load_file(Settings::folder_sorts_path());
    saved_sorts_ = folder_sorts_.revision();
    (void)sessions_.load_file(Settings::sessions_path());
    saved_sessions_ = sessions_.revision();
    (void)tags_.load_file(Settings::tags_path());
    saved_tags_ = tags_.revision();
    core::clear_archive_cache(); // (the unpacked copies of archives browsed in an earlier session)
    core::FileOps::on_renamed = [this](const std::string& from, const std::string& to) { tags_.rename_prefix(from, to); }; // (labels follow a rename)
}

Shell::~Shell() { core::FileOps::on_renamed = nullptr; }

// the local fixed drives: what the instant search covers (network and removable drives are slow and come and go)
void Shell::rebuild_index()
{
    std::vector<std::string> roots;
    const DWORD mask = ::GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if ((mask & (1u << i)) == 0) { continue; }
        const std::wstring root = std::wstring{static_cast<wchar_t>(L'A' + i)} + L":\\";
        if (::GetDriveTypeW(root.c_str()) == DRIVE_FIXED) { roots.push_back(core::text::narrow(root)); }
    }
    index_.start(std::move(roots));
}

void Shell::set_tag_masks(const std::vector<std::pair<std::string, std::uint16_t>>& masks)
{
    for (const auto& [path, mask] : masks) { (void)tags_.set(path, mask); }
}

void Shell::toggle_tag(const std::vector<std::string>& paths, int index)
{
    if (paths.empty() || index < 0 || index >= core::tag::count) { return; }
    const auto bit = static_cast<std::uint16_t>(1u << index);
    const bool all = std::ranges::all_of(paths, [&](const std::string& p) { return (tags_.get(p) & bit) != 0; });
    std::vector<std::pair<std::string, std::uint16_t>> before, after;
    for (const std::string& p : paths) {
        const std::uint16_t m = tags_.get(p);
        before.emplace_back(p, m);
        after.emplace_back(p, static_cast<std::uint16_t>(all ? (m & ~bit) : (m | bit)));
    }
    set_tag_masks(after);
    env_.history.push({trf("{} label", tag_label(index)),
                       [this, before] { set_tag_masks(before); return true; },
                       [this, after] { set_tag_masks(after); return true; }});
}

void Shell::clear_tags(const std::vector<std::string>& paths)
{
    std::vector<std::pair<std::string, std::uint16_t>> before, after;
    for (const std::string& p : paths) {
        const std::uint16_t m = tags_.get(p);
        if (m == 0) { continue; }
        before.emplace_back(p, m);
        after.emplace_back(p, std::uint16_t{0});
    }
    if (before.empty()) { return; }
    set_tag_masks(after);
    env_.history.push({std::string{tr("Clear labels")},
                       [this, before] { set_tag_masks(before); return true; },
                       [this, after] { set_tag_masks(after); return true; }});
}

// copies the labels into the entries of the panes on screen (and refreshes a `tag:` filter); cheap when nothing changed
void Shell::apply_tags()
{
    const auto sync = [&](model::Tab& t) {
        model::Listing& L = t.listing();
        const std::string folder = t.path();
        std::uint64_t token = tags_.revision();
        const auto mix = [&](std::uint64_t v) { token = (token ^ v) * 0x100000001b3ull + 0x9e3779b97f4a7c15ull; };
        mix(std::hash<std::string>{}(folder));
        mix(L.total());
        mix(L.finished() ? 1u : 0u);
        mix(L.epoch());
        if (tags_.any()) {
            L.sync_tags(token, [&](const core::FileEntry& e) { return tags_.get(core::path::join(folder, e.name)); });
        } else {
            L.sync_tags(token, {});
        }
    };
    sync(env_.tabs.active());
    if (right_tab_) { sync(*right_tab_); }
}

model::Session Shell::capture_session(std::string name) const
{
    model::Session s;
    s.name = std::move(name);
    for (std::size_t i = 0; i < env_.tabs.size(); ++i) {
        const std::string& path = env_.tabs.at(i).path();
        if (path.empty()) { continue; }
        if (i == env_.tabs.active_index()) { s.active = static_cast<int>(s.tabs.size()); }
        s.tabs.push_back(path);
    }
    if (right_tab_) { s.right = right_tab_->path(); }
    return s;
}

// --- commands ---------------------------------------------------------------------------------------------------

void Shell::open_tab(const std::string& path, bool activate) { pending_open_.push_back({path, activate}); }
void Shell::close_tab(std::size_t index) { pending_close_.push_back(index); }

// the new window is this one's size, staggered down-right of it (or, for a torn-off tab, under the pointer)
void Shell::spawn_window(const std::string& folder, std::optional<std::pair<int, int>> pointer)
{
    const platform::Window::Placement at = pointer ? env_.window.placement_at(pointer->first, pointer->second) : env_.window.cascaded_placement();
    const auto [lw, lh] = env_.window.logical_size();
    wchar_t exe[MAX_PATH]{};
    ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring{exe} + L"\" \"" + core::text::widen(folder) + L"\" --at " + std::to_wstring(at.x) + L"," + std::to_wstring(at.y) +
                       L" --size " + std::to_wstring(lw) + L"," + std::to_wstring(lh);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
    }
}

// a tab dropped outside this window: into the PrismFS window under the pointer, else into a window of its own
void Shell::move_tab_out(std::size_t index, int screen_x, int screen_y)
{
    if (index >= env_.tabs.size()) { return; }
    const std::string path = env_.tabs.at(index).path();
    if (path.empty()) { return; }
    if (!env_.window.send_folder_to_window_at(screen_x, screen_y, path)) {
        if (env_.tabs.size() < 2) { return; } // (the only tab: nothing to tear off)
        spawn_window(path, std::pair{screen_x, screen_y});
    }
    close_tab(index);
}
void Shell::restart_elevated()
{
    std::vector<std::string> folders{env_.tab().path()};
    if (settings_.split && right_tab_) { folders.push_back(right_tab_->path()); }
    if (core::shell::relaunch_elevated(folders)) { env_.window.request_close(); }
}

void Shell::restart_unelevated()
{
    std::vector<std::string> folders{env_.tab().path()};
    if (settings_.split && right_tab_) { folders.push_back(right_tab_->path()); }
    if (core::shell::relaunch_unelevated(folders)) { env_.window.request_close(); }
    else { toast(tr("Could not start a normal window"), toast_kind::warning); }
}

void Shell::toggle_theme() { settings_.dark = !settings_.dark; settings_dirty_ = true; }
void Shell::toggle_density() { settings_.compact = !settings_.compact; settings_dirty_ = true; }
void Shell::toggle_hidden() { settings_.show_hidden = !settings_.show_hidden; settings_dirty_ = true; }
void Shell::toggle_view() { settings_.grid_view = !settings_.grid_view; settings_dirty_ = true; }
void Shell::toggle_preview() { settings_.preview = !settings_.preview; settings_dirty_ = true; }
void Shell::toggle_split() { settings_.split = !settings_.split; settings_dirty_ = true; }

// the order a folder shows: the options of the order itself are global; the column and direction are the default, or what
// was last chosen for that folder when the order is remembered per folder
model::SortSpec Shell::sort_for(const std::string& folder) const
{
    model::SortSpec s;
    s.key         = static_cast<model::SortKey>(std::clamp(settings_.sort_key, 0, 3));
    s.ascending   = settings_.sort_ascending;
    s.folders     = static_cast<model::FolderPlacement>(std::clamp(settings_.sort_folders, 0, 2));
    s.natural     = settings_.sort_natural;
    s.hidden_last = settings_.sort_hidden_last;
    if (settings_.sort_per_folder) {
        if (const auto e = folder_sorts_.get(folder)) {
            s.key       = e->key;
            s.ascending = e->ascending;
        }
    }
    return s;
}

// (the panes on screen only: another tab picks its order up when it is shown)
bool Shell::apply_sorts()
{
    bool changed = false;
    const auto apply = [&](model::Tab& t) {
        const model::SortSpec want = sort_for(t.path());
        if (t.listing().sort() != want) { t.listing().set_sort(want); changed = true; }
        const auto group = static_cast<model::GroupBy>(std::clamp(settings_.group_by, 0, 4));
        if (t.listing().group() != group) { t.listing().set_group(group); changed = true; }
    };
    apply(env_.tabs.active());
    if (right_tab_) { apply(*right_tab_); }
    return changed;
}

void Shell::set_sort(const std::string& folder, model::SortKey key, bool ascending)
{
    if (settings_.sort_per_folder) {
        (void)folder_sorts_.set(folder, {key, ascending});
    } else {
        settings_.sort_key       = static_cast<int>(key);
        settings_.sort_ascending = ascending;
        settings_dirty_          = true;
    }
}
void Shell::focus_address() { toolbar_.begin_edit(); }
void Shell::focus_filter() { toolbar_.focus_filter(); }
void Shell::open_settings() { settings_dialog_.open(); }
void Shell::open_properties(std::vector<std::string> paths) { properties_dialog_.open(std::move(paths)); }
void Shell::toast(std::string_view text, toast_kind kind) { pending_toasts_.push_back({std::string{text}, kind}); }

// a modal (delete confirmation, Properties, Settings) owns the keyboard: panels and shortcuts behind it still see key
// presses in strata, so Delete or typing must not reach the file list while one is open. (the palette is a modal too, but
// it runs the shortcuts itself)
bool Shell::dialog_has_keyboard(context& ui) const
{
    return !delete_paths_.empty() || remove_section_ != std::string::npos || (ui.modal_open() && !palette_.is_open() && !goto_palette_.is_open());
}

void Shell::request_delete(std::vector<std::string> paths, bool permanent)
{
    if (paths.empty()) { return; }
    if (std::ranges::all_of(paths, [](const std::string& p) { return core::recycle::inside(p); })) { permanent = true; } // (nothing recycles twice)
    if (!permanent && !settings_.confirm_delete) {
        ops_.start(core::OpKind::recycle, std::move(paths), {});
        return;
    }
    delete_paths_     = std::move(paths);
    delete_permanent_ = permanent;
    delete_ask_       = true;
}

void Shell::restore_from_bin(std::vector<std::string> items)
{
    int         done = 0;
    std::string error;
    for (const std::string& item : items) {
        std::string why;
        if (core::recycle::restore(item, why)) { ++done; } else if (error.empty()) { error = why; }
    }
    if (done > 0) { toast(trf_n("Restored {} item", "Restored {} items", done, done), toast_kind::success); }
    if (!error.empty()) { toast(error, toast_kind::error); }
    refresh_all();
}

void Shell::empty_recycle_bin(std::string drive_root)
{
    if (bin_empty_->load() == 3) { return; }
    bin_empty_->store(3);
    // (the shell asks first and may take a while: a worker, which answers through the shared flag and wakes the ui)
    std::thread{[state = bin_empty_, wake = env_.window.waker(), root = std::move(drive_root)] {
        state->store(core::recycle::empty(root) ? 1 : 2);
        wake();
    }}.detach();
}

void Shell::service_delete(context& ui)
{
    if (delete_paths_.empty()) { return; }
    const char* const id = delete_permanent_ ? "pfs_delete_forever" : "pfs_delete";
    if (delete_ask_) {
        const long long n = static_cast<long long>(delete_paths_.size());
        ui.ask_confirm(id, delete_permanent_ ? trf_n("Permanently delete {} item? This can't be undone.", "Permanently delete {} items? This can't be undone.", n, n)
                                             : trf_n("Move {} item to the Recycle Bin?", "Move {} items to the Recycle Bin?", n, n));
        delete_ask_ = false;
    }
    int result = ui.confirm(id, {delete_permanent_ ? tr("Delete") : tr("Move to Recycle Bin"), tr("Cancel")});
    // strata's confirm only knows Esc and clicks; Enter takes the first button
    if (result == 0 && ui.key_pressed(key::enter)) {
        ui.close_modal();
        result = 1;
    }
    if (result == 0) { return; }
    if (result == 1) { ops_.start(delete_permanent_ ? core::OpKind::erase : core::OpKind::recycle, std::move(delete_paths_), {}); }
    delete_paths_.clear();
}

void Shell::request_remove_section(std::size_t section)
{
    if (section >= layout_.sections().size()) { return; }
    if (layout_.sections()[section].items.empty()) {
        const std::string title = layout_.sections()[section].title;
        (void)edit_sidebar(trf("Remove {}", title), [=](model::SidebarLayout& l) { (void)l.remove_section(section); });
        return;
    }
    remove_section_ = section;
    remove_ask_     = true;
}

void Shell::service_remove_section(context& ui)
{
    if (remove_section_ == std::string::npos) { return; }
    const std::size_t section = remove_section_;
    if (section >= layout_.sections().size()) {
        remove_section_ = std::string::npos;
        return;
    }
    const model::SidebarSection& sec = layout_.sections()[section];
    static constexpr const char* id = "pfs_remove_section";
    const bool keep_possible = layout_.has_neighbour(section);
    if (remove_ask_) {
        const long long n = static_cast<long long>(sec.items.size());
        ui.ask_confirm(id, trf_n("\"{}\" has {} folder pinned. ", "\"{}\" has {} folders pinned. ", n, sec.title, n) +
                               std::string{keep_possible ? tr("Keep them in another section, or remove them too?")
                                                         : tr("There is no other section to move them to. Remove them too?")});
        remove_ask_ = false;
    }
    int result = keep_possible ? ui.confirm(id, {tr("Keep folders"), tr("Remove them too"), tr("Cancel")}) : ui.confirm(id, {tr("Remove them too"), tr("Cancel")});
    if (keep_possible && result == 0 && ui.key_pressed(key::enter)) { // (Enter takes the harmless first button, never "remove")
        ui.close_modal();
        result = 1;
    }
    if (result == 0) { return; }
    const std::string title = sec.title;
    // (without a neighbour the first button already is "remove")
    if (result == 1 || (keep_possible && result == 2)) {
        const bool keep = keep_possible && result == 1;
        (void)edit_sidebar(trf("Remove {}", title), [=](model::SidebarLayout& l) { (void)l.remove_section(section, keep); });
    }
    remove_section_ = std::string::npos;
}

void Shell::transfer(std::vector<std::string> paths, const std::string& dest, bool move)
{
    if (paths.empty() || dest.empty()) { return; }
    ops_.start(move ? core::OpKind::move : core::OpKind::copy, std::move(paths), dest);
}

void Shell::drop_files(std::vector<std::string> paths, const std::string& dest, bool move)
{
    if (Sidebar::is_pin_target(dest)) {
        sidebar_.pin_dropped(paths, dest);
        return;
    }
    transfer(std::move(paths), dest, move);
}

bool Shell::edit_sidebar(std::string_view label, const std::function<void(model::SidebarLayout&)>& edit)
{
    const model::SidebarLayout before = layout_;
    edit(layout_);
    if (layout_.revision() == before.revision()) { return false; }
    env_.history.push({std::string{label},
                       [this, before] { layout_.restore(before); return true; },
                       [this, after = layout_] { layout_.restore(after); return true; }});
    return true;
}

void Shell::toggle_pin(const std::string& path)
{
    if (path.empty() || !core::path::is_directory(path)) {
        toast(tr("Only folders can be pinned to the sidebar"), toast_kind::info);
        return;
    }
    const std::string name = core::path::leaf(path);
    if (layout_.find(path)) {
        (void)edit_sidebar(trf("Unpin {}", name), [&](model::SidebarLayout& l) { (void)l.unpin(path); });
        toast(trf("Unpinned {}", name), toast_kind::info);
    } else {
        (void)edit_sidebar(trf("Pin {}", name), [&](model::SidebarLayout& l) { (void)l.pin(l.pin_section(), std::string::npos, path); });
        toast(trf("Pinned {} to the sidebar", name), toast_kind::info);
    }
}

void Shell::copy_sidebar()
{
    strata::config c;
    layout_.store(c);
    toast(core::shell::set_clipboard_text(c.to_string()) ? tr("Sidebar layout copied") : tr("Could not copy to the clipboard"), toast_kind::info);
}

void Shell::paste_sidebar()
{
    strata::config c;
    (void)c.from_string(core::shell::get_clipboard_text());
    model::SidebarLayout incoming;
    if (!incoming.load(c)) {
        toast(tr("The clipboard has no sidebar layout"), toast_kind::warning);
        return;
    }
    (void)edit_sidebar(tr("Paste sidebar layout"), [&](model::SidebarLayout& l) { l.replace(incoming); });
    toast(tr("Sidebar layout pasted"), toast_kind::info);
}

void Shell::refresh_all()
{
    git_.invalidate();
    for (std::size_t i = 0; i < env_.tabs.size(); ++i) { env_.tabs.at(i).refresh(); }
    if (right_tab_) { right_tab_->refresh(); }
}

void Shell::apply_deferred()
{
    for (const Open& o : pending_open_) { env_.tabs.open(o.path, o.activate); }
    pending_open_.clear();
    for (const std::size_t i : pending_close_) {
        if (i >= env_.tabs.size()) { continue; }
        const std::string path = env_.tabs.at(i).path();
        if (!env_.tabs.close(i)) {
            env_.window.request_close();
        } else if (!path.empty()) {
            closed_tabs_.push_back(path); // (Ctrl+Shift+T brings it back)
            if (closed_tabs_.size() > 20) { closed_tabs_.erase(closed_tabs_.begin()); }
        }
    }
    pending_close_.clear();

    if (pending_session_) {
        const model::Session s = std::move(*pending_session_);
        pending_session_.reset();
        const std::size_t old = env_.tabs.size();
        std::size_t opened = 0, active = 0;
        for (std::size_t i = 0; i < s.tabs.size(); ++i) {
            if (!core::path::is_directory(s.tabs[i])) { continue; }
            if (static_cast<int>(i) <= s.active) { active = opened; } // (a missing folder shifts the others down)
            env_.tabs.open(s.tabs[i], false);
            ++opened;
        }
        if (opened == 0) {
            toast(tr("None of the folders in this session exist any more"), toast_kind::warning);
        } else {
            for (std::size_t i = old; i-- > 0;) { (void)env_.tabs.close(i); } // (not into the reopen list: they are not "closed" tabs)
            env_.tabs.activate(active);
            if (core::path::is_directory(s.right)) {
                settings_.split = true;
                if (right_tab_) { right_tab_->navigate(s.right); } else { pending_right_ = s.right; }
            } else if (s.right.empty()) {
                settings_.split = false;
            }
            settings_dirty_ = true;
            toast(trf("Session \"{}\" opened", s.name), toast_kind::info);
        }
    }
}

// settings are the one source of truth; the theme and the panels follow them
void Shell::sync_settings()
{
    ops_.use_builtin(settings_.builtin_copy);
    Theme& t = env_.theme;
    bool restyle = false;
    if (settings_.dark_style != static_cast<int>(t.dark_style())) {
        t.set_dark_style(static_cast<DarkStyle>(settings_.dark_style));
        restyle = true;
    }
    if (settings_.dark != t.dark()) {
        t.set_mode(settings_.dark ? ThemeMode::dark : ThemeMode::light);
        restyle = true;
    }
    const color want{static_cast<u8>(settings_.accent >> 16), static_cast<u8>(settings_.accent >> 8), static_cast<u8>(settings_.accent), 255};
    if (want != t.accent_color()) {
        t.set_accent(want);
        restyle = true;
    }
    if (settings_.compact != (t.density() == Density::compact)) {
        t.set_density(settings_.compact ? Density::compact : Density::comfortable);
        if (settings_.sidebar_width <= 0.0f) { sidebar_w_ = t.metrics().sidebar_width; }
        restyle = true;
    }
    if (settings_.ui_scale != applied_scale_) {
        applied_scale_ = settings_.ui_scale;
        scale_request_ = applied_scale_;
    }
    show_hidden_ = settings_.show_hidden;
    restyle_ |= restyle;

    // the second pane exists only while split
    if (settings_.split && !right_tab_) {
        right_tab_ = std::make_unique<model::Tab>(env_.window.waker());
        right_tab_->navigate(pending_right_.empty() ? env_.tabs.active().path() : pending_right_);
        pending_right_.clear();
        env_.right = right_tab_.get();
    } else if (!settings_.split && right_tab_) {
        env_.right        = nullptr;
        env_.right_active = false;
        right_tab_.reset();
    }

    if (settings_dirty_) {
        settings_dirty_ = false;
        settings_.save();
    }
}

void Shell::service_ops()
{
    if (const int s = bin_empty_->load(); s == 1 || s == 2) {
        bin_empty_->store(0);
        if (s == 1) { toast(tr("Recycle Bin emptied"), toast_kind::success); } else { toast(tr("The Recycle Bin was not emptied"), toast_kind::info); }
        refresh_all();
    }
    core::OpResult r;
    while (ops_.poll(r)) {
        const std::string what = trf_n("{} item", "{} items", static_cast<long long>(r.count), r.count);
        if (r.aborted) {
            toast(tr("Cancelled"), toast_kind::warning);
        } else if (!r.ok) {
            toast(r.message.empty() ? tr("The operation failed") : std::string_view{r.message}, toast_kind::error);
        } else if (!r.silent) { // (an undo / redo reports itself)
            switch (r.kind) {
            case core::OpKind::copy:    toast(trf("Copied {}", what), toast_kind::success); break;
            case core::OpKind::move:    toast(trf("Moved {}", what), toast_kind::success); break;
            case core::OpKind::recycle: toast(trf("{} moved to the Recycle Bin", what), toast_kind::success); break;
            case core::OpKind::erase:   toast(trf("Deleted {}", what), toast_kind::success); break;
            }
        }
        // (a transfer that failed or was cancelled half way still left items behind, and undo can take them away again)
        if (!r.silent && (r.ok || std::ranges::any_of(r.made, [](const std::string& m) { return !m.empty(); }))) { record_undo(r); }
        if (r.ok && !r.aborted) { // (labels move with the items; undo / redo moves are results too)
            if (r.kind == core::OpKind::move) {
                const bool known = r.made.size() == r.sources.size();
                for (std::size_t i = 0; i < r.sources.size(); ++i) {
                    const std::string to = known ? r.made[i] : core::path::join(r.dest, core::path::leaf(r.sources[i]));
                    if (!to.empty()) { tags_.rename_prefix(r.sources[i], to); }
                }
            } else if (r.kind == core::OpKind::erase) {
                for (const std::string& s : r.sources) {
                    tags_.remove_prefix(s);
                    if (core::recycle::inside(s)) { core::recycle::forget(s); } // (the bin's record of what was just deleted for good)
                }
            }
        }
        refresh_all();
    }
}

// extract / compress the selection through core::ArchiveOps (tar.exe); the result arrives in service_archives()
void Shell::archive_action(const std::string& name)
{
    model::Tab&           tab = env_.tab();
    const model::Listing& L   = tab.listing();
    if (tab.path().empty() || tab.searching()) { toast(tr("Open a folder first (not a search result)"), toast_kind::info); return; }
    const std::vector<std::size_t> rows = L.selected_rows();
    if (name == "compress_zip") {
        if (rows.empty()) { toast(tr("Select what to compress"), toast_kind::info); return; }
        std::vector<std::string> names;
        for (const std::size_t r : rows) { names.push_back(L.at(r).name); }
        const std::string zip = rows.size() == 1 ? (L.at(rows.front()).is_dir() ? L.at(rows.front()).name : core::path::leaf(core::archive_stem(L.at(rows.front()).name)))
                                                 : core::path::leaf(tab.path());
        toast(tr("Compressing ..."), toast_kind::info);
        archives_.compress(tab.path(), std::move(names), zip.empty() ? "Archive" : zip);
        return;
    }
    for (const std::size_t r : rows) {
        const core::FileEntry& e = L.at(r);
        if (e.is_dir() || !core::is_archive_name(e.name)) { continue; }
        toast(tr("Extracting ..."), toast_kind::info);
        archives_.extract(core::path::join(tab.path(), e.name), tab.path(), name == "extract_folder");
        return;
    }
    toast(tr("Select an archive (zip, 7z, tar ...) first"), toast_kind::info);
}

void Shell::service_images()
{
    core::ImageJobs::Result r;
    while (image_jobs_.poll(r)) {
        toast(r.message, r.ok ? toast_kind::success : toast_kind::error);
        if (r.ok && !r.made.empty() && env_.tab().path() == r.dir) { env_.tab().reveal_name = r.made; }
        refresh_all();
    }
}

void Shell::open_archive(const std::string& archive)
{
    if (const auto root = views_.root_for(archive); root && core::path::is_directory(*root)) { // (unpacked already this session)
        env_.tab().navigate(*root);
        return;
    }
    const std::string dir = core::archive_cache_dir(archive, true);
    if (!core::path::is_directory(dir)) {
        toast(tr("Could not make a temporary folder for the archive"), toast_kind::error);
        return;
    }
    toast(trf("Opening {} ...", core::path::leaf(archive)), toast_kind::info);
    archives_.extract(archive, dir, false, "view\n" + archive);
}

void Shell::go_up(model::Tab& tab)
{
    if (const auto out = views_.leave_target(tab.path())) {
        tab.navigate(*out);
    } else {
        (void)tab.up();
    }
}

void Shell::service_archives()
{
    core::ArchiveOps::Result r;
    while (archives_.poll(r)) {
        if (r.tag.starts_with("view\n")) { // an archive being opened as a folder: no toast, it just appears
            const std::string archive = r.tag.substr(5);
            if (r.ok) {
                views_.add(r.dir, archive);
                env_.tab().navigate(r.dir);
            } else {
                toast(r.message, toast_kind::error);
            }
            continue;
        }
        toast(r.message, r.ok ? toast_kind::success : toast_kind::error);
        if (r.ok && !r.made.empty() && env_.tab().path() == r.dir) { env_.tab().reveal_name = r.made; }
        refresh_all();
    }
}

// a finished copy / move / recycle becomes an undo entry that carries its own inverse
void Shell::record_undo(const core::OpResult& r)
{
    core::FileOps* ops = &ops_;
    // the built-in queue says where each item ended up and which ones it skipped; the shell's own jobs keep their names
    // (a conflict dialog may have changed that, which is why the queue reports it)
    const bool known = r.made.size() == r.sources.size();
    std::vector<std::string> sources, created;
    for (std::size_t i = 0; i < r.sources.size(); ++i) {
        const std::string made = known ? r.made[i] : core::path::join(r.dest, core::path::leaf(r.sources[i]));
        if (made.empty()) { continue; } // (skipped, or failed: nothing to undo)
        sources.push_back(r.sources[i]);
        created.push_back(made);
    }
    const std::string dest = r.dest;
    const std::string what = sources.size() == 1 ? core::path::leaf(sources.front()) : trf_n("{} item", "{} items", static_cast<long long>(sources.size()), sources.size());
    if (sources.empty() && (r.kind == core::OpKind::copy || r.kind == core::OpKind::move)) { return; }

    switch (r.kind) {
    case core::OpKind::copy: {
        // a copy next to its original made by the shell gets a new name we do not know: undoing it would hit the original
        if (!known) {
            for (const std::string& s : sources) {
                if (core::path::parent(s) == dest) { return; }
            }
        }
        env_.history.push({trf("Copy {}", what),
                           [ops, created] { ops->start(core::OpKind::recycle, created, {}, true); return true; },
                           [ops, sources, dest] { ops->start(core::OpKind::copy, sources, dest, true); return true; }});
        break;
    }
    case core::OpKind::move:
        env_.history.push({trf("Move {}", what),
                           [ops, sources, created] {
                               // back to where each item came from (one job per source folder)
                               std::vector<std::string> parents;
                               for (const std::string& s : sources) {
                                   const std::string p = core::path::parent(s);
                                   if (std::find(parents.begin(), parents.end(), p) == parents.end()) { parents.push_back(p); }
                               }
                               for (const std::string& p : parents) {
                                   std::vector<std::string> group;
                                   for (std::size_t i = 0; i < sources.size(); ++i) {
                                       if (core::path::parent(sources[i]) == p) { group.push_back(created[i]); }
                                   }
                                   ops->start(core::OpKind::move, group, p, true);
                               }
                               return true;
                           },
                           [ops, sources, dest] { ops->start(core::OpKind::move, sources, dest, true); return true; }});
        break;
    case core::OpKind::recycle:
        env_.history.push({trf("Delete {}", what),
                           [sources] { return core::FileOps::restore_from_recycle(sources); },
                           [ops, sources] { ops->start(core::OpKind::recycle, sources, {}, true); return true; }});
        break;
    case core::OpKind::erase:
        break; // gone for good
    }
}

// the pinned folders and the drives as a searchable list (Ctrl+G): the keyboard way to reach the sidebar
void Shell::open_goto()
{
    goto_binds_ = strata::keybinds{};
    goto_targets_.clear();
    const auto add = [&](std::string label, const std::string& path) {
        if (std::ranges::any_of(goto_targets_, [&](const auto& t) { return t.first == label; })) { label += " (" + path + ")"; }
        goto_binds_.add(label, {}, path);
        goto_targets_.emplace_back(std::move(label), path);
    };
    for (const model::SidebarSection& s : layout_.sections()) {
        for (const model::SidebarItem& it : s.items) { add(it.label(), it.path); }
    }
    for (const core::Place& d : sidebar_.drives()) {
        if (!d.device) { add(d.name, d.path); }
    }
    goto_palette_.open();
}

void Shell::open_batch_rename()
{
    model::Tab&           tab = env_.tab();
    const model::Listing& L   = tab.listing();
    const std::vector<std::size_t> rows = L.selected_rows();
    if (rows.size() < 2) { toast(tr("Select two or more items to rename them together"), toast_kind::info); return; }
    if (tab.searching()) { toast(tr("Batch rename works inside one folder: leave the search first"), toast_kind::info); return; }
    std::vector<core::RenameItem> items;
    std::vector<std::string>      others;
    std::vector<bool>             picked(L.rows(), false);
    for (const std::size_t r : rows) {
        items.push_back({L.at(r).name, L.at(r).is_dir()});
        picked[r] = true;
    }
    for (std::size_t r = 0; r < L.rows(); ++r) {
        if (!picked[r]) { others.push_back(L.at(r).name); }
    }
    batch_rename_dialog_.open(tab.path(), std::move(items), std::move(others));
}

void Shell::do_undo()
{
    if (!env_.history.can_undo()) { toast(tr("Nothing to undo"), toast_kind::info); return; }
    const std::string label = env_.history.undo_label();
    if (env_.history.undo()) {
        toast(trf("Undone: {}", label), toast_kind::info);
    } else {
        toast(trf("Could not undo: {}", label), toast_kind::warning);
    }
    refresh_all();
}

void Shell::do_redo()
{
    if (!env_.history.can_redo()) { toast(tr("Nothing to redo"), toast_kind::info); return; }
    const std::string label = env_.history.redo_label();
    if (env_.history.redo()) {
        toast(trf("Redone: {}", label), toast_kind::info);
    } else {
        toast(trf("Could not redo: {}", label), toast_kind::warning);
    }
    refresh_all();
}

void Shell::save_session(int w, int h)
{
    settings_.window_w = w;
    settings_.window_h = h;
    settings_.sidebar_width = sidebar_w_;
    settings_.tabs.clear();
    for (std::size_t i = 0; i < env_.tabs.size(); ++i) { settings_.tabs.push_back(env_.tabs.at(i).path()); }
    settings_.active_tab = static_cast<int>(env_.tabs.active_index());
    settings_.save();
}

// --- drag and drop ---------------------------------------------------------------------------------------------

std::string Shell::resolve_drop(vec2 p) const
{
    if (const std::string s = sidebar_.drop_target_at(p); !s.empty()) { return s; }
    if (const std::string t = title_bar_.drop_target_at(p); !t.empty()) { return t; }
    if (const std::string r = file_list_r_.drop_target_at(p); !r.empty() && right_tab_) { return r; }
    return file_list_.drop_target_at(p);
}

void Shell::set_drop_point(std::optional<vec2> p)
{
    file_list_.set_drop_point(p && file_list_.contains(*p) ? p : std::nullopt);
    file_list_r_.set_drop_point(p && right_tab_ && file_list_r_.contains(*p) ? p : std::nullopt);
    ghost_at_ = p;
    sidebar_.set_drop_point(p);
    title_bar_.set_drop_point(p);
}

// a card that follows the pointer while files are dragged over the window, saying what a drop here would do
void Shell::draw_drag_ghost(Painter& p)
{
    if (ghost_paths_.empty() || !ghost_at_) { return; }
    context&       ui  = p.ui();
    const Palette& pal = env_.theme.pal();
    const FontSet& f   = env_.fonts;

    const std::string dest = resolve_drop(*ghost_at_);
    const std::string what = ghost_paths_.size() == 1 ? core::path::leaf(ghost_paths_.front()) : trf_n("{} item", "{} items", static_cast<long long>(ghost_paths_.size()), ghost_paths_.size());
    std::string       action;
    // dropping onto one of the dragged items, or back into the folder they already live in, does nothing: no hint
    const auto same_path = [](const std::string& a, const std::string& b) {
        const std::wstring wa = core::text::widen(a), wb = core::text::widen(b);
        const auto         trim = [](const std::wstring& s) { return s.size() > 3 && (s.back() == L'\\' || s.back() == L'/') ? s.size() - 1 : s.size(); };
        return ::CompareStringOrdinal(wa.c_str(), static_cast<int>(trim(wa)), wb.c_str(), static_cast<int>(trim(wb)), TRUE) == CSTR_EQUAL;
    };
    const bool no_op = !dest.empty() && !Sidebar::is_pin_target(dest) &&
                       std::ranges::any_of(ghost_paths_, [&](const std::string& g) { return same_path(g, dest) || same_path(core::path::parent(g), dest); });
    if (Sidebar::is_pin_target(dest)) { action = tr("Pin to sidebar"); }
    else if (!dest.empty() && !no_op) { action = trf("Drop in {}", core::path::leaf(dest)); }

    const float w1 = p.measure(f.body, what).x;
    const float w2 = action.empty() ? 0.0f : p.measure(f.small, action).x;
    const float w  = std::min(300.0f, 46.0f + std::max(w1, w2) + 16.0f);
    const float h  = action.empty() ? 36.0f : 50.0f;
    const vec2  s  = ui.display_size();
    const rect  at = rect::from_size({std::clamp(ghost_at_->x + 16.0f, 4.0f, s.x - w - 4.0f), std::clamp(ghost_at_->y + 18.0f, 4.0f, s.y - h - 4.0f)}, {w, h});

    auto fg = ui.layer(layer::foreground);
    p.dl().rect_filled(at.expanded(2.0f), color{0, 0, 0, 70}, 12.0f);
    p.dl().rect_filled(at, pal.raised, 10.0f);
    p.dl().rect_outline(at, action.empty() ? pal.border : pal.accent, 10.0f, action.empty() ? 1.0f : 1.5f);
    const rect icon = rect::from_size({at.min.x + 10.0f, at.center().y - 13.0f}, {26.0f, 26.0f});
    if (core::path::is_directory(ghost_paths_.front())) { p.folder_icon(icon); } else { p.glyph_center(icon, glyph::document, pal.text_dim); }
    const float tx = icon.max.x + 10.0f;
    if (action.empty()) {
        p.text_left(rect{{tx, at.min.y}, {at.max.x - 8.0f, at.max.y}}, what, pal.text, f.body);
    } else {
        p.text_left(rect{{tx, at.min.y + 5.0f}, {at.max.x - 8.0f, at.min.y + 26.0f}}, what, pal.text, f.body);
        p.text_left(rect{{tx, at.min.y + 26.0f}, {at.max.x - 8.0f, at.max.y - 4.0f}}, action, pal.accent, f.small);
    }
}

// --- shortcuts --------------------------------------------------------------------------------------------------

void Shell::shortcuts(context& ui)
{
    if (dialog_has_keyboard(ui)) { return; }
    model::TabSet& tabs = env_.tabs;
    const bool typing = ui.want_text_input();

    // fixed keys: Ctrl+1..9 pick a tab, backspace and the mouse side buttons go back / forward
    for (int i = 0; i < 9; ++i) {
        if (ui.key_pressed(static_cast<key>(static_cast<int>(key::d1) + i), true) && static_cast<std::size_t>(i) < tabs.size()) { tabs.activate(static_cast<std::size_t>(i)); }
    }
    model::Tab& tab = env_.tab();
    if (!typing && ui.key_pressed(key::backspace)) { (void)tab.back(); }
    if (ui.key_pressed(key::mouse_x1)) { (void)tab.back(); }
    if (ui.key_pressed(key::mouse_x2)) { (void)tab.forward(); }

    // everything else is a named action with a rebindable chord. a chord without Ctrl or Alt would eat typing: while a
    // text field has the keyboard only the modified ones fire
    for (const keybinds::action& a : env_.binds.actions()) {
        if (a.chord.count == 0 || !env_.binds.pressed(ui, a.name)) { continue; }
        if (typing && !a.chord.steps[0].ctrl && !a.chord.steps[0].alt) { continue; }
        run_action(a.name, ui);
    }
    if (const std::string pick = goto_palette_.show(ui, goto_binds_); !pick.empty()) {
        for (const auto& [label, path] : goto_targets_) {
            if (label == pick) { env_.tab().navigate(path); break; }
        }
    }
    const std::string chosen = palette_.show(ui, env_.binds); // (the palette runs any action by name)
    if (!chosen.empty()) { run_action(chosen, ui); }
}

void Shell::run_action(const std::string& name, context& ui)
{
    model::TabSet& tabs = env_.tabs;
    model::Tab&    tab  = env_.tab();

    if (name == "new_tab") { open_tab(tab.path(), true); }
    else if (name == "close_tab") { close_tab(tabs.active_index()); }
    else if (name == "reopen_tab") {
        if (closed_tabs_.empty()) { toast(tr("No closed tab to reopen"), toast_kind::info); }
        else { open_tab(closed_tabs_.back(), true); closed_tabs_.pop_back(); }
    }
    else if (name == "duplicate_tab") { open_tab(tabs.active().path(), true); }
    else if (name == "next_tab") { tabs.next(1); }
    else if (name == "prev_tab") { tabs.next(-1); }
    else if (name == "new_window") { spawn_window(tab.path()); }
    else if (name == "maximize") { env_.window.toggle_maximize(); }
    else if (name == "back") { (void)tab.back(); }
    else if (name == "forward") { (void)tab.forward(); }
    else if (name == "up") { go_up(tab); }
    else if (name == "home") { tab.navigate(core::home_folder()); }
    else if (name == "refresh") { tab.refresh(); }
    else if (name == "address") { focus_address(); }
    else if (name == "filter") { focus_filter(); }
    else if (name == "terminal") {
        // windows terminal if it is there, otherwise a plain console, started in this folder
        const std::wstring dir = core::text::widen(tab.path());
        // (no working directory for wt.exe: as a Store alias it would start nothing; -d carries the folder, doubled slash for a drive root)
        const std::wstring wt_dir = !dir.empty() && dir.back() == L'\\' ? dir + L"\\" : dir;
        const auto r = reinterpret_cast<INT_PTR>(::ShellExecuteW(nullptr, L"open", L"wt.exe", (L"-d \"" + wt_dir + L"\"").c_str(), nullptr, SW_SHOWNORMAL));
        if (r <= 32) { (void)::ShellExecuteW(nullptr, L"open", L"cmd.exe", nullptr, dir.c_str(), SW_SHOWNORMAL); }
    }
    else if (name == "hidden") { toggle_hidden(); }
    else if (name == "view_grid") { toggle_view(); }
    else if (name == "density") { toggle_density(); }
    else if (name == "split") { toggle_split(); }
    else if (name == "switch_pane") { if (right_tab_) { env_.right_active = !env_.right_active; } }
    else if (name == "preview") { toggle_preview(); }
    else if (name == "quick_look") { quick_look_.toggle(); }
    else if (name == "sessions") { sessions_dialog_.open(); }
    else if (name.starts_with("link_")) {
        const core::LinkKind kind = name == "link_hard" ? core::LinkKind::hard : name == "link_junction" ? core::LinkKind::junction : core::LinkKind::symbolic;
        const model::Listing& L = tab.listing();
        const std::vector<std::size_t> rows = L.selected_rows();
        // the link goes next to the original, or into the other pane's folder when two are shown
        const std::string folder = env_.other() != nullptr ? env_.other()->path() : tab.path();
        if (rows.empty()) { toast(tr("Select what to link to first"), toast_kind::info); }
        else if (folder.empty() || tab.searching()) { toast(tr("Open a folder first"), toast_kind::info); }
        else {
            std::vector<std::pair<std::string, std::string>> made; // link, target
            std::vector<bool> dirs;
            std::string error;
            for (const std::size_t r : rows) {
                const core::FileEntry& e = L.at(r);
                const std::string target = core::path::join(tab.path(), e.name);
                const std::size_t dot = e.is_dir() ? std::string::npos : e.name.rfind('.');
                const std::string stem = dot == std::string::npos || dot == 0 ? e.name : e.name.substr(0, dot);
                const std::string ext  = dot == std::string::npos || dot == 0 ? std::string{} : e.name.substr(dot);
                std::string link = core::path::join(folder, stem + " - Link" + ext);
                for (int i = 2; core::path::exists(link) && i < 1000; ++i) { link = core::path::join(folder, std::format("{} - Link ({}){}", stem, i, ext)); }
                error = core::make_link(kind, target, e.is_dir(), link);
                if (!error.empty()) { break; }
                made.emplace_back(link, target);
                dirs.push_back(e.is_dir());
            }
            if (!error.empty()) { toast(error, toast_kind::error); }
            if (!made.empty()) {
                const std::string label = made.size() == 1 ? trf("Link {}", core::path::leaf(made.front().first)) : trf("{} links", made.size());
                env_.history.push({label,
                                   [made] { for (const auto& m : made) { (void)core::remove_link(m.first); } return true; },
                                   [made, dirs, kind] {
                                       for (std::size_t i = 0; i < made.size(); ++i) { (void)core::make_link(kind, made[i].second, dirs[i], made[i].first); }
                                       return true;
                                   }});
                if (error.empty()) { toast(made.size() == 1 ? std::string{tr("Link created")} : trf("{} links created", made.size()), toast_kind::success); }
                refresh_all();
            }
        }
    }
    else if (name.starts_with("img_")) {
        static constexpr std::pair<std::string_view, core::ImageOp> ops[] = {
            {"img_jpg", core::ImageOp::to_jpeg}, {"img_png", core::ImageOp::to_png}, {"img_50", core::ImageOp::half}, {"img_25", core::ImageOp::quarter},
            {"img_1920", core::ImageOp::fit_1920}, {"img_left", core::ImageOp::rotate_left}, {"img_right", core::ImageOp::rotate_right}};
        const model::Listing& L = tab.listing();
        std::vector<std::string> paths;
        for (const std::size_t r : L.selected_rows()) {
            if (!L.at(r).is_dir() && core::is_image_name(L.at(r).name)) { paths.push_back(core::path::join(tab.path(), L.at(r).name)); }
        }
        if (paths.empty()) { toast(tr("Select one or more pictures first"), toast_kind::info); }
        else {
            for (const auto& [n, op] : ops) {
                if (n == name) { toast(tr("Working on the pictures ..."), toast_kind::info); image_jobs_.start(op, std::move(paths)); break; }
            }
        }
    }
    else if (name == "empty_folders") {
        if (tab.path().empty() || tab.searching()) { toast(tr("Open a folder first"), toast_kind::info); }
        else { empty_folders_dialog_.open(tab.path()); }
    }
    else if (name == "search_everywhere") {
        if (!settings_.index_drives) { toast(tr("Turn on \"Index drives for instant search\" in Settings first"), toast_kind::info); }
        else { search_dialog_.open(); }
    }
    else if (name == "compare_panes") {
        if (!right_tab_) { toast(trf("Show two panes first ({}), then compare them", key_hint(env_.binds, "split")), toast_kind::info); }
        else if (env_.tabs.active().path().empty() || right_tab_->path().empty() || env_.tabs.active().searching() || right_tab_->searching()) { toast(tr("Open a folder in both panes first"), toast_kind::info); }
        else { compare_dialog_.open(env_.tabs.active().path(), right_tab_->path()); }
    }
    else if (name == "disk_usage") {
        if (tab.path().empty() || tab.searching()) { toast(tr("Open a folder first"), toast_kind::info); }
        else { usage_dialog_.open(tab.path()); }
    }
    else if (name.starts_with("tag_")) {
        const model::Listing& L = tab.listing();
        std::vector<std::string> paths;
        for (const std::size_t r : L.selected_rows()) { paths.push_back(core::path::join(tab.path(), L.at(r).name)); }
        if (paths.empty()) { toast(tr("Select something to label first"), toast_kind::info); }
        else if (name == "tag_clear") { clear_tags(paths); }
        else { toggle_tag(paths, name.back() - '1'); }
    }
    else if (name.starts_with("group_")) {
        static constexpr std::string_view names[] = {"group_none", "group_name", "group_type", "group_modified", "group_size"};
        for (int i = 0; i < 5; ++i) {
            if (name == names[i]) { settings_.group_by = i; settings_dirty_ = true; }
        }
        if (settings_.grid_view && settings_.group_by != 0) { toast(tr("Groups show in the details view"), toast_kind::info); }
    }
    else if (name == "settings") { open_settings(); }
    else if (name == "palette") { palette_.open(); }
    else if (name == "pin_folder") { toggle_pin(tab.path()); }
    else if (name == "save_search") {
        const std::string text   = tab.searching() ? tab.search_query() : tab.listing().query().text();
        const std::string folder = tab.path();
        if (text.empty() || folder.empty()) {
            toast(tr("Type a search or filter first"), toast_kind::info);
        } else {
            const bool saved = edit_sidebar(tr("Save search"), [&](model::SidebarLayout& l) { (void)l.pin_search(l.pin_section(), folder, text); });
            toast(saved ? tr("Search saved to the sidebar") : tr("This search is already saved"), toast_kind::info);
        }
    }
    else if (name == "batch_rename") { open_batch_rename(); }
    else if (name == "extract_here" || name == "extract_folder" || name == "compress_zip") { archive_action(name); }
    else if (name == "find_duplicates") {
        if (tab.path().empty()) { toast(tr("Open a folder first"), toast_kind::info); }
        else { duplicates_dialog_.open(tab.path()); }
    }
    else if (name == "goto_folder") { open_goto(); }
    else if (name == "undo") { do_undo(); }
    else if (name == "redo") { do_redo(); }
    else if (name == "properties") {
        std::vector<std::string> sel;
        for (const std::size_t r : tab.listing().selected_rows()) { sel.push_back(core::path::join(tab.path(), tab.listing().at(r).name)); }
        if (sel.empty() && !tab.path().empty()) { sel.push_back(tab.path()); }
        open_properties(std::move(sel));
    }
    else {
        // the rest belong to the file list that has the keyboard
        FileList& list = env_.right_active && right_tab_ ? file_list_r_ : file_list_;
        (void)list.perform(ui, tab, name);
    }
}

// --- layout -----------------------------------------------------------------------------------------------------

// the draggable edge between the sidebar and the file card
void Shell::splitter(Painter& p, const rect& area)
{
    context& ui = p.ui();
    PanelScope panel{ui, "##split", area};
    const item_result r = ui.custom_item("##split_handle", {area.width(), area.height()});
    const vec2 mp = ui.mouse_pos();
    if (r.hovered && ui.mouse_clicked(mouse_button::left)) {
        split_drag_ = true;
        split_grab_ = sidebar_w_ - mp.x;
    }
    if (split_drag_) {
        if (ui.mouse_down(mouse_button::left)) {
            sidebar_w_ = std::clamp(mp.x + split_grab_, env_.theme.metrics().sidebar_min, 480.0f);
        } else {
            split_drag_ = false;
            settings_.sidebar_width = sidebar_w_;
            settings_dirty_ = true;
        }
    }
    if (r.hovered && ui.item_double_clicked()) {
        sidebar_w_ = env_.theme.metrics().sidebar_width;
        settings_.sidebar_width = 0.0f;
        settings_dirty_ = true;
    }
    const bool hot = r.hovered || split_drag_;
    if (hot) { cursor_ = cursor_kind::resize_ew; }
    const float k = ui.animate("split_hot", hot ? 1.0f : 0.0f, 18.0f);
    if (k > 0.01f) {
        const float x = area.center().x;
        p.dl().rect_filled({{x - 1.0f, area.min.y + 8.0f}, {x + 1.0f, area.max.y - 8.0f}}, p.pal().accent.scaled_alpha(k * 0.8f), 1.0f);
    }
}

void Shell::frame(context& ui, const FrameInput& in)
{
    cursor_ = cursor_kind::arrow;
    if (!index_started_) {
        index_started_ = true;
        if (settings_.index_drives) { rebuild_index(); }
    }
    for (const std::string& folder : env_.window.take_incoming()) { open_tab(folder, true); }
    env_.tabs.poll();
    if (right_tab_) { (void)right_tab_->poll(); }
    service_ops();
    service_archives();
    service_images();
    apply_sorts();
    apply_tags();
    sync_settings();
    {   // moving to another folder inside the same tab is a visit (switching tabs is not)
        const model::Tab& cur = env_.tabs.active();
        if (!settings_.track_history) {
            visits_.clear(); // (switched off: nothing stays in memory)
        } else if (&cur == visit_tab_ && cur.path() != visit_path_ && !cur.path().empty() && !cur.searching()) {
            visits_.note(cur.path());
        }
        visit_tab_  = &cur;
        visit_path_ = cur.path();
    }
    for (std::size_t i = 0; i < env_.tabs.size(); ++i) { env_.tabs.at(i).listing().set_show_hidden(show_hidden_); }
    if (right_tab_) { right_tab_->listing().set_show_hidden(show_hidden_); }
    shortcuts(ui);

    const Metrics& m   = env_.theme.metrics();
    const Palette& pal = env_.theme.pal();
    Painter p{ui, env_.theme, env_.fonts};

    const vec2  size = ui.display_size();
    const float w = size.x;
    const float h = size.y;

    {   // the window background: what shows between panels
        auto bg = ui.layer(layer::background);
        ui.draw().rect_filled({{0, 0}, size}, pal.window);
    }

    const float sw          = std::clamp(sidebar_w_, m.sidebar_min, std::max(m.sidebar_min, w * 0.5f));
    const float body_top    = m.title_bar + m.toolbar;
    const float body_bottom = h - m.status_bar;
    const rect title{{0, 0}, {w, m.title_bar}};
    const rect toolbar{{0, m.title_bar}, {w, body_top}};
    const rect sidebar{{0, body_top}, {sw - 6.0f, body_bottom}};
    const rect split{{sw - 6.0f, body_top}, {sw + 2.0f, body_bottom}};
    const rect status{{0, body_bottom}, {w, h}};

    // the file area: one or two cards, and the preview pane on the right
    const float area_l = sw + 2.0f;
    float       area_r = w - 10.0f;
    rect preview_rect{};
    if (settings_.preview) {
        const float pw = std::clamp(w * 0.26f, 280.0f, 400.0f);
        preview_rect   = rect{{area_r - pw, body_top}, {area_r, body_bottom - 2.0f}};
        area_r        -= pw + 8.0f;
    }
    const float bottom = body_bottom - 2.0f;

    title_bar_.frame(p, title);
    toolbar_.frame(p, toolbar, show_hidden_);
    sidebar_.frame(p, sidebar, in);
    bool keys_free = !dialog_has_keyboard(ui);
    if (keys_free && quick_look_.open() && ui.key_pressed(key::escape)) {
        quick_look_.close();
        keys_free = false; // (this Esc only closes the look: it must not also clear the selection)
    }
    if (right_tab_) {
        const float mid = (area_l + area_r) * 0.5f;
        file_list_.frame(p, rect{{area_l, body_top}, {mid - 4.0f, bottom}}, env_.tabs.active(), in, keys_free && !env_.right_active);
        file_list_r_.frame(p, rect{{mid + 4.0f, body_top}, {area_r, bottom}}, *right_tab_, in, keys_free && env_.right_active);
    } else {
        file_list_.frame(p, rect{{area_l, body_top}, {area_r, bottom}}, env_.tabs.active(), in, keys_free);
    }
    if (settings_.preview) { preview_.frame(p, preview_rect); }
    quick_look_.frame(p, rect{{area_l, body_top}, {area_r, bottom}});
    splitter(p, split);
    status_bar_.frame(p, status);
    transfers_ui_.frame_card(p, rect{{0, body_top}, {w, body_bottom}});

    service_delete(ui);
    service_remove_section(ui);
    properties_dialog_.frame(p);
    batch_rename_dialog_.frame(p);
    duplicates_dialog_.frame(p);
    sessions_dialog_.frame(p);
    usage_dialog_.frame(p);
    compare_dialog_.frame(p);
    transfers_ui_.frame_conflict(p);
    search_dialog_.frame(p);
    empty_folders_dialog_.frame(p);
    if (settings_dialog_.frame(p)) {
        settings_dirty_ = true;
    }
    draw_drag_ghost(p);
    if (env_.window.drop_hint()) { // another window is dragging a tab over this one
        auto fg = ui.layer(layer::foreground);
        const rect all = rect::from_size({0.0f, 0.0f}, ui.display_size());
        p.dl().rect_filled(all, p.pal().accent.scaled_alpha(0.10f));
        p.dl().rect_outline(all.expanded(-2.0f), p.pal().accent, 8.0f, 4.0f);
        ui.invalidate();
    }
    for (const Toast& t : pending_toasts_) { ui.toast(t.text, t.kind, t.kind == toast_kind::warning || t.kind == toast_kind::error ? 4.0f : 2.5f); }
    pending_toasts_.clear();
    {
        std::vector<std::string> actions;
        actions.swap(pending_actions_);
        for (const std::string& a : actions) { run_action(a, ui); }
        if (!actions.empty()) { ui.invalidate(); } // (an action may have opened a dialog above this point: draw it now, not at the next input)
    }
    if (apply_sorts()) { ui.invalidate(); } // (a header click or the settings changed the order: show it now)

    apply_deferred();

    if (layout_.revision() != saved_layout_) {
        saved_layout_ = layout_.revision();
        (void)layout_.save_file(Settings::sidebar_path());
    }
    if (folder_sorts_.revision() != saved_sorts_) {
        saved_sorts_ = folder_sorts_.revision();
        (void)folder_sorts_.save_file(Settings::folder_sorts_path());
    }
    if (tags_.revision() != saved_tags_) {
        saved_tags_ = tags_.revision();
        (void)tags_.save_file(Settings::tags_path());
    }
    if (sessions_.revision() != saved_sessions_) {
        saved_sessions_ = sessions_.revision();
        (void)sessions_.save_file(Settings::sessions_path());
    }
    if (visits_.revision() != saved_visits_) {
        saved_visits_ = visits_.revision();
        if (settings_.track_history) { (void)visits_.save_file(Settings::visits_path()); }
        else { ::DeleteFileW(core::text::widen(Settings::visits_path()).c_str()); } // (off: no history file either)
    }
}

} // namespace prism::ui
