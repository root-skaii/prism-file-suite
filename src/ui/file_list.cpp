#include "ui/file_list.hpp"

#include "core/path.hpp"
#include "core/shell_ops.hpp"
#include "core/text.hpp"
#include "ui/panel.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <format>

namespace prism::ui {

using namespace strata;
using core::FileKind;
using model::SortKey;

namespace {

constexpr float k_pad      = 8.0f;   // inset of an item from the card edge
constexpr float k_min_name = 200.0f;

[[nodiscard]] std::string_view error_text(std::uint32_t code) noexcept
{
    switch (code) {
    case 2:
    case 3:  return "This folder no longer exists";
    case 5:  return "Access is denied";
    case 21: return "The device is not ready";
    case 53:
    case 67: return "The network location cannot be reached";
    default: return "This folder cannot be opened";
    }
}

struct Columns {
    float mod_x{}, type_x{}, size_x{}, end_x{};
};

// x of each column's left edge, from the right: name takes what is left
[[nodiscard]] Columns layout_columns(const std::array<float, 3>& w, float body_w) noexcept
{
    const float sum  = w[0] + w[1] + w[2];
    const float room = std::max(0.0f, body_w - k_min_name);
    const float k    = sum > room && sum > 0.0f ? room / sum : 1.0f;
    Columns c;
    c.end_x  = body_w;
    c.size_x = body_w - w[2] * k;
    c.type_x = c.size_x - w[1] * k;
    c.mod_x  = c.type_x - w[0] * k;
    return c;
}

[[nodiscard]] std::string plural(std::size_t n, std::string_view one, std::string_view many)
{
    return std::format("{} {}", n, n == 1 ? one : many);
}

// kinds whose icon is worth asking the shell for (their own icon / thumbnail), instead of the coloured tile
[[nodiscard]] bool shell_icon_kind(FileKind k) noexcept
{
    return k == FileKind::executable || k == FileKind::shortcut;
}

[[nodiscard]] bool thumbnail_kind(FileKind k) noexcept
{
    switch (k) {
    case FileKind::image: case FileKind::video: case FileKind::pdf: case FileKind::document:
    case FileKind::presentation: case FileKind::spreadsheet: case FileKind::executable: case FileKind::shortcut:
    case FileKind::font:
        return true;
    default:
        return false;
    }
}

} // namespace

// --- geometry ---------------------------------------------------------------------------------------------------

FileList::Geo FileList::make_geo(float body_w) const
{
    const Metrics& m = env_.theme.metrics();
    Geo g;
    g.body_w = body_w;
    g.grid   = env_.settings.grid_view;
    if (g.grid) {
        const bool compact = env_.theme.density() == Density::compact;
        g.cell_w  = compact ? 112.0f : 132.0f;
        g.cell_h  = compact ? 116.0f : 140.0f;
        g.per_row = std::max(1, static_cast<int>((body_w - 2.0f * k_pad) / g.cell_w));
    } else {
        g.cell_w  = body_w;
        g.cell_h  = m.row_height;
        g.per_row = 1;
    }
    return g;
}

rect FileList::cell_rect(const Geo& g, std::size_t i, vec2 origin) const
{
    if (!g.grid) {
        const float y = origin.y + static_cast<float>(i) * g.cell_h;
        return {{origin.x + k_pad, y + 1.0f}, {origin.x + g.body_w - k_pad, y + g.cell_h - 1.0f}};
    }
    const std::size_t col = i % static_cast<std::size_t>(g.per_row);
    const std::size_t row = i / static_cast<std::size_t>(g.per_row);
    const float left = origin.x + k_pad + (g.body_w - 2.0f * k_pad - static_cast<float>(g.per_row) * g.cell_w) * 0.5f;
    return rect::from_size({left + static_cast<float>(col) * g.cell_w + 3.0f, origin.y + static_cast<float>(row) * g.cell_h + 3.0f}, {g.cell_w - 6.0f, g.cell_h - 6.0f});
}

int FileList::index_at(const Geo& g, vec2 origin, vec2 p, std::size_t rows) const
{
    if (!g.grid) {
        const int r = static_cast<int>(std::floor((p.y - origin.y) / g.cell_h));
        return r >= 0 && static_cast<std::size_t>(r) < rows ? r : -1;
    }
    const float left = origin.x + k_pad + (g.body_w - 2.0f * k_pad - static_cast<float>(g.per_row) * g.cell_w) * 0.5f;
    const int c = static_cast<int>(std::floor((p.x - left) / g.cell_w));
    const int r = static_cast<int>(std::floor((p.y - origin.y) / g.cell_h));
    if (c < 0 || c >= g.per_row || r < 0) { return -1; }
    const std::size_t i = static_cast<std::size_t>(r) * static_cast<std::size_t>(g.per_row) + static_cast<std::size_t>(c);
    return i < rows ? static_cast<int>(i) : -1;
}

std::string FileList::drop_target_at(vec2 p) const
{
    if (shown_ == nullptr || !area_.contains(p)) { return {}; }
    const model::Listing& L = shown_->listing();
    if (viewport_.contains(p)) {
        const int i = index_at(geo_, origin_, p, L.rows());
        if (i >= 0 && L.at(static_cast<std::size_t>(i)).is_dir()) { return core::path::join(shown_->path(), L.at(static_cast<std::size_t>(i)).name); }
    }
    return shown_->path();
}

// --- navigation ---------------------------------------------------------------------------------------------

void FileList::open_row(model::Tab& tab, std::size_t row)
{
    const core::FileEntry& e = tab.listing().at(row);
    const std::string full = core::path::join(tab.path(), e.name);
    if (e.is_dir()) {
        tab.navigate(full);
    } else if (!core::shell::open(full)) {
        env_.cmd.toast("Could not open " + core::path::leaf(e.name), toast_kind::error);
    }
}

void FileList::move_focus(model::Tab& tab, std::size_t row, bool extend)
{
    model::Listing& L = tab.listing();
    if (L.rows() == 0) { return; }
    row = std::min(row, L.rows() - 1);
    if (extend) {
        const std::size_t anchor = L.anchor() == model::Listing::npos ? row : L.anchor();
        L.select_range(anchor, row, false);
        L.set_focus(row, false);
    } else {
        L.select_only(row);
        L.set_focus(row, true);
    }
    ensure_visible_ = true;
}

// --- file operations ----------------------------------------------------------------------------------------

std::vector<std::string> FileList::selected_paths(const model::Tab& tab) const
{
    std::vector<std::string> out;
    const model::Listing& L = tab.listing();
    for (const std::size_t r : L.selected_rows()) { out.push_back(core::path::join(tab.path(), L.at(r).name)); }
    return out;
}

bool FileList::is_cut(const model::Tab& tab, std::string_view name) const
{
    if (cut_names_.empty() || cut_dir_ != tab.path()) { return false; }
    return std::find(cut_names_.begin(), cut_names_.end(), name) != cut_names_.end();
}

void FileList::copy_selection(model::Tab& tab, bool cut)
{
    const std::vector<std::string> paths = selected_paths(tab);
    if (paths.empty()) { return; }
    if (!core::clipboard_set_files(paths, cut)) {
        env_.cmd.toast("Could not use the clipboard", toast_kind::error);
        return;
    }
    cut_names_.clear();
    cut_dir_.clear();
    if (cut) {
        cut_dir_ = tab.path();
        for (const std::size_t r : tab.listing().selected_rows()) { cut_names_.push_back(tab.listing().at(r).name); }
    }
    env_.cmd.toast(plural(paths.size(), "item", "items") + (cut ? " cut" : " copied"), toast_kind::info);
}

void FileList::paste(model::Tab& tab)
{
    std::vector<std::string> paths;
    bool cut = false;
    if (!core::clipboard_get_files(paths, cut)) { return; }
    env_.ops.start(cut ? core::OpKind::move : core::OpKind::copy, std::move(paths), tab.path());
    if (cut) {
        cut_names_.clear();
        cut_dir_.clear();
    }
}

void FileList::to_other_pane(model::Tab& tab, bool move)
{
    model::Tab* other = env_.other();
    if (other == nullptr || other->path().empty()) { return; }
    const std::vector<std::string> paths = selected_paths(tab);
    if (!paths.empty()) { env_.cmd.transfer(paths, other->path(), move); }
}

bool FileList::perform(context& ui, model::Tab& tab, std::string_view action)
{
    model::Listing& L = tab.listing();
    if (action == "copy")           { copy_selection(tab, false); return true; }
    if (action == "cut")            { copy_selection(tab, true); return true; }
    if (action == "paste")          { paste(tab); return true; }
    if (action == "delete")         { env_.cmd.request_delete(selected_paths(tab), false); return true; }
    if (action == "delete_forever") { env_.cmd.request_delete(selected_paths(tab), true); return true; }
    if (action == "new_folder")     { new_folder(tab); return true; }
    if (action == "copy_to_pane")   { to_other_pane(tab, false); return true; }
    if (action == "move_to_pane")   { to_other_pane(tab, true); return true; }
    if (action == "select_all")     { L.select_all(); return true; }
    if (action == "select_none")    { L.clear_selection(); return true; }
    if (action == "select_invert") {
        for (std::size_t r = 0; r < L.rows(); ++r) { L.toggle(r); }
        return true;
    }
    if (action == "rename") {
        if (L.focus() != model::Listing::npos) { begin_rename(ui, tab, L.focus()); }
        return true;
    }
    if (action == "copy_path") {
        std::string all;
        for (const std::string& s : selected_paths(tab)) {
            if (!all.empty()) { all += "\r\n"; }
            all += s;
        }
        if (all.empty()) { all = tab.path(); }
        (void)core::shell::set_clipboard_text(all);
        env_.cmd.toast("Path copied", toast_kind::info);
        return true;
    }
    return false;
}

void FileList::new_folder(model::Tab& tab)
{
    if (tab.path().empty() || tab.searching()) { return; }
    const std::string name = core::FileOps::create_folder(tab.path());
    if (name.empty()) {
        env_.cmd.toast("Could not create a folder here", toast_kind::error);
        return;
    }
    {
        const std::string made = core::path::join(tab.path(), name);
        env_.history.push({"New folder " + name,
                           [made] { return core::FileOps::remove_empty_folder(made); },
                           [made] { return ::CreateDirectoryW(core::text::widen(made).c_str(), nullptr) != FALSE; }});
    }
    pending_rename_ = name;
    tab.reveal_name = name;
    tab.refresh();
}

void FileList::begin_rename(context&, model::Tab& tab, std::size_t row)
{
    const core::FileEntry& e = tab.listing().at(row);
    rename_active_ = true;
    rename_path_   = core::path::join(tab.path(), e.name);
    rename_orig_   = e.name;
    rename_text_   = core::path::leaf(e.name); // (search results carry a relative path)
    rename_row_    = row;
    rename_age_    = 0;
    ++rename_gen_; // a fresh window each time: it opens on top of everything
    ensure_visible_ = true;
}

// the name turns into a text field: a small placed window exactly over it
void FileList::draw_rename(Painter& p, model::Tab& tab, const Geo& g, vec2 origin)
{
    if (!rename_active_) { return; }
    context&        ui  = p.ui();
    model::Listing& L   = tab.listing();
    const Palette&  pal = p.pal();

    std::size_t row = rename_row_;
    if (row >= L.rows() || L.at(row).name != rename_orig_) {
        const auto f = L.find_name(rename_orig_);
        if (!f) { rename_active_ = false; return; }
        row = rename_row_ = *f;
    }
    const rect cell = cell_rect(g, row, origin);
    rect box;
    if (g.grid) {
        box = rect{{cell.min.x, cell.max.y - 34.0f}, {cell.max.x, cell.max.y - 6.0f}};
    } else {
        const float name_x = cell.min.x + 12.0f + env_.theme.metrics().icon + 10.0f - 10.0f;
        box = rect{{name_x, cell.min.y + 1.0f}, {name_x + std::min(380.0f, viewport_.width() - 160.0f), cell.max.y - 1.0f}};
    }

    bool done = false;
    bool commit = false;
    {
        PanelScope panel{ui, "##rename" + std::to_string(rename_gen_), box};
        shape_style s;
        s.radius       = radii(7.0f);
        s.fill_top     = s.fill_bottom = pal.raised;
        s.border       = pal.accent;
        s.border_width = 1.5f;
        p.dl().shape(box, s);
        ui.push_var(style_var::frame_padding_y, std::max(0.0f, (box.height() - p.line_height(env_.fonts.body)) * 0.5f));
        ui.push_color(style_color::widget_bg, color{0, 0, 0, 0});
        ui.push_color(style_color::widget_border, color{0, 0, 0, 0});
        ui.set_next_item_width(box.width());
        if (rename_age_ < 3) { ui.request_text_focus("##rn"); }
        (void)ui.input_text("##rn", rename_text_);
        ui.pop_color(2);
        ui.pop_var();
        ++rename_age_;
        if (ui.key_pressed(key::enter)) { done = commit = true; }
        else if (ui.key_pressed(key::escape)) { done = true; }
        else if (rename_age_ >= 4 && !ui.field_focused("##rn")) { done = commit = true; } // clicking elsewhere keeps the new name
    }
    if (!done) { return; }
    rename_active_ = false;
    if (commit && rename_text_ != core::path::leaf(rename_orig_) && !rename_text_.empty()) {
        if (core::FileOps::rename(rename_path_, rename_text_)) {
            const std::string from = rename_path_;
            const std::string to   = core::path::join(core::path::parent(rename_path_), rename_text_);
            env_.history.push({"Rename " + core::path::leaf(from),
                               [from, to] { return core::FileOps::rename(to, core::path::leaf(from)); },
                               [from, to] { return core::FileOps::rename(from, core::path::leaf(to)); }});
            tab.reveal_name = rename_text_;
            tab.refresh();
        } else {
            env_.cmd.toast("Could not rename: the name is invalid or already used", toast_kind::error);
        }
    }
}

// --- keyboard -------------------------------------------------------------------------------------------------

void FileList::handle_keys(context& ui, model::Tab& tab, const FrameInput& in, const Geo& g, float viewport_h)
{
    model::Listing& L = tab.listing();
    const std::size_t n = L.rows();
    if (ui.want_text_input() || ui.menu_open()) { return; }

    const auto per = static_cast<std::size_t>(g.per_row);
    const std::size_t page = std::max<std::size_t>(1, static_cast<std::size_t>(viewport_h / g.cell_h) - 1) * per;
    const bool        has_focus = L.focus() != model::Listing::npos;
    const std::size_t cur       = has_focus ? L.focus() : 0;

    const auto nav = [&](key k, std::size_t first_target, std::size_t target) {
        for (const bool shift : {false, true}) {
            if (ui.key_pressed(k, false, shift)) {
                keyboard_mode_ = true;
                move_focus(tab, has_focus ? target : first_target, shift);
                return;
            }
        }
    };
    if (n > 0) {
        nav(key::down,      0,     std::min(cur + per, n - 1));
        nav(key::up,        n - 1, cur >= per ? cur - per : 0);
        nav(key::page_down, 0,     std::min(cur + page, n - 1));
        nav(key::page_up,   0,     cur > page ? cur - page : 0);
        nav(key::home,      0,     0);
        nav(key::end,       n - 1, n - 1);
        if (g.grid) {
            nav(key::right, 0,     std::min(cur + 1, n - 1));
            nav(key::left,  n - 1, cur > 0 ? cur - 1 : 0);
        }
        if (ui.key_pressed(key::enter) && has_focus) { open_row(tab, cur); }
        if (ui.key_pressed(key::escape)) { L.clear_selection(); }
    }

    // type-to-find: letters typed in quick succession build a prefix
    find_age_ += in.dt;
    if (find_age_ > 1.0) { find_buffer_.clear(); }
    bool typed_any = false;
    for (const char c : in.typed) {
        if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7f) {
            find_buffer_.push_back(c);
            typed_any = true;
        }
    }
    if (typed_any && !ui.ctrl_down() && !ui.alt_down()) {
        find_age_ = 0.0;
        const std::size_t from = find_buffer_.size() == 1 && has_focus ? cur + 1 : cur;
        if (const auto hit = L.find_prefix(find_buffer_, from)) {
            keyboard_mode_ = true;
            move_focus(tab, *hit, false);
        }
    }
}

