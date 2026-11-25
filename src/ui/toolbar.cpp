#include "ui/toolbar.hpp"

#include "core/path.hpp"
#include "core/recycle.hpp"
#include "core/shell_menu.hpp"
#include "core/shell_ops.hpp"
#include "core/text.hpp"
#include "ui/menu_parts.hpp"
#include "ui/new_menu.hpp"
#include "ui/panel.hpp"
#include "ui/tr.hpp"

#include <algorithm>
#include <cmath>
#include <thread>

namespace prism::ui {

using namespace strata;

bool Toolbar::icon_button(Painter& p, std::string_view id, char32_t g, bool enabled, bool on, std::string_view tip, float size)
{
    context&       ui  = p.ui();
    const Palette& pal = p.pal();
    const item_result r = ui.custom_item(id, {size, size});
    const bool hot = enabled && r.hovered;
    const float hk = ui.animate(std::string{id} + "h", hot ? 1.0f : 0.0f, 18.0f);
    const float ok = ui.animate(std::string{id} + "o", on ? 1.0f : 0.0f, 14.0f);
    const rect  face = r.bounds.expanded(-1.0f);
    if (ok > 0.01f) { p.dl().rect_filled(face, pal.accent.scaled_alpha(0.18f * ok), 9.0f); }
    if (hk > 0.01f) { p.dl().rect_filled(face, (r.held ? pal.pressed : pal.hover).scaled_alpha(hk), 9.0f); }
    p.glyph_center(r.bounds, g, !enabled ? pal.text_faint : (on ? pal.accent : pal.text));
    if (r.hovered && !tip.empty()) { ui.tooltip(tip); }
    return hot && ui.mouse_released(mouse_button::left);
}

void Toolbar::field_background(Painter& p, const rect& r, bool invalid)
{
    shape_style s;
    s.radius       = radii(8.0f);
    s.fill_top     = s.fill_bottom = p.pal().raised;
    s.border       = invalid ? p.pal().danger : p.pal().border;
    s.border_width = 1.0f;
    p.dl().shape(r, s);
}

// --- filter / search box -----------------------------------------------------------------------------------------

namespace {

struct HelpRow {
    const char* example;
    const char* what;
};

constexpr HelpRow help_rows[] = {
    {"report 2026",       N_("all words must match")},
    {"\"annual report\"", N_("an exact phrase")},
    {"*.cpp",             N_("wildcards: * any text, ? one character")},
    {"report?.txt",       N_("a wildcard pattern matches the whole name")},
    {".png | .jpg",       N_("either one  (| or OR)")},
    {"-draft",            N_("leave out names containing draft")},
    {"ext:cpp,h",         N_("by extension  (-ext:obj to exclude)")},
    {"type:image",        N_("image video audio doc code archive text pdf exe font ...")},
    {"kind:folder",       N_("only folders  (kind:file: only files)")},
    {"size:>10mb",        N_("also <, >=, 1kb..1mb  (b kb mb gb)")},
    {"modified:today",    N_("yesterday week month year 3d 2026-10 >2026-01-01")},
    {"hidden:yes",        N_("hidden items (they are shown for this query)")},
    {"re:^IMG_\\d+",      N_("a regular expression on the name")},
    {"src\\*.h",          N_("a path: only in Subfolders results")},
};

} // namespace

core::QueryOptions Toolbar::query_options() const
{
    core::QueryOptions o;
    o.case_sensitive = env_.settings.search_case;
    o.regex          = env_.settings.search_regex;
    return o;
}

void Toolbar::apply_filter(model::Tab& tab, bool run)
{
    const core::Query q = core::Query::parse(filter_text_, query_options());
    if (!env_.settings.search_subfolders) {
        if (tab.searching()) { tab.clear_search(); } // back to the folder; the filter applies to its listing as it loads
        tab.listing().set_query(q);
        return;
    }
    // (an invalid query only carries its error to the status bar: it filters nothing)
    tab.listing().set_query(q.valid() ? core::Query{} : q);
    if (run) {
        if (q.empty() && q.valid()) { tab.clear_search(); } else { tab.search(q); }
    } else if (tab.searching() && q.empty() && q.valid()) {
        tab.clear_search();
    }
}

bool Toolbar::scope_control(Painter& p, float half, float height)
{
    context&       ui  = p.ui();
    const Palette& pal = p.pal();
    const bool     sub = env_.settings.search_subfolders;

    const item_result r = ui.custom_item("##scope", {half * 2.0f + 6.0f, height});
    const rect box = r.bounds;
    shape_style track;
    track.radius       = radii(8.0f);
    track.fill_top     = track.fill_bottom = pal.raised;
    track.border       = pal.border;
    track.border_width = 1.0f;
    p.dl().shape(box, track);

    const float t     = ui.animate("scope_thumb", sub ? 1.0f : 0.0f, 16.0f);
    const float thumb_x = box.min.x + 3.0f + t * half;
    const rect  thumb{{thumb_x, box.min.y + 3.0f}, {thumb_x + half, box.max.y - 3.0f}};
    p.dl().rect_filled(thumb, pal.accent.scaled_alpha(0.22f), 6.0f);
    p.dl().rect_outline(thumb, pal.accent.scaled_alpha(0.55f), 6.0f, 1.0f);

    const int over = r.hovered ? (ui.mouse_pos().x < box.min.x + 3.0f + half ? 0 : 1) : -1;
    const rect left{{box.min.x + 3.0f, box.min.y}, {box.min.x + 3.0f + half, box.max.y}};
    const rect right{{box.min.x + 3.0f + half, box.min.y}, {box.min.x + 3.0f + 2.0f * half, box.max.y}};
    const color on_col  = pal.text;
    const color off_col = pal.text_dim;
    p.glyph_center(left, glyph::folder, !sub ? on_col : (over == 0 ? on_col : off_col));
    // subfolders: a small tree (a parent line, two indented children joined by a stem)
    {
        const color c = sub ? on_col : (over == 1 ? on_col : off_col);
        const vec2  m = right.center();
        p.dl().rect_filled(rect{{m.x - 8.0f, m.y - 6.0f}, {m.x + 8.0f, m.y - 4.5f}}, c, 0.75f);
        p.dl().rect_filled(rect{{m.x - 4.0f, m.y - 6.0f}, {m.x - 2.5f, m.y + 5.0f}}, c, 0.75f);
        p.dl().rect_filled(rect{{m.x - 4.0f, m.y - 0.75f}, {m.x + 8.0f, m.y + 0.75f}}, c, 0.75f);
        p.dl().rect_filled(rect{{m.x - 4.0f, m.y + 4.0f}, {m.x + 8.0f, m.y + 5.5f}}, c, 0.75f);
    }
    if (over >= 0) {
        ui.tooltip(over == 0 ? tr("This folder: filter what is shown, as you type") : tr("Subfolders: search everything below this folder"));
        env_.cmd.set_cursor(cursor_kind::hand);
    }
    if (over >= 0 && ui.mouse_released(mouse_button::left) && (over == 1) != sub) {
        env_.settings.search_subfolders = over == 1;
        return true;
    }
    return false;
}

bool Toolbar::option_chip(Painter& p, std::string_view id, std::string_view label, bool on, std::string_view tip, float w, float h)
{
    context&       ui  = p.ui();
    const Palette& pal = p.pal();
    const item_result r = ui.custom_item(id, {w, h});
    const float hk = ui.animate(std::string{id} + "h", r.hovered ? 1.0f : 0.0f, 18.0f);
    const rect  pill = rect{{r.bounds.min.x + 1.0f, r.bounds.min.y + 6.0f}, {r.bounds.max.x - 1.0f, r.bounds.max.y - 6.0f}};
    if (on) { p.dl().rect_filled(pill, pal.accent.scaled_alpha(0.24f), 6.0f); }
    if (hk > 0.01f) { p.dl().rect_filled(pill, (r.held ? pal.pressed : pal.hover).scaled_alpha(hk), 6.0f); }
    p.text_center(pill, label, on ? pal.accent : pal.text_dim, env_.fonts.bold);
    if (r.hovered && !tip.empty()) { ui.tooltip(tip); }
    return r.hovered && ui.mouse_released(mouse_button::left);
}

void Toolbar::help_popup(Painter& p, model::Tab& tab)
{
    context&       ui  = p.ui();
    const Palette& pal = p.pal();
    const FontSet& f   = env_.fonts;
    auto pop = ui.popup("##search_help", 560.0f);
    if (!pop) { return; }

    const float row_h = 28.0f;
    const item_result title = ui.custom_item("##help_title", {540.0f, 26.0f});
    p.text_left(title.bounds, tr("Search and filter"), pal.text, f.bold, 8.0f);
    for (std::size_t i = 0; i < std::size(help_rows); ++i) {
        const item_result r = ui.custom_item("##help" + std::to_string(i), {540.0f, row_h});
        if (r.hovered) { p.dl().rect_filled(r.bounds, pal.hover, 6.0f); }
        p.text_left(rect{r.bounds.min, {r.bounds.min.x + 190.0f, r.bounds.max.y}}, help_rows[i].example, pal.accent, f.body, 8.0f);
        p.text_left(rect{{r.bounds.min.x + 192.0f, r.bounds.min.y}, r.bounds.max}, tr(help_rows[i].what), pal.text_dim, f.small, 4.0f);
        if (r.hovered && ui.mouse_released(mouse_button::left)) {
            filter_text_ = help_rows[i].example;
            apply_filter(tab, env_.settings.search_subfolders);
            ui.close_popup();
        }
    }
    {
        // keep what you typed: it becomes a sidebar entry that reopens this folder and runs the search
        const item_result r = ui.custom_item("##help_save", {540.0f, row_h});
        if (r.hovered) { p.dl().rect_filled(r.bounds, pal.hover, 6.0f); env_.cmd.set_cursor(cursor_kind::hand); }
        p.text_left(r.bounds, trf("Save this search to the sidebar  ({})", key_hint(env_.binds, "save_search")), pal.accent, f.bold, 8.0f);
        if (r.hovered && ui.mouse_released(mouse_button::left)) {
            env_.cmd.request_action("save_search");
            ui.close_popup();
        }
    }
    const rect foot = ui.custom_item("##help_foot", {540.0f, 56.0f}).bounds;
    p.text_left(rect{foot.min, {foot.max.x, foot.min.y + 18.0f}}, tr("This folder filters the list as you type"), pal.text_dim, f.small, 8.0f);
    p.text_left(rect{{foot.min.x, foot.min.y + 18.0f}, {foot.max.x, foot.min.y + 36.0f}}, tr("Subfolders searches everything below it: press Enter  (Ctrl+Enter from This folder)"), pal.text_dim, f.small, 8.0f);
    p.text_left(rect{{foot.min.x, foot.min.y + 36.0f}, {foot.max.x, foot.max.y}}, tr("Aa matches case, .* reads plain words as regular expressions, Esc clears"), pal.text_dim, f.small, 8.0f);
}

std::string Toolbar::drop_target_at(vec2 p) const
{
    for (const CrumbRect& c : crumb_rects_) {
        if (c.rect.contains(p)) { return c.path; }
    }
    return {};
}

void Toolbar::address(Painter& p, float area_x, float area_y, float width, float height)
{
    context&       ui  = p.ui();
    const Palette& pal = p.pal();
    const FontSet& f   = env_.fonts;
    model::Tab&    tab = env_.tab();
    crumb_rects_.clear();

    if (want_edit_) {
        want_edit_ = false;
        editing_   = true;
        edit_age_  = 0;
        edit_text_ = tab.path();
    }

    if (editing_) {
        // poll finished autocomplete directory listing
        if (ac_load_ && ac_load_->done.load(std::memory_order_acquire)) {
            if (ac_load_->dir == ac_dir_) { ac_dirs_ = std::move(ac_load_->dirs); }
            ac_load_.reset();
        }
        // the field is a strata widget: sized to the row by its padding
        if (edit_age_ < 3) { ui.request_text_focus("##address"); }
        const rect field_box = rect::from_size({area_x, area_y}, {width, height});
        field_background(p, field_box);
        ui.set_next_item_width(width);
        ui.push_var(style_var::frame_padding_y, (height - p.line_height(f.body)) * 0.5f);
        ui.push_color(style_color::widget_bg, color{0, 0, 0, 0});
        const bool text_changed = ui.input_text("##address", edit_text_, tr("Type a path, %ENV%, shell:... or a command"));
        ui.pop_color();
        ui.pop_var();
        ++edit_age_;

        // split typed path into parent folder + partial child prefix for async autocomplete
        std::string ac_parent, ac_prefix;
        {
            const std::size_t sep = edit_text_.find_last_of("\\/");
            if (sep != std::string::npos) {
                ac_parent = core::path::normalize(std::string_view{edit_text_}.substr(0, sep + 1));
                ac_prefix = edit_text_.substr(sep + 1);
            }
        }
        if (ac_parent != ac_dir_) {
            ac_dir_ = ac_parent;
            ac_dirs_.clear();
            ac_sel_ = -1;
            if (!ac_dir_.empty() && ((ac_dir_.size() >= 2 && ac_dir_[1] == ':') || ac_dir_.starts_with("\\\\"))) {
                auto job = std::make_shared<AcLoad>();
                job->dir = ac_dir_;
                ac_load_ = job;
                std::thread{[job, wake = env_.window.waker()] {
                    job->dirs = core::path::subfolders(job->dir, 200);
                    job->done.store(true, std::memory_order_release);
                    if (wake) { wake(); }
                }}.detach();
            }
        }
        if (text_changed) { ac_sel_ = -1; }

        std::vector<std::string> matches;
        if (!ac_dirs_.empty()) {
            for (const std::string& d : ac_dirs_) {
                if (ac_prefix.empty() || core::text::starts_with_icase(d, ac_prefix)) {
                    matches.push_back(d);
                    if (matches.size() >= 8) { break; }
                }
            }
        }

        // Tab completes the selected (or first) match; Down / Up cycles through matches
        if (!matches.empty()) {
            if (ui.key_pressed(key::down)) { ac_sel_ = (ac_sel_ + 1) % static_cast<int>(matches.size()); }
            if (ui.key_pressed(key::up))   { ac_sel_ = ac_sel_ <= 0 ? static_cast<int>(matches.size()) - 1 : ac_sel_ - 1; }
            if (ui.key_pressed(key::tab)) {
                const int pick = ac_sel_ >= 0 ? ac_sel_ : 0;
                edit_text_ = core::path::join(ac_dir_, matches[static_cast<std::size_t>(pick)]) + "\\";
                ac_sel_    = -1;
                ui.request_text_focus("##address", edit_text_.size(), edit_text_.size());
            }
        }

        // sleek floating autocomplete dropdown beneath the address bar
        bool over_ac = false;
        if (!matches.empty()) {
            const float row_h = 28.0f;
            const float drop_h = static_cast<float>(matches.size()) * row_h + 8.0f;
            const rect  drop{{field_box.min.x, field_box.max.y + 4.0f}, {field_box.max.x, field_box.max.y + 4.0f + drop_h}};
            const vec2  mp = ui.mouse_pos();
            over_ac = drop.contains(mp);
            auto fg = ui.layer(layer::foreground);
            p.dl().rect_filled(drop.expanded(2.0f), color{0, 0, 0, 75}, 10.0f);
            shape_style ds;
            ds.radius       = radii(8.0f);
            ds.fill_top     = ds.fill_bottom = pal.raised;
            ds.border       = pal.border;
            ds.border_width = 1.0f;
            p.dl().shape(drop, ds);
            for (std::size_t i = 0; i < matches.size(); ++i) {
                const rect rr{{drop.min.x + 4.0f, drop.min.y + 4.0f + static_cast<float>(i) * row_h},
                              {drop.max.x - 4.0f, drop.min.y + 4.0f + static_cast<float>(i + 1) * row_h}};
                const bool hot = rr.contains(mp);
                const bool sel = static_cast<int>(i) == ac_sel_;
                if (hot || sel) {
                    p.dl().rect_filled(rr, sel ? pal.accent.scaled_alpha(0.22f) : pal.hover, 6.0f);
                    if (hot) { env_.cmd.set_cursor(cursor_kind::hand); }
                }
                p.glyph_center(rect{{rr.min.x + 6.0f, rr.min.y}, {rr.min.x + 24.0f, rr.max.y}}, glyph::folder, pal.accent, f.tile);
                p.text_left(rect{{rr.min.x + 28.0f, rr.min.y}, {rr.max.x - 8.0f, rr.max.y}}, matches[i], pal.text, f.body);
                if (hot && ui.mouse_clicked(mouse_button::left)) {
                    const std::string chosen = core::path::join(ac_dir_, matches[i]);
                    tab.navigate(chosen);
                    editing_ = false;
                    check_.reset();
                    return;
                }
            }
        }

        if (ui.key_pressed(key::enter) && check_ == nullptr) {
            if (ac_sel_ >= 0 && ac_sel_ < static_cast<int>(matches.size())) {
                tab.navigate(core::path::join(ac_dir_, matches[static_cast<std::size_t>(ac_sel_)]));
                editing_ = false;
                return;
            }
            check_target_ = core::path::resolve_input(edit_text_);
            check_        = std::make_shared<PathCheck>();
            std::thread{[state = check_, target = check_target_, wake = env_.window.waker()] {
                state->state.store(static_cast<int>(core::path::kind(target)));
                if (wake) { wake(); }
            }}.detach();
        } else if (check_ != nullptr && check_->state.load() >= 0) {
            const auto found = static_cast<core::path::Kind>(check_->state.load());
            check_.reset();
            if (found == core::path::Kind::folder) {
                tab.navigate(check_target_);
                editing_ = false;
            } else if (found == core::path::Kind::file) {
                // a file: show the folder it is in with the file selected
                tab.reveal_name = core::path::leaf(check_target_);
                tab.navigate(core::path::parent(check_target_));
                editing_ = false;
            } else if (!tab.path().empty()) {
                // not a place: treat it as a command to run here ("cmd", "code .", "pwsh")
                if (core::shell::run_command(edit_text_, tab.path())) {
                    editing_ = false;
                } else {
                    env_.cmd.toast(trf("Not a folder or a command: {}", edit_text_), toast_kind::error);
                }
            } else {
                env_.cmd.toast(trf("Not a folder: {}", edit_text_), toast_kind::error);
            }
        } else if (ui.key_pressed(key::escape) || (edit_age_ >= 3 && !ui.field_focused("##address") && !over_ac)) {
            editing_ = false;
            check_.reset();
        }
        return;
    }

    const item_result r = ui.custom_item("##crumbs", {width, height});
    const rect box = r.bounds;
    shape_style bg;
    bg.radius       = radii(8.0f);
    bg.fill_top     = bg.fill_bottom = pal.raised;
    bg.border       = r.hovered ? pal.text_faint.scaled_alpha(0.6f) : pal.border;
    bg.border_width = 1.0f;
    p.dl().shape(box, bg);

    struct Seg { std::string name; std::string path; float w; };
    std::vector<Seg> segs;
    const auto add_seg = [&](std::string name, std::string path) {
        const float w = p.measure(f.body, name).x + 16.0f;
        segs.push_back({std::move(name), std::move(path), w});
    };
    const auto view = env_.views.find(tab.path());
    const auto root = view ? env_.views.root_for(view->archive) : std::nullopt;
    if (view && root) {
        // inside an archive: "C:\ > folder > photos.zip > sub" instead of the temporary folder it was unpacked into
        for (auto& s : core::path::segments(core::path::parent(view->archive))) { add_seg(std::move(s.name), std::move(s.path)); }
        add_seg(core::path::leaf(view->archive), *root);
        std::string at = *root;
        std::size_t from = 0;
        while (from < view->inner.size()) {
            std::size_t to = view->inner.find('\\', from);
            if (to == std::string::npos) { to = view->inner.size(); }
            const std::string part = view->inner.substr(from, to - from);
            at = core::path::join(at, part);
            add_seg(part, at);
            from = to + 1;
        }
    } else {
        auto parts = core::path::segments(tab.path());
        if (tab.path() == core::recycle::all) { parts.clear(); add_seg(std::string{tr("Recycle Bin")}, tab.path()); }
        // the Recycle Bin:"Recycle Bin > ..." instead of "C:\ > $Recycle.Bin > S-1-5-21-..."
        const auto bin = std::ranges::find_if(parts, [](const auto& s) { return core::text::lower_ascii(s.name) == "$recycle.bin"; });
        if (bin != parts.end() && std::next(bin) != parts.end()) {
            for (auto it = parts.begin(); it != bin; ++it) { add_seg(std::move(it->name), std::move(it->path)); } // ("C:\")
            add_seg(std::string{tr("Recycle Bin")}, std::next(bin)->path);
            parts.erase(parts.begin(), bin + 2);
        }
        for (auto& s : parts) { add_seg(std::move(s.name), std::move(s.path)); }
    }
    const float chev = 16.0f;
    const float avail = width - 16.0f;
    // drop leading segments until the rest fits; a "..." takes their place
    std::size_t first = 0;
    const auto total = [&](std::size_t from) {
        float t = from > 0 ? 28.0f : 0.0f;
        for (std::size_t i = from; i < segs.size(); ++i) { t += segs[i].w + (i + 1 < segs.size() ? chev : 0.0f); }
        return t;
    };
    while (first + 1 < segs.size() && total(first) > avail) { ++first; }

    p.dl().push_clip(box);
    float x = box.min.x + 8.0f;
    const vec2 mp = ui.mouse_pos();
    bool over_segment = false;
    if (first > 0) {
        p.text_center(rect{{x, box.min.y}, {x + 28.0f, box.max.y}}, "...", pal.text_dim, f.body);
        x += 28.0f;
    }
    for (std::size_t i = first; i < segs.size(); ++i) {
        const rect cell{{x, box.min.y + 4.0f}, {x + segs[i].w, box.max.y - 4.0f}};
        const bool hot = r.hovered && cell.contains(mp);
        crumb_rects_.push_back({cell, segs[i].path});
        if (drop_ && cell.contains(*drop_)) { p.dl().rect_outline(cell, pal.accent, 6.0f, 2.0f); }
        if (hot) {
            over_segment = true;
            p.dl().rect_filled(cell, r.held ? pal.pressed : pal.hover, 6.0f);
            if (ui.mouse_clicked(mouse_button::left)) { tab.navigate(segs[i].path); }
            if (ui.mouse_clicked(mouse_button::middle)) { env_.cmd.open_tab(segs[i].path, false); }
            if (ui.mouse_clicked(mouse_button::right)) {
                crumb_ctx_path_  = segs[i].path;
                crumb_ctx_paste_ = core::clipboard_has_files();
                ui.open_popup_menu("##crumb_ctx", ui.mouse_pos());
            }
        }
        p.text_center(cell, segs[i].name, i + 1 == segs.size() ? pal.text : pal.text_dim, f.body);
        x += segs[i].w;
        if (i + 1 < segs.size()) {
            // the chevron opens the folders next to the next segment (siblings of where the path goes on)
            const rect cc{{x, box.min.y + 4.0f}, {x + chev, box.max.y - 4.0f}};
            const bool chev_hot = r.hovered && cc.contains(mp);
            if (chev_hot) {
                over_segment = true;
                p.dl().rect_filled(cc, r.held ? pal.pressed : pal.hover, 6.0f);
                env_.cmd.set_cursor(cursor_kind::hand);
                if (ui.mouse_clicked(mouse_button::left)) {
                    crumb_parent_  = segs[i].path;
                    crumb_next_    = segs[i + 1].name;
                    crumb_dirs_.clear();
                    crumb_loading_ = true;
                    auto job = std::make_shared<CrumbLoad>();
                    crumb_load_ = job;
                    std::thread{[job, dir = crumb_parent_, wake = env_.window.waker()] {
                        job->dirs = core::path::subfolders(dir, 400);
                        job->done.store(true, std::memory_order_release);
                        if (wake) { wake(); }
                    }}.detach();
                    ui.open_popup_menu("##crumb_menu", {cc.min.x, box.max.y + 4.0f});
                }
            }
            p.glyph_center(cc, glyph::chevron_r, chev_hot ? pal.text : pal.text_faint, f.tile);
            x += chev;
        }
    }
    p.dl().pop_clip();

    if (r.hovered && !over_segment && ui.mouse_clicked(mouse_button::left)) { want_edit_ = true; }
    crumb_menu(p);
    crumb_context_menu(p);
}

// right click on a segment of the path: the same things the folder's row would offer, so there is no need to find it in
// the list (or an empty spot in it)
void Toolbar::crumb_context_menu(Painter& p)
{
    context& ui = p.ui();
    const MenuDensity density{ui, env_.settings.compact};
    shell_page_.begin_frame();
    auto menu = ui.popup_menu("##crumb_ctx");
    if (!menu) { return; }
    const std::string& dir = crumb_ctx_path_;
    model::Tab&        tab = env_.tab();

    const bool in_bin = core::recycle::inside(dir);
    if (ui.menu_item(tr("Open"))) { tab.navigate(dir); }
    menus::open_in(ui, env_, dir);
    ui.menu_separator();
    if (in_bin) {
        if (ui.menu_item(tr("Empty Recycle Bin"))) { env_.cmd.empty_recycle_bin(core::recycle::drive_of(dir)); }
        return;
    }
    if (ui.menu_item(tr("New folder here"))) {
        const std::string name = core::FileOps::create_folder(dir);
        if (name.empty()) {
            env_.cmd.toast(tr("Could not create a folder there"), toast_kind::error);
        } else {
            const std::string made = core::path::join(dir, name);
            env_.history.push({trf("New folder {}", name), [made] { return core::FileOps::remove_empty_folder(made); }, {}});
            env_.cmd.toast(trf("Created {}", name), toast_kind::success);
            if (core::path::normalize(tab.path()) == core::path::normalize(dir)) { tab.reveal_name = name; }
            tab.refresh();
        }
    }
    const bool is_root = core::path::is_root(dir); // drive roots ("C:\") and UNC share roots ("\\server\share")
    if (!is_root && ui.menu_item(tr("Cut"), env_.binds.text("cut"))) { (void)core::clipboard_set_files({dir}, true); }
    if (ui.menu_item(tr("Copy"), env_.binds.text("copy"))) { (void)core::clipboard_set_files({dir}, false); }
    menus::paste_into(ui, env_, dir, crumb_ctx_paste_);
    if (!is_root && ui.menu_item(tr("Delete"), "Del")) { env_.cmd.request_delete({dir}, false); }
    ui.menu_separator();
    menus::terminal(ui, env_, dir);
    ui.menu_separator();
    menus::path_items(ui, env_, {dir});
    menus::pin_item(ui, env_, dir);
    ui.menu_separator();
    menus::properties_item(ui, env_, {dir});
    menus::more_options(ui, env_, shell_page_, shell_menu_, {dir}, false);
    menus::windows_items(ui, env_, {dir});
}

void Toolbar::history_menu(Painter& p, model::Tab& tab)
{
    context& ui = p.ui();
    const MenuDensity density{ui, env_.settings.compact};
    auto menu = ui.popup_menu("##hist_menu");
    if (!menu) { return; }
    const auto list = hist_forward_ ? tab.forward_history() : tab.back_history();
    if (list.empty()) { return; }
    char icon[5];
    const std::size_t cap = std::min<std::size_t>(list.size(), 16);
    for (std::size_t i = 0; i < cap; ++i) {
        const auto& [idx, path] = list[i];
        std::string label = path == core::recycle::all ? std::string{tr("Recycle Bin")} : core::path::leaf(path);
        if (label.empty()) { label = path; }
        for (std::size_t k = 0; (k = label.find('&', k)) != std::string::npos; k += 2) { label.insert(k, 1, '&'); }
        menu_item_options o;
        o.icon      = glyph_utf8(glyph::folder, icon);
        o.icon_font = env_.fonts.icons;
        if (ui.menu_item(label + "##h" + std::to_string(idx), o)) { (void)tab.jump_history(idx); }
    }
}

void Toolbar::crumb_menu(Painter& p)
{
    if (crumb_load_ && crumb_load_->done.load(std::memory_order_acquire)) {
        crumb_dirs_    = std::move(crumb_load_->dirs);
        crumb_load_.reset();
        crumb_loading_ = false;
    }
    context& ui = p.ui();
    const MenuDensity density{ui, env_.settings.compact};
    auto menu = ui.popup_menu("##crumb_menu");
    if (!menu) { return; }
    if (crumb_loading_) {
        menu_item_options o;
        o.enabled = false;
        (void)ui.menu_item(tr("Loading ..."), o);
        return;
    }
    if (crumb_dirs_.empty()) {
        menu_item_options o;
        o.enabled = false;
        (void)ui.menu_item(tr("No subfolders"), o);
        return;
    }
    char icon[5];
    for (const std::string& name : crumb_dirs_) {
        menu_item_options o;
        o.icon      = glyph_utf8(glyph::folder, icon);
        o.icon_font = env_.fonts.icons;
        o.selected  = name == crumb_next_;
        if (ui.menu_item(name, o)) { env_.tab().navigate(core::path::join(crumb_parent_, name)); }
    }
}

void Toolbar::frame(Painter& p, const rect& area, bool show_hidden)
{
    context&       ui  = p.ui();
    const Palette& pal = p.pal();
    const Metrics& m   = p.metrics();
    const FontSet& f   = env_.fonts;
    model::TabSet& tabs = env_.tabs;
    (void)tabs;
    model::Tab&    tab  = env_.tab();

    // the filter follows the tab: another tab, or this one navigated
    if (last_tab_ != &tab || last_generation_ != tab.generation) {
        last_tab_        = &tab;
        last_generation_ = tab.generation;
        filter_text_     = tab.searching() ? tab.search_query() : tab.listing().query().text();
        editing_         = false;
        check_.reset();
    }

    PanelScope panel{ui, "##toolbar", area};
    p.dl().rect_filled(area, pal.title_bar);

    const float gap = m.gap;
    const float rh  = m.toolbar - 2.0f * gap;
    const float chip_w   = 26.0f;
    const float half     = rh * 0.85f + 6.0f; // two icon buttons: This folder / Subfolders (tooltips say which)
    const float scope_w  = half * 2.0f + 6.0f;
    const float filter_w = std::clamp(area.width() * 0.13f, 110.0f, 190.0f) + chip_w * 3.0f; // (the chips sit inside the box)
    const float right_w  = rh * 3.0f + gap * 3.0f + 8.0f + scope_w + filter_w; // (+ the New button)
    const float left_w   = 12.0f + rh * 4.0f + 6.0f + gap * 2.0f;
    const float addr_w   = std::max(120.0f, area.width() - left_w - right_w - 10.0f);

    (void)ui.custom_item("##tb_vpad", {1.0f, gap});
    (void)ui.custom_item("##tb_lpad", {12.0f, rh});
    ui.same_line();
    const auto sp = [&](const char* id) { ui.same_line(); (void)ui.custom_item(id, {2.0f, rh}); ui.same_line(); };
    if (icon_button(p, "##nav_back", glyph::back, tab.can_back(), false, tr("Back  (Alt+Left, right-click for history)"), rh)) { (void)tab.back(); }
    if (tab.can_back() && ui.item_rect().contains(ui.mouse_pos()) && ui.mouse_clicked(mouse_button::right)) {
        hist_forward_ = false;
        const rect b = ui.item_rect();
        ui.open_popup_menu("##hist_menu", {b.min.x, b.max.y + 4.0f});
    }
    sp("##sp1");
    if (icon_button(p, "##nav_fwd", glyph::forward, tab.can_forward(), false, tr("Forward  (Alt+Right, right-click for history)"), rh)) { (void)tab.forward(); }
    if (tab.can_forward() && ui.item_rect().contains(ui.mouse_pos()) && ui.mouse_clicked(mouse_button::right)) {
        hist_forward_ = true;
        const rect b = ui.item_rect();
        ui.open_popup_menu("##hist_menu", {b.min.x, b.max.y + 4.0f});
    }
    history_menu(p, tab);
    sp("##sp2");
    if (icon_button(p, "##nav_up", glyph::up, tab.can_up(), false, "Up  (Alt+Up)", rh)) { env_.cmd.go_up(tab); }
    sp("##sp3");
    if (icon_button(p, "##nav_refresh", glyph::refresh, true, false, "Refresh  (F5)", rh)) { tab.refresh(); }
    ui.same_line();
    (void)ui.custom_item("##tb_gap1", {gap * 2.0f, rh});
    ui.same_line();

    address(p, area.min.x + left_w, area.min.y + gap, addr_w, rh);
    ui.same_line();
    (void)ui.custom_item("##tb_gap2", {gap, rh});
    ui.same_line();

    // filter field (a strata widget, sized to the row). strata darkens a field's fill by a fixed amount, which turns grey
    // on the light theme: the fill is made transparent and the background drawn here, where the layout puts the field
    Settings& st = env_.settings;
    if (scope_control(p, half, rh)) {
        env_.cmd.settings_changed();
        apply_filter(tab, !filter_text_.empty()); // (to Subfolders: search at once; back to This folder: filter live)
    }
    ui.same_line();
    (void)ui.custom_item("##tb_gap2b", {gap, rh});
    ui.same_line();

    const float filter_x = area.min.x + left_w + addr_w + gap + scope_w + gap;
    const bool  invalid  = !tab.listing().query().valid();
    field_background(p, rect::from_size({filter_x, area.min.y + gap}, {filter_w, rh}), invalid);
    ui.set_next_item_width(filter_w - chip_w * 3.0f);
    ui.push_var(style_var::frame_padding_y, (rh - p.line_height(f.body)) * 0.5f);
    ui.push_color(style_color::widget_bg, color{0, 0, 0, 0});
    if (want_filter_focus_) {
        want_filter_focus_ = false;
        ui.request_text_focus("##filter");
    }
    const bool typed_in_filter = ui.input_text("##filter", filter_text_, st.search_subfolders ? tr("Search subfolders") : tr("Filter this folder"));
    const bool filter_focus = ui.field_focused("##filter");
    ui.pop_color();
    ui.pop_var();
    if (typed_in_filter) { apply_filter(tab, false); }
    if (filter_focus && ui.key_pressed(key::enter, true)) {
        if (!st.search_subfolders) {
            st.search_subfolders = true;
            env_.cmd.settings_changed();
        }
        apply_filter(tab, true);
    } else if (filter_focus && ui.key_pressed(key::enter) && st.search_subfolders) {
        apply_filter(tab, true);
    }
    if (filter_focus && ui.key_pressed(key::escape)) {
        filter_text_.clear();
        apply_filter(tab, false);
    }
    ui.same_line();
    bool options_changed = false;
    if (option_chip(p, "##opt_case", "Aa", st.search_case, "Match case", chip_w, rh)) { st.search_case = !st.search_case; options_changed = true; }
    ui.same_line();
    if (option_chip(p, "##opt_regex", ".*", st.search_regex, "Regular expressions", chip_w, rh)) { st.search_regex = !st.search_regex; options_changed = true; }
    ui.same_line();
    if (option_chip(p, "##opt_help", "?", ui.popup_open("##search_help"), "Search syntax", chip_w, rh)) { ui.toggle_popup("##search_help"); }
    if (options_changed) {
        env_.cmd.settings_changed();
        apply_filter(tab, st.search_subfolders && tab.searching());
    }
    help_popup(p, tab);
    ui.same_line();
    (void)ui.custom_item("##tb_gap3", {gap, rh});
    ui.same_line();
    // New: a folder or an empty file here, for when the folder is too full to find empty space to right-click
    const bool can_create = !tab.path().empty() && !tab.searching();
    if (icon_button(p, "##tb_new", glyph::add, can_create, ui.popup_open("##new_menu"), "New folder or file", rh)) {
        const rect b = ui.item_rect();
        ui.open_popup_menu("##new_menu", {b.min.x, b.max.y + 4.0f});
    }
    new_menu(p, tab);
    ui.same_line();
    (void)ui.custom_item("##tb_gap4", {2.0f, rh});
    ui.same_line();

    // everything about how the folder is shown lives in one menu; it lights up when something in it is switched on
    const Settings& cur = env_.settings;
    const bool view_on  = cur.grid_view || cur.split || cur.preview || cur.compact || show_hidden;
    if (icon_button(p, "##tb_view", glyph::view, true, view_on, "View options", rh)) {
        const rect b = ui.item_rect();
        ui.open_popup_menu("##view_menu", {b.min.x, b.max.y + 4.0f});
    }
    view_menu(p, show_hidden);
    ui.same_line();
    if (icon_button(p, "##tb_settings", glyph::settings, true, false, "Settings  (Ctrl+,)", rh)) { env_.cmd.open_settings(); }
}

void Toolbar::new_menu(Painter& p, const model::Tab& tab)
{
    context& ui = p.ui();
    const MenuDensity density{ui, env_.settings.compact};
    auto menu = ui.popup_menu("##new_menu");
    if (!menu) { return; }

    char icons[3][5];
    int  n = 0;
    const auto item = [&](char32_t g, std::string_view label, std::string_view shortcut) {
        menu_item_options o;
        o.shortcut  = shortcut;
        o.icon      = glyph_utf8(g, icons[n++]);
        o.icon_font = env_.fonts.icons;
        return ui.menu_item(label, o);
    };
    if (item(glyph::folder, tr("Folder"), env_.binds.text("new_folder"))) { env_.cmd.request_action("new_folder"); }
    if (item(glyph::document, tr("Empty file"), "")) { env_.cmd.request_action("new_file"); }
    if (item(glyph::document, tr("Text document"), "")) { env_.cmd.request_action("new_text"); }

    // what Windows' own "New" menu has (Shortcut, Bitmap image, Word document ...): the same entries, run by the shell
    if (env_.settings.shell_menu && !tab.path().empty() && !tab.searching()) {
        shell_menu_.query({tab.path()}, true);
        const std::vector<core::ShellMenuItem> all = shell_menu_.items();
        const auto windows_new = std::ranges::find_if(all, [](const core::ShellMenuItem& i) { return i.label == "New" && !i.children.empty(); });
        if (windows_new != all.end()) {
            bool first = true;
            for (const core::ShellMenuItem& c : windows_new->children) {
                if (c.separator) { continue; }
                if (c.label == "Folder" || c.label == "Text Document") { continue; } // (ours are above)
                if (first) { ui.menu_separator(); first = false; }
                std::string label; // ('&' marks a mnemonic in strata menus)
                for (const char ch : c.label) {
                    label.push_back(ch);
                    if (ch == '&') { label.push_back('&'); }
                }
                core::text::note_codepoints(c.label);
                menu_item_options o;
                o.enabled = c.enabled;
                if (c.icon != nullptr) { o.image = env_.images.raw(c.icon->key, c.icon->w, c.icon->h, c.icon->bgra).id; }
                if (ui.menu_item(label + "##w" + std::to_string(c.id), o)) { shell_menu_.invoke(c.id, env_.window.handle()); }
            }
        }
    }
    ui.menu_separator();
    new_template_items(ui, env_);
}

void Toolbar::view_menu(Painter& p, bool show_hidden)
{
    context& ui = p.ui();
    const MenuDensity density{ui, env_.settings.compact};
    auto menu = ui.popup_menu("##view_menu");
    if (!menu) { return; }

    char icons[16][5];
    int  n = 0;
    // (the menu stays open on a click: switching a few things in a row is the common case)
    const auto item = [&](char32_t g, std::string_view label, std::string_view shortcut, bool on) {
        menu_item_options o;
        o.shortcut  = shortcut;
        o.icon      = glyph_utf8(g, icons[n++]);
        o.icon_font = env_.fonts.icons;
        o.selected  = on;
        o.keep_open = true;
        return ui.menu_item(label, o);
    };
    const Settings& st = env_.settings;
    if (item(glyph::list, tr("Details"), "", !st.grid_view) && st.grid_view) { env_.cmd.toggle_view(); }
    if (item(glyph::grid, tr("Thumbnails"), env_.binds.text("view_grid"), st.grid_view) && !st.grid_view) { env_.cmd.toggle_view(); }
    ui.menu_separator();
    if (item(glyph::rows, tr("Compact rows"), env_.binds.text("density"), st.compact)) { env_.cmd.toggle_density(); }
    if (item(glyph::split, tr("Two panes"), env_.binds.text("split"), st.split)) { env_.cmd.toggle_split(); }
    if (item(glyph::preview, tr("Preview pane"), env_.binds.text("preview"), st.preview)) { env_.cmd.toggle_preview(); }
    ui.menu_separator();
    if (item(glyph::eye, tr("Show hidden files"), env_.binds.text("hidden"), show_hidden)) { env_.cmd.toggle_hidden(); }
    if (item(glyph::drive, tr("Folder sizes"), "", st.folder_sizes)) {
        env_.settings.folder_sizes = !env_.settings.folder_sizes;
        env_.cmd.settings_changed();
    }
    if (item(glyph::code, tr("Git status"), "", st.git_badges)) {
        env_.settings.git_badges = !env_.settings.git_badges;
        env_.cmd.settings_changed();
    }
    ui.menu_separator();
    {
        menu_item_options o;
        o.icon      = glyph_utf8(glyph::copy, icons[n++]);
        o.icon_font = env_.fonts.icons;
        if (ui.menu_item(tr("Find duplicate files"), o)) { env_.cmd.request_action("find_duplicates"); }
    }
    {
        menu_item_options o;
        o.icon      = glyph_utf8(glyph::folder, icons[n++]);
        o.icon_font = env_.fonts.icons;
        if (ui.menu_item(tr("Find empty folders"), o)) { env_.cmd.request_action("empty_folders"); }
    }
    {
        menu_item_options o;
        const std::string sc = env_.binds.text("search_everywhere");
        o.shortcut  = sc;
        o.icon      = glyph_utf8(glyph::search, icons[n++]);
        o.icon_font = env_.fonts.icons;
        if (ui.menu_item(tr("Search everywhere"), o)) { env_.cmd.request_action("search_everywhere"); }
    }
    {
        menu_item_options o;
        const std::string sc = env_.binds.text("disk_usage");
        o.shortcut  = sc;
        o.icon      = glyph_utf8(glyph::grid, icons[n++]);
        o.icon_font = env_.fonts.icons;
        if (ui.menu_item(tr("Disk usage"), o)) { env_.cmd.request_action("disk_usage"); }
    }
    {
        menu_item_options o;
        const std::string sc = env_.binds.text("sessions");
        o.shortcut  = sc;
        o.icon      = glyph_utf8(glyph::folder_open, icons[n++]);
        o.icon_font = env_.fonts.icons;
        if (ui.menu_item(tr("Sessions"), o)) { env_.cmd.request_action("sessions"); }
    }
    {
        menu_item_options o;
        const std::string sc = env_.binds.text("compare_panes");
        o.shortcut  = sc;
        o.icon      = glyph_utf8(glyph::split, icons[n++]);
        o.icon_font = env_.fonts.icons;
        o.enabled   = st.split;
        if (ui.menu_item(tr("Compare panes"), o)) { env_.cmd.request_action("compare_panes"); }
    }
}

} // namespace prism::ui
