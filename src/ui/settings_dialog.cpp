#include "ui/settings_dialog.hpp"
#include "ui/tr.hpp"

#include "core/default_manager.hpp"
#include "ui/tr.hpp"
#include "core/i18n.hpp"
#include "ui/tr.hpp"
#include "core/text.hpp"
#include "ui/tr.hpp"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <array>

namespace prism::ui {

using namespace strata;
using core::i18n::tr;

namespace {

constexpr std::array<int, 5> k_scales{90, 100, 110, 125, 150};
constexpr std::array<unsigned, 8> k_accents{0x6d8cff, 0x8b5cf6, 0xec4899, 0xef4444, 0xf59e0b, 0x22c55e, 0x14b8a6, 0x0ea5e9};

// a settings row: the label (and a hint) on the left, then whatever `control` draws on the same line. `control_w` is
// the width it will take, so the label gets the rest
template <class Fn>
bool setting_row(Kit& kit, std::string_view id, std::string_view label, std::string_view hint, float control_w, bool separator, Fn control)
{
    Painter&       p   = kit.painter();
    context&       ui  = p.ui();
    const Palette& pal = p.pal();
    const float base_h = kit.row_h() + 12.0f;
    const float w = ui.content_width();
    const float label_w = std::max(60.0f, w - control_w - Kit::pad() * 2.0f);
    const bool hinted = !hint.empty();
    const auto lines = kit.hint_lines(hint, label_w - Kit::pad());
    const float extra = hinted ? kit.hint_extra(lines.size()) : 0.0f;
    kit.add_extra(extra);
    const float h = base_h + extra;
    const item_result r = ui.custom_item(id, {label_w, h});
    p.text_left(rect{{r.bounds.min.x, r.bounds.min.y + (hinted ? 6.0f : 0.0f)}, {r.bounds.max.x, r.bounds.min.y + (hinted ? base_h * 0.55f : h)}}, label, pal.text, p.fonts().body, Kit::pad());
    if (hinted) {
        const float lh = p.line_height(p.fonts().small) + 1.0f;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const float y = r.bounds.min.y + base_h * 0.5f + lh * static_cast<float>(i);
            p.text_left(rect{{r.bounds.min.x, y}, {r.bounds.max.x, y + lh + 2.0f}}, lines[i], pal.text_dim, p.fonts().small, Kit::pad());
        }
    }
    if (separator) { p.dl().rect_filled(rect{{r.bounds.min.x + Kit::pad(), r.bounds.max.y - 1.0f}, {r.bounds.min.x + w - Kit::pad(), r.bounds.max.y}}, pal.divider); }
    ui.same_line();
    const bool changed = control(h);
    ui.same_line();
    (void)ui.custom_item(std::string{id} + "_pad", {Kit::pad(), h});
    return changed;
}

} // namespace