// --- drawing ---------------------------------------------------------------------------------------------------

void FileList::draw_header(Painter& p, model::Listing& L, float width, float height, float body_w)
{
    context&       ui  = p.ui();
    const Palette& pal = p.pal();
    const FontSet& f   = env_.fonts;

    const item_result r = ui.custom_item("##hdr" + std::string{right_ ? "r" : "l"}, {width, height});
    const rect  box = r.bounds;
    const Columns c = layout_columns(widths_, body_w);
    const vec2  mp  = ui.mouse_pos();
    p.dl().rect_filled({{box.min.x + k_pad, box.max.y - 1.0f}, {box.max.x - k_pad, box.max.y}}, pal.divider);

    struct Cell { SortKey key; std::string_view label; float x0, x1; bool right; };
    const Cell cells[] = {
        {SortKey::name,     "Name",          box.min.x + k_pad,    box.min.x + c.mod_x,  false},
        {SortKey::modified, "Date modified", box.min.x + c.mod_x,  box.min.x + c.type_x, false},
        {SortKey::type,     "Type",          box.min.x + c.type_x, box.min.x + c.size_x, false},
        {SortKey::size,     "Size",          box.min.x + c.size_x, box.max.x - k_pad,    true},
    };

    // resize handles sit on the left edge of the three right columns
    const float handles[col_count] = {box.min.x + c.mod_x, box.min.x + c.type_x, box.min.x + c.size_x};
    int hot_handle = -1;
    if (r.hovered) {
        for (int i = 0; i < col_count; ++i) {
            if (std::abs(mp.x - handles[i]) <= 4.0f) { hot_handle = i; }
        }
    }
    if (hot_handle >= 0 || resize_col_ >= 0) { env_.cmd.set_cursor(cursor_kind::resize_ew); }
    if (hot_handle >= 0 && ui.mouse_clicked(mouse_button::left)) {
        resize_col_     = hot_handle;
        resize_start_x_ = mp.x;
        resize_start_w_ = widths_[static_cast<std::size_t>(hot_handle)];
    }
    if (resize_col_ >= 0) {
        if (!ui.mouse_down(mouse_button::left)) {
            resize_col_ = -1;
        } else {
            widths_[static_cast<std::size_t>(resize_col_)] = std::clamp(resize_start_w_ - (mp.x - resize_start_x_), 60.0f, 420.0f);
        }
    }

    const float name_pad = 10.0f + env_.theme.metrics().icon + 10.0f;
    for (const Cell& cell : cells) {
        // the hover is a flat band the full height of the header, neighbours touching: no rounded blobs with gaps
        const rect cr{{cell.x0, box.min.y}, {cell.x1, box.max.y - 1.0f}};
        const bool hot = r.hovered && cr.contains(mp) && hot_handle < 0 && resize_col_ < 0;
        const bool is_sorted = L.sort().key == cell.key;
        const float k = ui.animate(std::string{"hdr"} + std::string{cell.label} + (right_ ? "r" : "l"), hot ? 1.0f : 0.0f, 16.0f);
        if (k > 0.01f) { p.dl().rect_filled(cr, pal.hover.scaled_alpha(k * 0.8f)); }

        const float text_pad = cell.key == SortKey::name ? name_pad : 10.0f;
        const color col = is_sorted ? pal.text : pal.text_dim;
        float label_w = 0.0f;
        if (cell.right) {
            label_w = p.text_right(cr, cell.label, col, f.small, 14.0f);
        } else {
            label_w = p.text_left(cr, cell.label, col, f.small, text_pad);
        }
        if (is_sorted) {
            const float ax = cell.right ? cr.max.x - 14.0f - label_w - 10.0f : cr.min.x + text_pad + label_w + 10.0f;
            p.sort_arrow({ax, cr.center().y}, L.sort().ascending, pal.accent);
        }
        if (hot && ui.mouse_clicked(mouse_button::left)) {
            model::SortSpec s = L.sort();
            if (s.key == cell.key) {
                s.ascending = !s.ascending;
            } else {
                s.key = cell.key;
                // sizes and dates are most useful biggest / newest first
                s.ascending = cell.key == SortKey::name || cell.key == SortKey::type;
            }
            L.set_sort(s);
        }
    }
    for (int i = 0; i < col_count; ++i) {
        const bool active = hot_handle == i || resize_col_ == i;
        p.dl().rect_filled({{handles[i], box.min.y + 9.0f}, {handles[i] + 1.0f, box.max.y - 10.0f}}, active ? pal.accent : pal.divider);
    }
}

