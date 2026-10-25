#include "ui/new_menu.hpp"
#include "ui/tr.hpp"

#include "core/templates.hpp"
#include "ui/tr.hpp"
#include "core/text.hpp"
#include "ui/tr.hpp"

#include <windows.h>
#include <shellapi.h>

#include <vector>

namespace prism::ui {

namespace {

// the Templates folder listing, looked at again at most every 1.5 s while a menu is open (a small local folder)
const std::vector<core::Template>& user_list()
{
    static std::vector<core::Template> list;
    static ULONGLONG                   at = 0;
    const ULONGLONG                    now = ::GetTickCount64();
    if (at == 0 || now - at > 1500) {
        list = core::user_templates(Settings::templates_path());
        at   = now;
    }
    return list;
}

} // namespace

void new_template_items(strata::context& ui, Env& env)
{
    const auto& builtin = core::builtin_templates();
    for (std::size_t i = 0; i < builtin.size(); ++i) {
        if (ui.menu_item(builtin[i].label)) { env.cmd.request_action("new_template:b" + std::to_string(i)); }
    }
    const auto& user = user_list();
    if (user.empty()) {
        strata::menu_item_options none;
        none.enabled = false;
        (void)ui.menu_item(tr("No templates yet"), none);
    } else {
        if (!builtin.empty()) { ui.menu_separator(); }
        for (const core::Template& t : user) {
            const std::string file = t.stem + t.ext;
            core::text::note_codepoints(file);
            if (ui.menu_item(t.ext.empty() ? t.label : t.label + "  " + t.ext)) { env.cmd.request_action("new_template:u" + file); }
        }
    }
    ui.menu_separator();
    if (ui.menu_item(tr("Open templates folder"))) {
        const std::string dir = Settings::templates_path();
        const bool ready = ::CreateDirectoryW(core::text::widen(dir).c_str(), nullptr) || ::GetLastError() == ERROR_ALREADY_EXISTS;
        const bool shown = ready && reinterpret_cast<INT_PTR>(::ShellExecuteW(nullptr, L"open", core::text::widen(dir).c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
        if (!shown) { env.cmd.toast(tr("Could not open the templates folder"), strata::toast_kind::error); }
    }
}

} // namespace prism::ui