bool SettingsDialog::frame(Painter& p)
{
    context& ui = p.ui();
    Settings& s = env_.settings;
    bool changed = false;

    if (want_open_) {
        want_open_ = false;
        shown_     = true;
        default_manager_ = core::default_manager_active();
        bin_handler_     = core::recycle_bin_handler_active();
        Kit::reset_focus();
        ui.open_modal("Settings");
    }
    if (!shown_) { return false; }
    const Palette& pal = p.pal();
    const FontSet  f   = p.fonts();
    const DialogStyle style{ui, pal};
    const float width = std::min(620.0f, ui.display_size().x - 40.0f);
    auto modal = ui.modal("Settings", {width, 0.0f}, modal_flags::esc_closes | modal_flags::backdrop_closes | modal_flags::no_title_bar);
    if (!modal) {
        shown_ = false;
        Kit::reset_focus();
        return false;
    }

    Kit kit{p, env_.cmd};
    kit.begin_frame();

    // header
    {
        const item_result r = ui.custom_item("##set_head", {ui.content_width() - 36.0f, 48.0f});
        p.text_left(rect{{r.bounds.min.x, r.bounds.min.y + 2.0f}, {r.bounds.max.x, r.bounds.min.y + 30.0f}}, tr("Settings"), pal.text, f.title);
        p.text_left(rect{{r.bounds.min.x, r.bounds.min.y + 28.0f}, {r.bounds.max.x, r.bounds.max.y}}, tr("Changes apply and save at once"), pal.text_dim, f.body);
        ui.same_line();
        if (kit.icon_button("##set_close", glyph::close, tr("Close  (Esc)"), true, true, 32.0f)) { ui.close_modal(); }
    }
    kit.vgap(10.0f);

    const float max_h  = std::clamp(ui.display_size().y - 250.0f, 260.0f, 640.0f);
    const float body_h = std::min(max_h, content_h_ + 2.0f);
    {
        // (the scrollbar sits in the dialog's right padding, so the cards keep the same margin on both sides)
        auto body = ui.child("##set_body", {ui.content_width() + (content_h_ + 2.0f > max_h ? 16.0f : 0.0f), body_h}, child_flags::no_padding);
        if (body) {
            const float rh = kit.row_h() + 12.0f;

            const float sw_h = kit.row_h() + 16.0f;

            // the few things most people want
            // language: one row per *.lang file found (English is built in); files can be edited and reloaded without a restart
            {
                const auto  langs  = core::i18n::languages();
                kit.card_begin("##card_lang", tr("Language"), rh + 6.0f + kit.control_h() + 14.0f);
                std::vector<std::string_view> names;
                int cur = 0;
                for (std::size_t i = 0; i < langs.size(); ++i) {
                    names.push_back(langs[i].name);
                    if (langs[i].code == s.language) { cur = static_cast<int>(i); }
                }
                kit.vgap(8.0f);
                setting_row(kit, "##r_lang", tr("Interface language"), {}, 200.0f, false, [&](float) {
                    ui.set_next_item_width(200.0f);
                    ui.push_var(style_var::frame_padding_y, std::max(2.0f, (kit.control_h() - p.line_height(f.body)) * 0.5f));
                    const bool picked = ui.combo("##lang", cur, names);
                    ui.pop_var();
                    if (picked) {
                        s.language = std::string{langs[static_cast<std::size_t>(cur)].code};
                        core::i18n::set_language(s.language);
                        changed = true;
                    }
                    return false;
                });
                kit.vgap(6.0f);
                (void)ui.custom_item("##lang_lead", {Kit::pad(), 1.0f});
                ui.same_line();
                if (kit.button({"##lang_folder", tr("Open languages folder"), 0, Kit::ButtonKind::secondary, true, tr("Put a .lang file here to add a language"), 0.0f})) {
                    const std::string dir = Settings::language_dirs().back();
                    ::CreateDirectoryW(core::text::widen(dir).c_str(), nullptr);
                    ::ShellExecuteW(nullptr, L"open", core::text::widen(dir).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
                kit.hgap(8.0f);
                if (kit.button({"##lang_reload", tr("Reload"), 0, Kit::ButtonKind::secondary, true, tr("Read the language files again after editing them"), 0.0f})) {
                    core::i18n::scan(Settings::language_dirs());
                    core::i18n::set_language(s.language);
                }
                kit.vgap(8.0f);
                kit.card_end("##card_lang");
            }

            kit.card_begin("##card_look", tr("Appearance"), rh * 5.0f + sw_h);
            int theme = s.dark ? 0 : 1;
            const std::array<std::string_view, 2> themes{tr("Dark"), tr("Light")};
            if (setting_row(kit, "##r_theme", tr("Theme"), {}, 180.0f, true, [&](float h) { return kit.pill_tabs("##t_theme", themes, theme, 180.0f, h); })) {
                s.dark = theme == 0;
                changed = true;
            }
            int style_i = s.dark_style;
            const std::array<std::string_view, 3> styles{tr("Neutral"), tr("Graphite"), tr("Slate")};
            if (setting_row(kit, "##r_dstyle", tr("Dark style"), s.dark ? std::string_view{} : std::string_view{"Used by the dark theme"}, 270.0f, true,
                            [&](float h) { return kit.pill_tabs("##t_dstyle", styles, style_i, 270.0f, h); })) {
                s.dark_style = style_i;
                changed = true;
            }
            const float sw_total = 28.0f * static_cast<float>(k_accents.size());
            setting_row(kit, "##r_accent", tr("Accent colour"), {}, sw_total, true, [&](float h) {
                for (std::size_t i = 0; i < k_accents.size(); ++i) {
                    const unsigned rgb = k_accents[i];
                    if (i > 0) { ui.same_line(); }
                    const color c{static_cast<u8>(rgb >> 16), static_cast<u8>(rgb >> 8), static_cast<u8>(rgb), 255};
                    if (kit.swatch("##acc" + std::to_string(i), c, s.accent == rgb, h)) {
                        s.accent = rgb;
                        changed  = true;
                    }
                }
                return false;
            });
            // any other colour: the swatch opens a full picker
            setting_row(kit, "##r_custom", tr("Custom colour"), {}, 150.0f, true, [&](float h) {
                color c{static_cast<u8>(s.accent >> 16), static_cast<u8>(s.accent >> 8), static_cast<u8>(s.accent), 255};
                ui.set_next_item_width(150.0f);
                ui.push_var(style_var::frame_padding_y, std::max(2.0f, (kit.control_h() - p.line_height(f.body)) * 0.5f));
                const bool edited = ui.color_edit("##accent", c, color_flags::no_alpha);
                ui.pop_var();
                if (edited) {
                    s.accent = (static_cast<unsigned>(c.r) << 16) | (static_cast<unsigned>(c.g) << 8) | c.b;
                    changed  = true;
                }
                (void)h;
                return false;
            });
            int scale = 1;
            for (std::size_t i = 0; i < k_scales.size(); ++i) { if (k_scales[i] == s.ui_scale) { scale = static_cast<int>(i); } }
            const std::array<std::string_view, 5> scales{"90 %", "100 %", "110 %", "125 %", "150 %"};
            if (setting_row(kit, "##r_scale", tr("Interface size"), {}, 330.0f, true, [&](float h) { return kit.pill_tabs("##t_scale", scales, scale, 330.0f, h); })) {
                s.ui_scale = k_scales[static_cast<std::size_t>(scale)];
                changed = true;
            }
            if (kit.switch_row("##sw_compact", tr("Compact density"), tr("Smaller rows in the file list"), s.compact, false)) { changed = true; }
            kit.card_end("##card_look");

            kit.card_begin("##card_beh", tr("Behavior"), kit.row_h() * 8.0f + 16.0f * 6.0f);
            if (kit.switch_row("##sw_hidden", tr("Show hidden files"), {}, s.show_hidden, true)) { changed = true; }
            if (kit.switch_row("##sw_confirm", tr("Ask before deleting"), tr("Before a file goes to the Recycle Bin"), s.confirm_delete, true)) { changed = true; }
            if (kit.switch_row("##sw_tabs", tr("Open my tabs again next time"), {}, s.restore_tabs, true)) { changed = true; }
            if (kit.switch_row("##sw_background", tr("Run in the background"), tr("Stay in the tray after the last window is closed"), s.background, true)) { changed = true; }
            if (kit.switch_row("##sw_index", tr("Index drives for instant search"), trf("Lets Search everywhere ({}) find files instantly", key_hint(env_.binds, "search_everywhere")), s.index_drives, true)) { changed = true; }
            if (kit.switch_row("##sw_copyengine",tr("Copy and move in PrismFS"), tr("Shows progress with pause and cancel; when off, Windows' own copy window is used"), s.builtin_copy, true)) { changed = true; }
            if (kit.switch_row("##sw_archives",tr("Archive support"), tr("Enter on a zip, 7z or tar shows what is inside; adds Extract and Compress. When off, archives open with Windows' default app"), s.browse_archives, true)) { changed = true; }
            if (kit.switch_row("##sw_shellmenu", tr("Windows context menu entries"), tr("Open with Code, 7-Zip and the other entries Explorer shows"), s.shell_menu, false)) { changed = true; }
            kit.card_end("##card_beh");

            // sorting: how a folder is ordered when it opens. clicking a column header changes it for the folder (or for all)
            {
                const float sw_h = kit.row_h() + 16.0f;
                kit.card_begin("##card_sort", tr("Sorting"), rh * 3.0f + sw_h * 3.0f);
                int folders = s.sort_folders;
                const std::array<std::string_view, 3> placements{tr("On top"), tr("With files"), tr("At the bottom")};
                if (setting_row(kit, "##r_sfolders", tr("Folders"), {}, 330.0f, true, [&](float h) { return kit.pill_tabs("##t_sfolders", placements, folders, 330.0f, h); })) {
                    s.sort_folders = folders;
                    changed = true;
                }
                int key = s.sort_key;
                const std::array<std::string_view, 4> keys{tr("Name"), tr("Date"), tr("Type"), tr("Size")};
                if (setting_row(kit, "##r_skey", tr("Sort by"), tr("What every folder starts with"), 300.0f, true, [&](float h) { return kit.pill_tabs("##t_skey", keys, key, 300.0f, h); })) {
                    s.sort_key = key;
                    changed = true;
                }
                int dir = s.sort_ascending ? 0 : 1;
                const std::array<std::string_view, 2> dirs{tr("Ascending"), tr("Descending")};
                if (setting_row(kit, "##r_sdir", tr("Direction"), {}, 230.0f, true, [&](float h) { return kit.pill_tabs("##t_sdir", dirs, dir, 230.0f, h); })) {
                    s.sort_ascending = dir == 0;
                    changed = true;
                }
                if (kit.switch_row("##sw_natural", tr("Numbers in order"), tr("Puts file2 before file10; when off, names are sorted strictly letter by letter"), s.sort_natural, true)) { changed = true; }
                if (kit.switch_row("##sw_hiddenlast", tr("Hidden files last"), tr("Dot files and hidden items go after the rest of their group"), s.sort_hidden_last, true)) { changed = true; }
                if (kit.switch_row("##sw_perfolder", tr("Remember the order per folder"), tr("A column you click is remembered for that folder only; when off, it sets the order for every folder"), s.sort_per_folder, false)) { changed = true; }
                kit.card_end("##card_sort");
            }

            // Windows: who opens folders for other programs. per user, nothing needs admin rights, and off puts Explorer back
            kit.card_begin("##card_sys", tr("Windows"), (kit.row_h() + 16.0f) * 2.0f);
            if (kit.switch_row("##sw_default", tr("Open folders from other apps with PrismFS"), tr("Folders opened by other apps use PrismFS instead of Windows Explorer; Win+E and the taskbar still open Explorer"), default_manager_, false)) {
                if (!core::set_default_manager(default_manager_)) {
                    default_manager_ = !default_manager_;
                    env_.cmd.toast(tr("Windows would not change the setting"), toast_kind::error);
                }
            }
            if (kit.switch_row("##sw_bin", tr("Open the desktop Recycle Bin with PrismFS"), tr("Double-clicking the Recycle Bin icon opens it in PrismFS; Empty and Properties in its menu stay with Windows"), bin_handler_, false)) {
                if (!core::set_recycle_bin_handler(bin_handler_)) {
                    bin_handler_ = !bin_handler_;
                    env_.cmd.toast(tr("Windows would not change the setting"), toast_kind::error);
                }
            }
            kit.card_end("##card_sys");

            // sidebar: which lists it has, drives left out, and moving the layout in and out
            {
                using Auto = model::SidebarSection::Auto;
                const model::SidebarLayout& lay = env_.layout;
                const std::vector<std::string> hidden = lay.hidden_drives(); // (a copy: a click below edits the layout)
                const float sw_h  = kit.row_h() + 16.0f;
                const float row_h = kit.row_h() + 12.0f;
                kit.card_begin("##card_side", tr("Sidebar"), sw_h * 4.0f + row_h * static_cast<float>(hidden.size()) + sw_h);

                const auto slot_for_auto = [&] {
                    std::size_t slot = 0;
                    for (std::size_t i = 0; i < lay.sections().size(); ++i) {
                        if (!lay.sections()[i].drives) { slot = i + 1; }
                    }
                    return slot;
                };
                const auto toggle_auto = [&](Auto kind, std::string_view hide_label, std::string_view show_label) {
                    const std::size_t slot = slot_for_auto();
                    if (lay.has_auto(kind)) {
                        std::size_t at = 0;
                        for (std::size_t i = 0; i < lay.sections().size(); ++i) {
                            if (lay.sections()[i].auto_kind == kind) { at = i; }
                        }
                        (void)env_.cmd.edit_sidebar(hide_label, [=](model::SidebarLayout& l) { (void)l.remove_section(at); });
                    } else {
                        (void)env_.cmd.edit_sidebar(show_label, [=](model::SidebarLayout& l) { (void)l.add_auto_section(slot, kind); });
                    }
                };
                bool drives_on = lay.has_drives();
                if (kit.switch_row("##sw_drives", tr("Drives"), tr("This PC: every drive with its free space"), drives_on, true)) {
                    if (drives_on) {
                        (void)env_.cmd.edit_sidebar(tr("Show This PC"), [](model::SidebarLayout& l) { (void)l.add_drives_section(std::string::npos); });
                    } else {
                        std::size_t at = 0;
                        for (std::size_t i = 0; i < lay.sections().size(); ++i) {
                            if (lay.sections()[i].drives) { at = i; }
                        }
                        (void)env_.cmd.edit_sidebar(tr("Hide This PC"), [=](model::SidebarLayout& l) { (void)l.remove_section(at); });
                    }
                }
                bool recent_on = lay.has_auto(Auto::recent);
                if (kit.switch_row("##sw_recent", tr("Recent folders"), tr("The folders you opened last"), recent_on, true)) { toggle_auto(Auto::recent, tr("Hide recent folders"), tr("Show recent folders")); }
                bool frequent_on = lay.has_auto(Auto::frequent);
                if (kit.switch_row("##sw_frequent", tr("Frequent folders"), tr("The ones you keep coming back to"), frequent_on, true)) { toggle_auto(Auto::frequent, tr("Hide frequent folders"), tr("Show frequent folders")); }
                if (kit.switch_row("##sw_history", tr("Remember visited folders"), tr("Needed for the Recent and Frequent lists; when off, visited folders are not recorded"), s.track_history, false)) { changed = true; }

                for (std::size_t i = 0; i < hidden.size(); ++i) {
                    const std::string root = hidden[i];
                    const float bw = kit.button_width(tr("Show again"), false);
                    if (setting_row(kit, "##hd" + std::to_string(i), trf("Hidden drive  {}", root), {}, bw, true, [&](float) {
                            return kit.button({"##hd_btn" + std::to_string(i), tr("Show again"), 0, Kit::ButtonKind::secondary, true, {}, bw});
                        })) {
                        (void)env_.cmd.edit_sidebar(trf("Show {}", root), [=](model::SidebarLayout& l) { (void)l.show_drive(root); });
                    }
                }

                kit.vgap(8.0f);
                (void)ui.custom_item("##sb_lead", {Kit::pad(), 1.0f});
                ui.same_line();
                if (kit.button({"##sb_copy", tr("Copy layout"), 0, Kit::ButtonKind::secondary, true, tr("Put this sidebar on the clipboard as text"), 0.0f})) { env_.cmd.copy_sidebar(); }
                kit.hgap(8.0f);
                if (kit.button({"##sb_paste", tr("Paste layout"), 0, Kit::ButtonKind::secondary, true, tr("Use a sidebar copied from here or from a friend"), 0.0f})) { env_.cmd.paste_sidebar(); }
                kit.hgap(8.0f);
                if (kit.button({"##sb_reset", tr("Reset"), 0, Kit::ButtonKind::ghost, true, tr("Restores Quick access and This PC"), 0.0f})) {
                    (void)env_.cmd.edit_sidebar(tr("Reset sidebar"), [](model::SidebarLayout& l) { l.reset(); });
                }
                kit.vgap(8.0f);
                kit.card_end("##card_side");
            }

            // shortcuts
            // the table is laid out inline (height 0): a scroller inside the page's scroller would take the wheel whenever
            // the pointer crossed it and stall the page. its height is measured and the card follows one frame behind
            kit.card_begin("##card_keys", tr("Keyboard shortcuts"), 32.0f + keys_h_ + 8.0f);
            const item_result hint = ui.custom_item("##keys_hint", {ui.content_width(), 32.0f});
            p.text_left(hint.bounds, trf("Click a field and press the new keys  -  {} searches every command", key_hint(env_.binds, "palette")), pal.text_dim, f.small, Kit::pad());
            (void)ui.custom_item("##keys_lead", {Kit::pad(), 1.0f});
            ui.same_line();
            ui.set_next_item_width(ui.content_width() - Kit::pad() * 2.0f);
            if (strata::keybind_editor(ui, env_.binds, "pfs_keys")) { changed = true; }
            const item_result keys_end = ui.custom_item("##keys_end", {1.0f, 8.0f});
            keys_h_ = keys_end.bounds.min.y - hint.bounds.max.y;
            kit.card_end("##card_keys");

            // diagnostics: for "why is it slow to start / slow sometimes"
            kit.card_begin("##card_diag", tr("Diagnostics"), kit.row_h() + 16.0f + kit.control_h() + 14.0f);
            if (kit.switch_row("##sw_log", tr("Write a log file"), tr("Records start-up steps and any slow frames or folders, with timings"), s.log_enabled, false)) { changed = true; }
            kit.vgap(6.0f);
            (void)ui.custom_item("##log_lead", {Kit::pad(), 1.0f});
            ui.same_line();
            if (kit.button({"##log_open", tr("Open log"), 0, Kit::ButtonKind::secondary, true, tr("Open prismfs.log in your text editor"), 0.0f})) {
                const std::wstring w = core::text::widen(Settings::log_path());
                ::ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            kit.hgap(8.0f);
            if (kit.button({"##log_dir", tr("Show in folder"), 0, Kit::ButtonKind::secondary, true, {}, 0.0f})) {
                const std::wstring arg = L"/select,\"" + core::text::widen(Settings::log_path()) + L"\"";
                ::ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
            }
            kit.vgap(8.0f);
            kit.card_end("##card_diag");

            // about
            const std::string file = Settings::file_path();
            kit.card_begin("##card_about", tr("About"), kit.row_h() * 2.0f);
            const float key_w = std::max(90.0f, p.measure(f.body, tr("Settings file")).x + 24.0f);
            kit.key_value({"##ab_app", tr("PrismFS"), tr("A fast file explorer, built on the strata UI framework"), false, false, true, key_w});
            kit.key_value({"##ab_file", tr("Settings file"), file, false, true, false, key_w});
            kit.card_end("##card_about");
            content_h_ = kit.cards_height();
        }
    }
    return changed;
}

} // namespace prism::ui