void FileList::draw_empty_state(Painter& p, model::Tab& tab, const rect& vp)
{
    const Palette& pal = p.pal();
    const FontSet& f   = env_.fonts;
    std::string line, hint;
    color col = pal.text_dim;
    char32_t g = glyph::folder_open;
    if (tab.listing().finished()) {
        if (tab.error() != 0) {
            line = std::string{error_text(tab.error())};
            col  = pal.danger;
            g    = glyph::lock;
        } else if (tab.listing().total() > 0) {
            line = tab.searching() ? "No results" : "No matching items";
            g    = glyph::search;
            if (!tab.searching()) { hint = "Ctrl+Enter in the box searches the subfolders too"; }
        } else {
            line = tab.searching() ? "No results" : "This folder is empty";
            if (tab.searching()) { g = glyph::search; }
        }
    } else {
        line = tab.searching() ? "Searching..." : "Loading...";
    }
    const float cy = vp.min.y + vp.height() * 0.34f;
    const rect  icon = rect::from_size({vp.center().x - 28.0f, cy - 40.0f}, {56.0f, 56.0f});
    p.dl().rect_filled(icon, col.scaled_alpha(0.12f), 16.0f);
    p.glyph_center(icon, g, col.scaled_alpha(0.9f), f.icons);
    p.text_center(rect{{vp.min.x, cy + 24.0f}, {vp.max.x, cy + 48.0f}}, line, col, f.body);
    if (!hint.empty()) { p.text_center(rect{{vp.min.x, cy + 48.0f}, {vp.max.x, cy + 70.0f}}, hint, pal.text_dim, f.small); }
}

