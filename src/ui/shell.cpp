#include "ui/shell.hpp"

#include "core/path.hpp"
#include "core/places.hpp"
#include "core/shell_ops.hpp"
#include "core/text.hpp"
#include "ui/panel.hpp"
#include "strata/config.hpp"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <format>

namespace prism::ui {

using namespace strata;

Shell::Shell(platform::Window& window, Theme& theme, FontSet fonts, model::TabSet& tabs, core::FileOps& ops, Settings& settings,
             platform::ImageCache& images, strata::keybinds& binds, model::UndoStack& history)
    : layout_{model::SidebarLayout::defaults()},
      env_{window, theme, fonts, tabs, *this, ops, settings, images, binds, history, layout_, visits_},
      ops_{ops}, settings_{settings},
      title_bar_{env_}, toolbar_{env_}, sidebar_{env_}, file_list_{env_, false}, file_list_r_{env_, true}, preview_{env_},
      status_bar_{env_}, settings_dialog_{env_}, properties_dialog_{env_}
{
    sidebar_w_     = settings.sidebar_width > 0.0f ? settings.sidebar_width : theme.metrics().sidebar_width;
    applied_scale_ = settings.ui_scale;
    goto_palette_.shortcut = {}; // (opened by its action, not its own chord)
    (void)layout_.load_file(Settings::sidebar_path()); // (nothing stored yet keeps the defaults)
    saved_layout_ = layout_.revision();
    if (settings.track_history) { (void)visits_.load_file(Settings::visits_path()); }
    else { ::DeleteFileW(core::text::widen(Settings::visits_path()).c_str()); }
    saved_visits_ = visits_.revision();
}

// --- commands ---------------------------------------------------------------------------------------------------

void Shell::open_tab(const std::string& path, bool activate) { pending_open_.push_back({path, activate}); }
void Shell::close_tab(std::size_t index) { pending_close_.push_back(index); }
void Shell::toggle_theme() { settings_.dark = !settings_.dark; settings_dirty_ = true; }
void Shell::toggle_density() { settings_.compact = !settings_.compact; settings_dirty_ = true; }
void Shell::toggle_hidden() { settings_.show_hidden = !settings_.show_hidden; settings_dirty_ = true; }
void Shell::toggle_view() { settings_.grid_view = !settings_.grid_view; settings_dirty_ = true; }
void Shell::toggle_preview() { settings_.preview = !settings_.preview; settings_dirty_ = true; }
void Shell::toggle_split() { settings_.split = !settings_.split; settings_dirty_ = true; }
void Shell::focus_address() { toolbar_.begin_edit(); }
void Shell::focus_filter() { toolbar_.focus_filter(); }
void Shell::open_settings() { settings_dialog_.open(); }
void Shell::open_properties(std::vector<std::string> paths) { properties_dialog_.open(std::move(paths)); }
void Shell::toast(std::string text, toast_kind kind) { pending_toasts_.push_back({std::move(text), kind}); }

// a modal (delete confirmation, Properties, Settings) owns the keyboard: panels and shortcuts behind it still see key
// presses in strata, so Delete or typing must not reach the file list while one is open. (the palette is a modal too, but
// it runs the shortcuts itself)
bool Shell::dialog_has_keyboard(context& ui) const
{
    return !delete_paths_.empty() || (ui.modal_open() && !palette_.is_open() && !goto_palette_.is_open());
}

void Shell::request_delete(std::vector<std::string> paths, bool permanent)
{
    if (paths.empty()) { return; }
    if (!permanent && !settings_.confirm_delete) {
        ops_.start(core::OpKind::recycle, std::move(paths), {});
        return;
    }
    delete_paths_     = std::move(paths);
    delete_permanent_ = permanent;
    delete_ask_       = true;
}

void Shell::service_delete(context& ui)
{
    if (delete_paths_.empty()) { return; }
    const char* const id = delete_permanent_ ? "pfs_delete_forever" : "pfs_delete";
    if (delete_ask_) {
        const std::string what = delete_paths_.size() == 1 ? "1 item" : std::to_string(delete_paths_.size()) + " items";
        ui.ask_confirm(id, delete_permanent_ ? "Permanently delete " + what + "? This can't be undone."
                                             : "Move " + what + " to the Recycle Bin?");
        delete_ask_ = false;
    }
    int result = ui.confirm(id, {delete_permanent_ ? "Delete" : "Move to Recycle Bin", "Cancel"});
    // strata's confirm only knows Esc and clicks; Enter takes the first button
    if (result == 0 && ui.key_pressed(key::enter)) {
        ui.close_modal();
        result = 1;
    }
    if (result == 0) { return; }
    if (result == 1) { ops_.start(delete_permanent_ ? core::OpKind::erase : core::OpKind::recycle, std::move(delete_paths_), {}); }
    delete_paths_.clear();
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

bool Shell::edit_sidebar(std::string label, const std::function<void(model::SidebarLayout&)>& edit)
{
    const model::SidebarLayout before = layout_;
    edit(layout_);
    if (layout_.revision() == before.revision()) { return false; }
    env_.history.push({std::move(label),
                       [this, before] { layout_.restore(before); return true; },
                       [this, after = layout_] { layout_.restore(after); return true; }});
    return true;
}

void Shell::toggle_pin(const std::string& path)
{
    if (path.empty() || !core::path::is_directory(path)) {
        toast("Only folders can be pinned to the sidebar", toast_kind::info);
        return;
    }
    const std::string name = core::path::leaf(path);
    if (layout_.find(path)) {
        (void)edit_sidebar("Unpin " + name, [&](model::SidebarLayout& l) { (void)l.unpin(path); });
        toast("Unpinned " + name, toast_kind::info);
    } else {
        (void)edit_sidebar("Pin " + name, [&](model::SidebarLayout& l) { (void)l.pin(l.pin_section(), std::string::npos, path); });
        toast("Pinned " + name + " to the sidebar", toast_kind::info);
    }
}

void Shell::copy_sidebar()
{
    strata::config c;
    layout_.store(c);
    toast(core::shell::set_clipboard_text(c.to_string()) ? "Sidebar layout copied" : "Could not copy to the clipboard", toast_kind::info);
}

void Shell::paste_sidebar()
{
    strata::config c;
    (void)c.from_string(core::shell::get_clipboard_text());
    model::SidebarLayout incoming;
    if (!incoming.load(c)) {
        toast("The clipboard has no sidebar layout", toast_kind::warning);
        return;
    }
    (void)edit_sidebar("Paste sidebar layout", [&](model::SidebarLayout& l) { l.replace(incoming); });
    toast("Sidebar layout pasted", toast_kind::info);
}

void Shell::refresh_all()
{
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
}

// settings are the one source of truth; the theme and the panels follow them
void Shell::sync_settings()
{
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
        right_tab_->navigate(env_.tabs.active().path());
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
    core::OpResult r;
    while (ops_.poll(r)) {
        const std::string what = r.count == 1 ? "1 item" : std::format("{} items", r.count);
        if (r.aborted) {
            toast("Cancelled", toast_kind::warning);
        } else if (!r.ok) {
            toast("The operation failed", toast_kind::error);
        } else if (!r.silent) { // (an undo / redo reports itself)
            switch (r.kind) {
            case core::OpKind::copy:    toast("Copied " + what, toast_kind::success); break;
            case core::OpKind::move:    toast("Moved " + what, toast_kind::success); break;
            case core::OpKind::recycle: toast(what + " moved to the Recycle Bin", toast_kind::success); break;
            case core::OpKind::erase:   toast("Deleted " + what, toast_kind::success); break;
            }
            record_undo(r);
        }
        refresh_all();
    }
}

// a finished copy / move / recycle becomes an undo entry that carries its own inverse
void Shell::record_undo(const core::OpResult& r)
{
    core::FileOps* ops = &ops_;
    const std::vector<std::string> sources = r.sources;
    const std::string              dest    = r.dest;
    const std::string              what    = sources.size() == 1 ? core::path::leaf(sources.front()) : std::format("{} items", sources.size());

    std::vector<std::string> created; // where the items ended up (the same names: a conflict dialog may have changed that)
    for (const std::string& s : sources) { created.push_back(core::path::join(dest, core::path::leaf(s))); }

    switch (r.kind) {
    case core::OpKind::copy: {
        // a copy next to its original gets a new name we do not know: undoing it would hit the original
        for (const std::string& s : sources) {
            if (core::path::parent(s) == dest) { return; }
        }
        env_.history.push({"Copy " + what,
                           [ops, created] { ops->start(core::OpKind::recycle, created, {}, true); return true; },
                           [ops, sources, dest] { ops->start(core::OpKind::copy, sources, dest, true); return true; }});
        break;
    }
    case core::OpKind::move:
        env_.history.push({"Move " + what,
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
        env_.history.push({"Delete " + what,
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
    for (const core::Place& d : core::drives()) { add(d.name, d.path); }
    goto_palette_.open();
}

void Shell::do_undo()
{
    if (!env_.history.can_undo()) { toast("Nothing to undo", toast_kind::info); return; }
    const std::string label = env_.history.undo_label();
    if (env_.history.undo()) {
        toast("Undone: " + label, toast_kind::info);
    } else {
        toast("Could not undo: " + label, toast_kind::warning);
    }
    refresh_all();
}

void Shell::do_redo()
{
    if (!env_.history.can_redo()) { toast("Nothing to redo", toast_kind::info); return; }
    const std::string label = env_.history.redo_label();
    if (env_.history.redo()) {
        toast("Redone: " + label, toast_kind::info);
    } else {
        toast("Could not redo: " + label, toast_kind::warning);
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
    const std::string what = ghost_paths_.size() == 1 ? core::path::leaf(ghost_paths_.front()) : std::format("{} items", ghost_paths_.size());
    std::string       action;
    if (Sidebar::is_pin_target(dest)) { action = "Pin to sidebar"; }
    else if (!dest.empty()) { action = "Drop in " + core::path::leaf(dest); }

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
        if (closed_tabs_.empty()) { toast("No closed tab to reopen", toast_kind::info); }
        else { open_tab(closed_tabs_.back(), true); closed_tabs_.pop_back(); }
    }
    else if (name == "duplicate_tab") { open_tab(tabs.active().path(), true); }
    else if (name == "next_tab") { tabs.next(1); }
    else if (name == "prev_tab") { tabs.next(-1); }
    else if (name == "new_window") {
        wchar_t exe[MAX_PATH]{};
        ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring{exe} + L"\" \"" + core::text::widen(tab.path()) + L"\"";
        STARTUPINFOW si{};
        si.cb = sizeof si;
        PROCESS_INFORMATION pi{};
        if (::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            ::CloseHandle(pi.hThread);
            ::CloseHandle(pi.hProcess);
        }
    }
    else if (name == "maximize") { env_.window.toggle_maximize(); }
    else if (name == "back") { (void)tab.back(); }
    else if (name == "forward") { (void)tab.forward(); }
    else if (name == "up") { (void)tab.up(); }
    else if (name == "home") { tab.navigate(core::home_folder()); }
    else if (name == "refresh") { tab.refresh(); }
    else if (name == "address") { focus_address(); }
    else if (name == "filter") { focus_filter(); }
    else if (name == "terminal") {
        // windows terminal if it is there, otherwise a plain console, started in this folder
        const std::wstring dir = core::text::widen(tab.path());
        const auto r = reinterpret_cast<INT_PTR>(::ShellExecuteW(nullptr, L"open", L"wt.exe", (L"-d \"" + dir + L"\"").c_str(), dir.c_str(), SW_SHOWNORMAL));
        if (r <= 32) { (void)::ShellExecuteW(nullptr, L"open", L"cmd.exe", nullptr, dir.c_str(), SW_SHOWNORMAL); }
    }
    else if (name == "hidden") { toggle_hidden(); }
    else if (name == "view_grid") { toggle_view(); }
    else if (name == "density") { toggle_density(); }
    else if (name == "split") { toggle_split(); }
    else if (name == "switch_pane") { if (right_tab_) { env_.right_active = !env_.right_active; } }
    else if (name == "preview") { toggle_preview(); }
    else if (name == "settings") { open_settings(); }
    else if (name == "palette") { palette_.open(); }
    else if (name == "pin_folder") { toggle_pin(tab.path()); }
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
    env_.tabs.poll();
    if (right_tab_) { (void)right_tab_->poll(); }
    service_ops();
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
    const bool keys_free = !dialog_has_keyboard(ui);
    if (right_tab_) {
        const float mid = (area_l + area_r) * 0.5f;
        file_list_.frame(p, rect{{area_l, body_top}, {mid - 4.0f, bottom}}, env_.tabs.active(), in, keys_free && !env_.right_active);
        file_list_r_.frame(p, rect{{mid + 4.0f, body_top}, {area_r, bottom}}, *right_tab_, in, keys_free && env_.right_active);
    } else {
        file_list_.frame(p, rect{{area_l, body_top}, {area_r, bottom}}, env_.tabs.active(), in, keys_free);
    }
    if (settings_.preview) { preview_.frame(p, preview_rect); }
    splitter(p, split);
    status_bar_.frame(p, status);

    service_delete(ui);
    properties_dialog_.frame(p);
    if (settings_dialog_.frame(p)) {
        settings_dirty_ = true;
    }
    draw_drag_ghost(p);
    for (const Toast& t : pending_toasts_) { ui.toast(t.text, t.kind); }
    pending_toasts_.clear();

    apply_deferred();

    if (layout_.revision() != saved_layout_) {
        saved_layout_ = layout_.revision();
        (void)layout_.save_file(Settings::sidebar_path());
    }
    if (visits_.revision() != saved_visits_) {
        saved_visits_ = visits_.revision();
        if (settings_.track_history) { (void)visits_.save_file(Settings::visits_path()); }
        else { ::DeleteFileW(core::text::widen(Settings::visits_path()).c_str()); } // (off: no history file either)
    }
}

} // namespace prism::ui
