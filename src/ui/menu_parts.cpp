#include "ui/menu_parts.hpp"

#include "core/file_ops.hpp"
#include "core/path.hpp"
#include "core/places.hpp"
#include "core/rclone.hpp"
#include "core/recycle.hpp"
#include "core/shell_ops.hpp"
#include "core/terminal.hpp"
#include "ui/tr.hpp"

#include <thread>

namespace prism::ui::menus {

using namespace strata;

bool terminal_target(const std::string& folder)
{
    return !folder.empty() && !core::is_device_path(folder) && !core::recycle::inside(folder) && !folder.starts_with("prism:") && !core::rclone::is_path(folder);
}

void open_file(Env& env, const std::string& path)
{
    if (core::rclone::is_path(path)) {
        env.cmd.toast(trf("Downloading {}...", core::path::leaf(path)), toast_kind::info);
        std::thread{[path] {
            std::string local, err;
            if (core::rclone::download_to_cache(path, local, err) && !local.empty()) {
                (void)core::shell::open(local);
            }
        }}.detach();
        return;
    }
    if (!core::shell::open(path)) { env.cmd.toast(trf("Could not open {}", core::path::leaf(path)), toast_kind::error); }
}

void open_in(context& ui, Env& env, const std::string& folder, bool enabled)
{
    if (ui.menu_item(tr("Open in new tab"), "", false, enabled)) { env.cmd.open_tab(folder, false); }
    if (ui.menu_item(tr("Open in new window"), "", false, enabled)) { env.cmd.open_window(folder); }
}

void terminal(context& ui, Env& env, const std::string& folder)
{
    auto sub = ui.menu(tr("Open in terminal"), terminal_target(folder));
    if (!sub) { return; }
    const auto start = [&](core::terminal::Shell shell, bool admin) {
        if (!core::terminal::open({shell, folder, {}, admin})) { env.cmd.toast(tr("Could not open the terminal"), toast_kind::error); }
    };
    const std::vector<core::terminal::Shell> shells = core::terminal::installed();
    for (const core::terminal::Shell s : shells) {
        if (ui.menu_item(std::string{core::terminal::label(s)} + "##t" + std::to_string(static_cast<int>(s)))) { start(s, false); }
    }
    // (already elevated: everything started from here is)
    if (core::shell::is_elevated()) { return; }
    ui.menu_separator();
    for (const core::terminal::Shell s : shells) {
        if (s == core::terminal::Shell::default_terminal) { continue; } // (Windows Terminal cannot be started elevated from here)
        if (ui.menu_item(trf("{} as administrator", core::terminal::label(s)) + "##a" + std::to_string(static_cast<int>(s)))) { start(s, true); }
    }
}

void paste_into(context& ui, Env& env, const std::string& folder, bool clipboard_has_files)
{
    if (!ui.menu_item(tr("Paste into folder"), "", false, clipboard_has_files)) { return; }
    std::vector<std::string> paths;
    bool                     cut = false;
    if (core::clipboard_get_files(paths, cut)) { env.ops.start(cut ? core::OpKind::move : core::OpKind::copy, std::move(paths), folder); }
}

void path_items(context& ui, Env& env, const std::vector<std::string>& paths)
{
    if (paths.empty()) { return; }
    if (ui.menu_item(paths.size() > 1 ? tr("Copy paths") : tr("Copy path"))) {
        std::string all;
        for (const std::string& s : paths) {
            if (!all.empty()) { all += "\r\n"; }
            all += s;
        }
        env.cmd.toast(core::shell::set_clipboard_text(all) ? (paths.size() > 1 ? tr("Paths copied") : tr("Path copied")) : tr("Could not copy to the clipboard"),
                      toast_kind::info);
    }
    if (ui.menu_item(tr("Show in File Explorer"))) {
        if (!core::shell::reveal_in_explorer(paths.front())) { env.cmd.toast(tr("Could not open File Explorer"), toast_kind::error); }
    }
}

void pin_item(context& ui, Env& env, const std::string& folder)
{
    if (ui.menu_item(env.layout.find(folder) ? tr("Unpin from sidebar") : tr("Pin to sidebar"), env.binds.text("pin_folder"))) { env.cmd.toggle_pin(folder); }
}

void properties_item(context& ui, Env& env, const std::vector<std::string>& paths)
{
    if (ui.menu_item(tr("Properties"), env.binds.text("properties"))) { env.cmd.open_properties(paths); }
}

void more_items(context& ui, ShellPage& page, const std::vector<core::ShellMenuItem>& items)
{
    if (items.empty()) { return; }
    // (one submenu: installed tools add a dozen rows, more than the window is tall)
    if (auto more = ui.menu(tr("More options"))) { page.draw(ui, items); }
}

void more_options(context& ui, Env& env, ShellPage& page, core::ShellMenu& menu, const std::vector<std::string>& paths, bool background)
{
    if (!env.settings.shell_menu || paths.empty()) { return; }
    menu.query(paths, background);
    more_items(ui, page, menu.items());
}

void windows_items(context& ui, Env& env, const std::vector<std::string>& paths)
{
    if (paths.empty()) { return; }
    if (paths.size() == 1 && ui.menu_item(tr("Windows properties"))) { (void)core::shell::show_properties(paths.front()); }
    if (ui.menu_item(tr("Windows options"))) { (void)core::shell::show_native_menu(paths, env.window.handle()); }
}

} // namespace prism::ui::menus