void FileList::draw_details(Painter& p, model::Tab& tab, const Geo& g, vec2 origin, float vtop, float vbottom, int hover, float enter)
{
    context&        ui  = p.ui();
    const Palette&  pal = p.pal();
    const Metrics&  m   = p.metrics();
    const FontSet&  f   = env_.fonts;
    model::Listing& L   = tab.listing();
    const Columns   c   = layout_columns(widths_, g.body_w);

    const float rh = g.cell_h;
    const std::size_t rows = L.rows();
    const auto first = static_cast<std::size_t>(std::max(0.0f, std::floor((vtop - origin.y) / rh)));
    const auto last  = std::min(rows, static_cast<std::size_t>(std::max(0.0f, std::ceil((vbottom - origin.y) / rh))) + 1);

    const float x0 = origin.x;
    const float dy = (1.0f - enter) * 10.0f; // rows rise into place after navigating
    p.dl().push_alpha(enter);

    // the hover highlight glides between rows instead of jumping
    const std::string side = right_ ? "r" : "l";
    const float target_y = origin.y + static_cast<float>(std::max(hover, 0)) * rh;
    const float hy = hover >= 0 ? ui.animate("fl_hy" + side, target_y, 34.0f) : target_y;
    const float ha = ui.animate("fl_ha" + side, hover >= 0 ? 1.0f : 0.0f, 20.0f);
    if (ha > 0.01f && hover >= 0) {
        p.dl().rect_filled({{x0 + k_pad, hy + 1.0f + dy}, {x0 + g.body_w - k_pad, hy + rh - 1.0f + dy}}, pal.hover.scaled_alpha(ha), 8.0f);
    }

    const float icon = m.icon;
    const int   icon_px = static_cast<int>(std::ceil(icon * ui.scale()));
    for (std::size_t i = first; i < last; ++i) {
        const float y = origin.y + static_cast<float>(i) * rh + dy;
        const rect  row{{x0 + k_pad, y + 1.0f}, {x0 + g.body_w - k_pad, y + rh - 1.0f}};
        const core::FileEntry& e = L.at(i);
        const bool sel = L.selected(i);

        if (sel) {
            p.dl().rect_filled(row, pal.selection, 8.0f);
            p.dl().rect_filled(rect::from_size({row.min.x, row.center().y - 8.0f}, {3.0f, 16.0f}), pal.accent, 1.5f);
        }
        if (keyboard_mode_ && L.focus() == i) { p.dl().rect_outline(row, pal.selection_edge, 8.0f, 1.5f); }
        if (drop_point_ && e.is_dir() && static_cast<int>(i) == hover) { p.dl().rect_outline(row, pal.accent, 8.0f, 2.0f); }

        const bool cut = is_cut(tab, e.name);
        if (cut) { p.dl().push_alpha(0.45f); }
        const rect ibox = rect::from_size({row.min.x + 12.0f, y + (rh - icon) * 0.5f}, {icon, icon});
        bool drawn = false;
        if (shell_icon_kind(e.kind)) {
            const platform::Image img = env_.images.get(core::path::join(tab.path(), e.name), icon_px, platform::ImageKind::icon);
            if (img.id != 0) { p.image_fit(ibox, img, 0.0f); drawn = true; }
        }
        if (!drawn) { p.kind_icon(ibox, e.kind); }

        const float tx = row.min.x + 12.0f + icon + 10.0f;
        const color name_col = e.is_hidden() ? pal.text_dim : pal.text;
        // search results carry a relative path: the name stands out, the folder it lives in follows dimmed
        const rect name_cell{{tx, y}, {x0 + c.mod_x - 8.0f, y + rh}};
        const std::size_t slash = e.name.rfind('\\');
        if (slash == std::string::npos) {
            p.text_left(name_cell, e.name, name_col, f.body);
        } else {
            const float w  = p.text_left(name_cell, std::string_view{e.name}.substr(slash + 1), name_col, f.body);
            const float dx = tx + w + 12.0f;
            if (dx < name_cell.max.x - 40.0f) {
                p.text_left(rect{{dx, y}, name_cell.max}, std::string_view{e.name}.substr(0, slash), pal.text_dim, f.small);
            }
        }

        const color dim = pal.text_dim;
        p.text_left(rect{{x0 + c.mod_x, y}, {x0 + c.type_x - 8.0f, y + rh}}, core::path::format_time(e.modified), dim, f.body, 10.0f);
        p.text_left(rect{{x0 + c.type_x, y}, {x0 + c.size_x - 8.0f, y + rh}}, core::type_label(e.kind, e.ext()), dim, f.body, 10.0f);
        if (!e.is_dir()) {
            p.text_right(rect{{x0 + c.size_x, y}, {x0 + c.end_x, y + rh}}, core::path::format_size(e.size), dim, f.body, 14.0f);
        }
        if (cut) { p.dl().pop_alpha(); }
    }
    p.dl().pop_alpha();
}

void FileList::draw_grid(Painter& p, model::Tab& tab, const Geo& g, vec2 origin, float vtop, float vbottom, int hover, float enter)
{
    context&        ui  = p.ui();
    const Palette&  pal = p.pal();
    const FontSet&  f   = env_.fonts;
    model::Listing& L   = tab.listing();

    const std::size_t rows = L.rows();
    const auto per = static_cast<std::size_t>(g.per_row);
    const auto first_row = static_cast<std::size_t>(std::max(0.0f, std::floor((vtop - origin.y) / g.cell_h)));
    const auto last_row  = static_cast<std::size_t>(std::max(0.0f, std::ceil((vbottom - origin.y) / g.cell_h))) + 1;
    const std::size_t first = std::min(rows, first_row * per);
    const std::size_t last  = std::min(rows, last_row * per);

    const float dy = (1.0f - enter) * 10.0f;
    p.dl().push_alpha(enter);

    const std::string side = right_ ? "r" : "l";
    if (hover >= 0) {
        const rect hc = cell_rect(g, static_cast<std::size_t>(hover), origin);
        const float hx = ui.animate("fg_hx" + side, hc.min.x, 34.0f);
        const float hy = ui.animate("fg_hy" + side, hc.min.y, 34.0f);
        const float ha = ui.animate("fg_ha" + side, 1.0f, 20.0f);
        p.dl().rect_filled(rect::from_size({hx, hy + dy}, hc.size()), pal.hover.scaled_alpha(ha), 11.0f);
    } else {
        (void)ui.animate("fg_ha" + side, 0.0f, 20.0f);
    }

    for (std::size_t i = first; i < last; ++i) {
        rect cell = cell_rect(g, i, origin);
        cell.min.y += dy;
        cell.max.y += dy;
        const core::FileEntry& e = L.at(i);
        const bool sel = L.selected(i);
        if (sel) {
            p.dl().rect_filled(cell, pal.selection, 11.0f);
            p.dl().rect_outline(cell, pal.selection_edge, 11.0f, 1.0f);
        }
        if (keyboard_mode_ && L.focus() == i) { p.dl().rect_outline(cell, pal.selection_edge, 11.0f, 1.5f); }
        if (drop_point_ && e.is_dir() && static_cast<int>(i) == hover) { p.dl().rect_outline(cell, pal.accent, 11.0f, 2.0f); }

        const bool cut = is_cut(tab, e.name);
        if (cut) { p.dl().push_alpha(0.45f); }
        const float edge = cell.width() - 16.0f;
        const rect  thumb = rect::from_size({cell.min.x + 8.0f, cell.min.y + 8.0f}, {edge, cell.height() - 16.0f - 24.0f});
        bool drawn = false;
        if (thumbnail_kind(e.kind) && !e.is_dir()) {
            const platform::Image img = env_.images.get(core::path::join(tab.path(), e.name), static_cast<int>(std::ceil(edge * ui.scale())), platform::ImageKind::thumbnail);
            if (img.id != 0) {
                p.image_fit(thumb, img, 6.0f);
                drawn = true;
            }
        }
        if (!drawn) {
            const float s = std::min(thumb.width(), thumb.height()) * 0.78f;
            p.kind_icon(rect::from_size(thumb.center() - vec2{s * 0.5f, s * 0.5f}, {s, s}), e.kind);
        }
        const rect label{{cell.min.x + 4.0f, cell.max.y - 28.0f}, {cell.max.x - 4.0f, cell.max.y - 4.0f}};
        p.text_center(label, core::path::leaf(e.name), e.is_hidden() ? pal.text_dim : pal.text, f.body);
        if (cut) { p.dl().pop_alpha(); }
    }
    p.dl().pop_alpha();
}

void FileList::context_menu(Painter& p, model::Tab& tab, const rect& area)
{
    context&        ui = p.ui();
    model::Listing& L  = tab.listing();
    auto menu = ui.context_menu("##file_ctx" + std::string{right_ ? "r" : "l"}, area);
    if (!menu) { return; }

    const std::vector<std::size_t> picked = L.selected_rows();
    const bool can_paste = core::clipboard_has_files();
    const bool two_panes = env_.other() != nullptr;

    // sort submenu
    const auto sort_menu = [&] {
        if (auto sub = ui.menu("Sort by")) {
            model::SortSpec s = L.sort();
            const struct { const char* label; SortKey key; } keys[] = {{"Name", SortKey::name}, {"Date modified", SortKey::modified}, {"Type", SortKey::type}, {"Size", SortKey::size}};
            for (const auto& k : keys) {
                if (ui.menu_item(k.label, "", s.key == k.key)) { s.key = k.key; L.set_sort(s); }
            }
            ui.menu_separator();
            if (ui.menu_item("Ascending", "", s.ascending)) { s.ascending = true; L.set_sort(s); }
            if (ui.menu_item("Descending", "", !s.ascending)) { s.ascending = false; L.set_sort(s); }
        }
    };

    if (picked.empty()) {
        if (ui.menu_item("New folder", "Ctrl+Shift+N")) { new_folder(tab); }
        if (ui.menu_item("Paste", "Ctrl+V", false, can_paste)) { paste(tab); }
        ui.menu_separator();
        sort_menu();
        if (ui.menu_item(env_.settings.grid_view ? "View as list" : "View as thumbnails")) { env_.cmd.toggle_view(); }
        ui.menu_separator();
        if (ui.menu_item("Refresh", "F5")) { tab.refresh(); }
        if (ui.menu_item("Copy folder path")) { (void)core::shell::set_clipboard_text(tab.path()); }
        if (ui.menu_item("Open in new tab")) { env_.cmd.open_tab(tab.path(), true); }
        if (ui.menu_item("Properties", "Alt+Enter")) { env_.cmd.open_properties({tab.path()}); }
        return;
    }
    const std::size_t row = picked.front();
    const core::FileEntry& e = L.at(row);
    const std::string full = core::path::join(tab.path(), e.name);

    if (ui.menu_item("Open", "Enter")) { open_row(tab, row); }
    if (e.is_dir() && ui.menu_item("Open in new tab")) { env_.cmd.open_tab(full, false); }
    ui.menu_separator();
    if (ui.menu_item("Cut", "Ctrl+X")) { copy_selection(tab, true); }
    if (ui.menu_item("Copy", "Ctrl+C")) { copy_selection(tab, false); }
    if (e.is_dir() && ui.menu_item("Paste into folder", "", false, can_paste)) {
        std::vector<std::string> paths;
        bool cut = false;
        if (core::clipboard_get_files(paths, cut)) { env_.ops.start(cut ? core::OpKind::move : core::OpKind::copy, std::move(paths), full); }
    }
    if (two_panes) {
        if (ui.menu_item("Copy to other pane", "Ctrl+Shift+C")) { to_other_pane(tab, false); }
        if (ui.menu_item("Move to other pane", "Ctrl+Shift+X")) { to_other_pane(tab, true); }
    }
    if (picked.size() == 1 && ui.menu_item("Rename", "F2")) { begin_rename(ui, tab, row); }
    if (ui.menu_item("Delete", "Del")) { env_.cmd.request_delete(selected_paths(tab), false); }
    ui.menu_separator();
    if (ui.menu_item(picked.size() > 1 ? "Copy paths" : "Copy path")) {
        std::string all;
        for (const std::string& s : selected_paths(tab)) {
            if (!all.empty()) { all += "\r\n"; }
            all += s;
        }
        (void)core::shell::set_clipboard_text(all);
    }
    if (ui.menu_item("Show in File Explorer")) { (void)core::shell::reveal_in_explorer(full); }
    if (ui.menu_item("Properties", "Alt+Enter")) { env_.cmd.open_properties(selected_paths(tab)); }
    if (ui.menu_item("Windows properties")) { (void)core::shell::show_properties(full); }
}

void FileList::frame(Painter& p, const rect& area, model::Tab& tab, const FrameInput& in, bool active)
{
    context&        ui  = p.ui();
    const Palette&  pal = p.pal();
    const Metrics&  m   = p.metrics();
    model::Listing& L   = tab.listing();
    area_  = area;
    shown_ = &tab;

    PanelScope panel{ui, right_ ? "##files_r" : "##files", area};

    shape_style card;
    card.radius       = radii(m.pane_radius);
    card.fill_top     = card.fill_bottom = pal.content;
    card.border       = (env_.right != nullptr && active) ? pal.accent.scaled_alpha(0.55f) : pal.border;
    card.border_width = 1.0f;
    p.dl().shape(area, card);

    if (last_tab_ != &tab || applied_generation_ != tab.generation) {
        const bool same_tab = last_tab_ == &tab;
        last_tab_           = &tab;
        applied_generation_ = tab.generation;
        restore_pending_    = true;
        keyboard_mode_      = false;
        rename_active_      = false;
        find_buffer_.clear();
        enter_ = same_tab ? 0.0f : 1.0f; // (switching tabs is instant; navigating within one eases in)
    }
    enter_ = std::min(1.0f, enter_ + in.dt * 7.0f);
    const float enter_eased = 1.0f - (1.0f - enter_) * (1.0f - enter_);

    const bool  grid_mode = env_.settings.grid_view;
    const float hh     = grid_mode ? 6.0f : m.header_height;
    const float body_w = area.width() - context::scrollbar_width();
    if (!grid_mode) {
        draw_header(p, L, area.width(), hh, body_w);
    } else {
        (void)ui.custom_item("##hdr_gap" + std::string{right_ ? "r" : "l"}, {area.width(), hh});
    }

    auto body = ui.child(right_ ? "##rows_r" : "##rows", {area.width(), area.height() - hh - k_pad}, child_flags::no_padding);
    const rect vp = ui.content_rect();
    viewport_ = vp;

    const Geo g = make_geo(body_w);
    geo_ = g;
    const std::size_t line_count = (L.rows() + static_cast<std::size_t>(g.per_row) - 1) / static_cast<std::size_t>(g.per_row);
    const float total_h = static_cast<float>(line_count) * g.cell_h + (g.grid ? k_pad : 0.0f);
    const item_result cv = ui.custom_item(right_ ? "##canvas_r" : "##canvas", {body_w, std::max(total_h, vp.height() - 1.0f)});
    const bool dbl = ui.item_double_clicked();
    const vec2 origin = cv.bounds.min;
    origin_ = origin;

    // scroll: restore after navigation, keep the focused item in view
    {
        const float scroll = ui.scroll_y();
        if (restore_pending_ && L.finished()) {
            restore_pending_ = false;
            float target = tab.scroll_y;
            if (!tab.reveal_name.empty()) {
                if (const auto row = L.find_name(tab.reveal_name)) {
                    L.select_only(*row);
                    L.set_focus(*row, true);
                    target = std::max(0.0f, static_cast<float>(*row / static_cast<std::size_t>(g.per_row)) * g.cell_h - vp.height() * 0.35f);
                }
                tab.reveal_name.clear();
            }
            ui.set_scroll_y(target);
            tab.scroll_y = target;
            if (!pending_rename_.empty()) {
                if (const auto row = L.find_name(pending_rename_)) { begin_rename(ui, tab, *row); }
                pending_rename_.clear();
            }
        } else if (!restore_pending_) {
            tab.scroll_y = scroll;
        }
        if (ensure_visible_ && L.focus() != model::Listing::npos) {
            const float top = static_cast<float>(L.focus() / static_cast<std::size_t>(g.per_row)) * g.cell_h;
            if (top < scroll) {
                ui.set_scroll_y(top);
            } else if (top + g.cell_h > scroll + vp.height()) {
                ui.set_scroll_y(top + g.cell_h - vp.height());
            }
        }
        ensure_visible_ = false;
    }

    // pointer
    const vec2 mp = drop_point_ ? *drop_point_ : ui.mouse_pos();
    int hover = -1;
    if ((cv.hovered || drop_point_) && vp.contains(mp)) { hover = index_at(g, origin, mp, L.rows()); }
    if (cv.hovered && vp.contains(ui.mouse_pos())) {
        if (ui.mouse_clicked(mouse_button::left)) {
            keyboard_mode_ = false;
            env_.right_active = right_;
            if (hover >= 0) {
                const auto row = static_cast<std::size_t>(hover);
                if (ui.ctrl_down()) {
                    L.toggle(row);
                    L.set_focus(row, true);
                } else if (ui.shift_down()) {
                    const std::size_t anchor = L.anchor() == model::Listing::npos ? row : L.anchor();
                    L.select_range(anchor, row, false);
                    L.set_focus(row, false);
                } else if (L.selected(row) && L.selected_count() > 1) {
                    collapse_row_ = hover; // maybe a drag of the whole selection: decided on release
                    L.set_focus(row, true);
                } else {
                    L.select_only(row);
                    L.set_focus(row, true);
                }
                drag_armed_  = !ui.ctrl_down() && !ui.shift_down();
                drag_origin_ = mp;
            } else if (!ui.ctrl_down()) {
                L.clear_selection();
            }
        }
        if (dbl && hover >= 0 && !ui.ctrl_down() && !ui.shift_down()) {
            collapse_row_ = -1;
            drag_armed_   = false;
            open_row(tab, static_cast<std::size_t>(hover));
        }
        if (ui.mouse_clicked(mouse_button::right)) {
            env_.right_active = right_;
            if (hover >= 0 && !L.selected(static_cast<std::size_t>(hover))) {
                L.select_only(static_cast<std::size_t>(hover));
                L.set_focus(static_cast<std::size_t>(hover), true);
            } else if (hover < 0) {
                L.clear_selection();
            }
        }
        if (ui.mouse_clicked(mouse_button::middle) && hover >= 0) {
            const core::FileEntry& e = L.at(static_cast<std::size_t>(hover));
            if (e.is_dir()) { env_.cmd.open_tab(core::path::join(tab.path(), e.name), false); }
        }
    }
    // a press that moves far enough becomes an OS drag of the selection
    if (drag_armed_) {
        if (!ui.mouse_down(mouse_button::left)) {
            drag_armed_ = false;
            if (collapse_row_ >= 0 && static_cast<std::size_t>(collapse_row_) < L.rows()) { L.select_only(static_cast<std::size_t>(collapse_row_)); }
            collapse_row_ = -1;
        } else {
            const vec2 d = ui.mouse_pos() - drag_origin_;
            if (d.x * d.x + d.y * d.y > 36.0f && L.selected_count() > 0) {
                drag_armed_   = false;
                collapse_row_ = -1;
                env_.cmd.begin_drag(selected_paths(tab));
            }
        }
    }
    if (active) { handle_keys(ui, tab, in, g, vp.height()); }

    if (g.grid) {
        draw_grid(p, tab, g, origin, vp.min.y, vp.max.y, hover, enter_eased);
    } else {
        draw_details(p, tab, g, origin, vp.min.y, vp.max.y, hover, enter_eased);
    }
    if (L.rows() == 0) { draw_empty_state(p, tab, vp); }
    if (drop_point_ && hover < 0 && area.contains(*drop_point_)) { p.dl().rect_outline(area.expanded(-1.0f), pal.accent, m.pane_radius, 2.0f); }
    context_menu(p, tab, vp);
    draw_rename(p, tab, g, origin);
}

} // namespace prism::ui
