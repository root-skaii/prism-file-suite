#include "ui/file_list.hpp"

#include "core/archive.hpp"
#include "core/git_status.hpp"
#include "core/image_tools.hpp"
#include "core/path.hpp"
#include "core/recycle.hpp"
#include "core/shell_ops.hpp"
#include "core/templates.hpp"
#include "core/text.hpp"
#include "ui/menu_parts.hpp"
#include "ui/new_menu.hpp"
#include "ui/panel.hpp"
#include "ui/tag_colors.hpp"
#include "ui/tr.hpp"

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
    case 3:  return tr("This folder no longer exists");
    case 5:  return tr("Access is denied");
    case 21: return tr("The device is not ready");
    case 53:
    case 67: return tr("The network location cannot be reached");
    default: return tr("This folder cannot be opened");
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

// an archive that is opened inside the app: not disc images (mounting them is what Windows does) and nothing huge
// (it is unpacked to a temporary folder first)
bool browsable_archive(const core::FileEntry& e)
{
    if (e.is_dir() || !core::is_archive_name(e.name)) { return false; }
    const std::string lower = core::text::lower_ascii(e.name);
    return !lower.ends_with(".iso") && e.size <= (2ull << 30);
}

// a small filled circle in a label's colour, as a texture for a menu row's icon
platform::Image tag_dot(platform::ImageCache& images, int t)
{
    constexpr int n = 16;
    static std::vector<unsigned char> pixels[core::tag::count];
    std::vector<unsigned char>& px = pixels[t];
    if (px.empty()) {
        const color c = tag_color(t);
        px.assign(static_cast<std::size_t>(n * n * 4), 0);
        for (int y = 0; y < n; ++y) {
            for (int x = 0; x < n; ++x) {
                const float dx = static_cast<float>(x) + 0.5f - n * 0.5f, dy = static_cast<float>(y) + 0.5f - n * 0.5f;
                const float edge = std::clamp(n * 0.5f - 1.5f - std::sqrt(dx * dx + dy * dy) + 0.5f, 0.0f, 1.0f); // (1 px soft rim)
                unsigned char* o = &px[static_cast<std::size_t>((y * n + x) * 4)];
                o[0] = static_cast<unsigned char>(c.b); o[1] = static_cast<unsigned char>(c.g); o[2] = static_cast<unsigned char>(c.r);
                o[3] = static_cast<unsigned char>(edge * 255.0f);
            }
        }
    }
    return images.raw("tag_dot_" + std::to_string(t), n, n, px);
}

} // namespace

// --- geometry ---------------------------------------------------------------------------------------------------

std::size_t FileList::Geo::group_index(std::size_t i) const noexcept
{
    if (starts == nullptr || starts->empty()) { return 0; }
    return static_cast<std::size_t>(std::upper_bound(starts->begin(), starts->end(), static_cast<std::uint32_t>(i)) - starts->begin());
}

float FileList::Geo::top_of(std::size_t i) const noexcept
{
    if (grid) {
        const auto per = static_cast<std::size_t>(per_row);
        if (starts == nullptr || starts->empty()) { return static_cast<float>(i / per) * cell_h; }
        const std::size_t gk = group_index(i);
        float y = 0.0f;
        for (std::size_t k = 0; k < gk; ++k) {
            y += group_h;
            if (k + 1 < gk) {
                const std::size_t count = (*starts)[k + 1] - (*starts)[k];
                const std::size_t lines = (count + per - 1) / per;
                y += static_cast<float>(lines) * cell_h;
            } else {
                const std::size_t offset = i - (*starts)[k];
                y += static_cast<float>(offset / per) * cell_h;
            }
        }
        return y;
    }
    return static_cast<float>(i) * cell_h + static_cast<float>(group_index(i)) * group_h;
}

float FileList::Geo::total_height(std::size_t rows) const noexcept
{
    if (grid) {
        const auto per = static_cast<std::size_t>(per_row);
        if (starts == nullptr || starts->empty()) {
            return static_cast<float>((rows + per - 1) / per) * cell_h + k_pad;
        }
        float y = 0.0f;
        for (std::size_t k = 0; k < starts->size(); ++k) {
            const std::size_t s = (*starts)[k];
            const std::size_t next = k + 1 < starts->size() ? (*starts)[k + 1] : rows;
            const std::size_t count = next > s ? next - s : 0;
            const std::size_t lines = (count + per - 1) / per;
            y += group_h + static_cast<float>(lines) * cell_h;
        }
        return y + k_pad;
    }
    return static_cast<float>(rows) * cell_h + (starts != nullptr ? static_cast<float>(starts->size()) * group_h : 0.0f);
}

std::size_t FileList::Geo::first_at(float y, std::size_t rows) const noexcept
{
    if (rows == 0 || y <= 0.0f) { return 0; }
    std::size_t lo = 0, hi = rows - 1; // the last row whose top is at or above y
    while (lo < hi) {
        const std::size_t mid = (lo + hi + 1) / 2;
        if (top_of(mid) <= y) { lo = mid; } else { hi = mid - 1; }
    }
    return lo;
}

int FileList::Geo::row_at(float y, std::size_t rows) const noexcept
{
    if (rows == 0 || y < 0.0f) { return -1; }
    const std::size_t i = first_at(y, rows);
    return y >= top_of(i) && y < top_of(i) + cell_h ? static_cast<int>(i) : -1; // (outside a row: a group header, or the end)
}

FileList::Geo FileList::make_geo(float body_w, const model::Listing& listing) const
{
    const Metrics& m = env_.theme.metrics();
    Geo g;
    g.body_w = body_w;
    g.grid   = env_.settings.grid_view;
    if (g.grid) {
        const float base = std::clamp(env_.settings.grid_size, 88.0f, 220.0f);
        g.cell_w  = base;
        g.cell_h  = std::round(base * 1.06f);
        g.per_row = std::max(1, static_cast<int>((body_w - 2.0f * k_pad) / g.cell_w));
    } else {
        g.cell_w  = body_w;
        g.cell_h  = m.row_height;
        g.per_row = 1;
    }
    if (!listing.group_starts().empty()) {
        g.starts  = &listing.group_starts();
        g.group_h = m.row_height + 4.0f;
    }
    return g;
}

rect FileList::cell_rect(const Geo& g, std::size_t i, vec2 origin) const
{
    if (!g.grid) {
        const float y = origin.y + g.top_of(i);
        return {{origin.x + k_pad, y + 1.0f}, {origin.x + g.body_w - k_pad, y + g.cell_h - 1.0f}};
    }
    const auto per = static_cast<std::size_t>(g.per_row);
    std::size_t local = i;
    if (g.starts != nullptr && !g.starts->empty()) {
        const std::size_t gk = g.group_index(i);
        if (gk > 0) { local = i - (*g.starts)[gk - 1]; }
    }
    const std::size_t col = local % per;
    const float left = origin.x + k_pad + (g.body_w - 2.0f * k_pad - static_cast<float>(g.per_row) * g.cell_w) * 0.5f;
    const float y    = origin.y + g.top_of(i);
    return rect::from_size({left + static_cast<float>(col) * g.cell_w + 3.0f, y + 3.0f}, {g.cell_w - 6.0f, g.cell_h - 6.0f});
}

int FileList::index_at(const Geo& g, vec2 origin, vec2 p, std::size_t rows) const
{
    if (!g.grid) {
        return g.row_at(p.y - origin.y, rows);
    }
    const float left = origin.x + k_pad + (g.body_w - 2.0f * k_pad - static_cast<float>(g.per_row) * g.cell_w) * 0.5f;
    const int c = static_cast<int>(std::floor((p.x - left) / g.cell_w));
    if (c < 0 || c >= g.per_row) { return -1; }
    const float y = p.y - origin.y;
    if (y < 0.0f) { return -1; }
    if (g.starts == nullptr || g.starts->empty()) {
        const int r = static_cast<int>(std::floor(y / g.cell_h));
        const std::size_t i = static_cast<std::size_t>(r) * static_cast<std::size_t>(g.per_row) + static_cast<std::size_t>(c);
        return i < rows ? static_cast<int>(i) : -1;
    }
    const auto per = static_cast<std::size_t>(g.per_row);
    float gy = 0.0f;
    for (std::size_t k = 0; k < g.starts->size(); ++k) {
        const std::size_t s     = (*g.starts)[k];
        const std::size_t next  = k + 1 < g.starts->size() ? (*g.starts)[k + 1] : rows;
        const std::size_t count = next > s ? next - s : 0;
        const std::size_t lines = (count + per - 1) / per;
        const float body_top    = gy + g.group_h;
        const float body_bot    = body_top + static_cast<float>(lines) * g.cell_h;
        if (y < body_top) { return -1; } // over group header
        if (y < body_bot) {
            const std::size_t r = static_cast<std::size_t>((y - body_top) / g.cell_h);
            const std::size_t idx = s + r * per + static_cast<std::size_t>(c);
            return idx < next ? static_cast<int>(idx) : -1;
        }
        gy = body_bot;
    }
    return -1;
}

// a row files can be dropped on: a folder, or (like Explorer) a program / script / shortcut that is run with them
bool FileList::accepts_drop(const core::FileEntry& e) const
{
    return e.is_dir() || (env_.settings.drop_on_programs && core::is_program_name(e.name));
}

std::string FileList::drop_target_at(vec2 p) const
{
    if (shown_ == nullptr || !area_.contains(p)) { return {}; }
    const model::Listing& L = shown_->listing();
    if (viewport_.contains(p)) {
        const int i = index_at(geo_, origin_, p, L.rows());
        if (i >= 0 && accepts_drop(L.at(static_cast<std::size_t>(i)))) { return core::path::join(shown_->path(), L.at(static_cast<std::size_t>(i)).name); }
    }
    return shown_->path();
}

// --- navigation ---------------------------------------------------------------------------------------------

void FileList::open_row(model::Tab& tab, std::size_t row)
{
    const core::FileEntry& e = tab.listing().at(row);
    const std::string full = core::path::join(tab.path(), e.name);
    if (e.has_display_override() && core::recycle::inside(tab.path())) {
        // Explorer opens Properties for items inside the Recycle Bin instead of executing raw $R... files
        env_.cmd.open_properties({full});
    } else if (e.is_dir()) {
        tab.navigate(full);
    } else if (env_.settings.browse_archives && browsable_archive(e)) {
        env_.cmd.open_archive(full);
    } else {
        menus::open_file(env_, full);
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

void FileList::sync_cut_clipboard()
{
    const unsigned long seq = core::clipboard_sequence();
    if (seq == clip_seq_) { return; }
    clip_seq_ = seq;
    cut_paths_.clear();
    cut_dir_lower_.clear();
    cut_names_lower_.clear();
    if (!core::clipboard_has_files()) { return; }
    std::vector<std::string> paths;
    bool cut = false;
    if (core::clipboard_get_files(paths, cut) && cut && !paths.empty()) {
        std::string first_dir = core::text::lower_ascii(core::path::normalize(core::path::parent(paths.front())));
        bool same_dir = !first_dir.empty();
        cut_paths_.reserve(paths.size());
        for (const std::string& p : paths) {
            const std::string norm = core::path::normalize(p);
            const std::string par  = core::text::lower_ascii(core::path::parent(norm));
            if (par != first_dir) { same_dir = false; }
            cut_paths_.push_back(core::text::lower_ascii(norm));
        }
        if (same_dir) {
            cut_dir_lower_ = std::move(first_dir);
            cut_names_lower_.reserve(paths.size());
            for (const std::string& p : paths) { cut_names_lower_.push_back(core::text::lower_ascii(core::path::leaf(p))); }
        }
    }
}

bool FileList::is_cut(const model::Tab& tab, std::string_view name) const
{
    if (cut_paths_.empty()) { return false; }
    if (!cut_dir_lower_.empty()) {
        // fast path: compare folder once without building full paths for every visible row
        const std::string_view tp = tab.path();
        if (tp.size() != cut_dir_lower_.size()) { return false; }
        for (std::size_t i = 0; i < tp.size(); ++i) {
            char c = tp[i];
            if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c + 32); }
            if (c != cut_dir_lower_[i]) { return false; }
        }
        for (const std::string& n : cut_names_lower_) {
            if (n.size() == name.size() && core::text::compare_plain(n, name) == 0) { return true; }
        }
        return false;
    }
    const std::string full = core::text::lower_ascii(core::path::normalize(core::path::join(tab.path(), name)));
    return std::find(cut_paths_.begin(), cut_paths_.end(), full) != cut_paths_.end();
}

void FileList::copy_selection(model::Tab& tab, bool cut)
{
    const std::vector<std::string> paths = selected_paths(tab);
    if (paths.empty()) { return; }
    if (!core::clipboard_set_files(paths, cut)) {
        env_.cmd.toast(tr("Could not use the clipboard"), toast_kind::error);
        return;
    }
    sync_cut_clipboard();
    env_.cmd.toast(cut ? trf_n("{} item cut", "{} items cut", static_cast<long long>(paths.size()), paths.size()) : trf_n("{} item copied", "{} items copied", static_cast<long long>(paths.size()), paths.size()), toast_kind::info);
}

void FileList::paste(model::Tab& tab)
{
    std::vector<std::string> paths;
    bool cut = false;
    if (!core::clipboard_get_files(paths, cut)) { return; }
    env_.ops.start(cut ? core::OpKind::move : core::OpKind::copy, std::move(paths), tab.path());
    if (cut) {
        core::clipboard_clear_cut();
        sync_cut_clipboard();
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
    if (action == "new_file")       { new_file(tab, std::string{tr("New file")}, ""); return true; }
    if (action == "new_text")       { new_file(tab, std::string{tr("New text document")}, ".txt"); return true; }
    if (action.starts_with("new_template:")) { new_from_template(tab, action.substr(13)); return true; }
    if (action == "copy_to_pane")   { to_other_pane(tab, false); return true; }
    if (action == "move_to_pane")   { to_other_pane(tab, true); return true; }
    if (action == "select_all")     { L.select_all(); return true; }
    if (action == "select_none")    { L.clear_selection(); return true; }
    if (action == "select_invert") { L.invert_selection(); return true; }
    if (action == "rename") {
        if (L.selected_rows().size() > 1) { env_.cmd.request_action("batch_rename"); } // (several: one pattern for all)
        else if (L.focus() != model::Listing::npos) { begin_rename(ui, tab, L.focus()); }
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
        env_.cmd.toast(tr("Path copied"), toast_kind::info);
        return true;
    }
    return false;
}

void FileList::new_folder(model::Tab& tab)
{
    if (tab.path().empty() || tab.searching()) { return; }
    const std::string name = core::FileOps::create_folder(tab.path());
    if (name.empty()) {
        env_.cmd.toast(tr("Could not create a folder here"), toast_kind::error);
        return;
    }
    {
        const std::string made = core::path::join(tab.path(), name);
        env_.history.push({trf("New folder {}", name),
                           [made] { return core::FileOps::remove_empty_folder(made); },
                           [made] { return ::CreateDirectoryW(core::text::widen(made).c_str(), nullptr) != FALSE; }});
    }
    pending_rename_ = name;
    tab.reveal_name = name;
    tab.refresh();
}

void FileList::new_file(model::Tab& tab, const std::string& stem, const std::string& ext)
{
    if (tab.path().empty() || tab.searching()) { return; }
    const std::string name = core::FileOps::create_file(tab.path(), stem, ext);
    if (name.empty()) {
        env_.cmd.toast(tr("Could not create a file here"), toast_kind::error);
        return;
    }
    const std::string made = core::path::join(tab.path(), name);
    env_.history.push({trf("New file {}", name),
                       [made] { return core::FileOps::remove_empty_file(made); },
                       [made] {
                           HANDLE h = ::CreateFileW(core::text::widen(made).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
                           if (h == INVALID_HANDLE_VALUE) { return false; }
                           ::CloseHandle(h);
                           return true;
                       }});
    pending_rename_ = name;
    tab.reveal_name = name;
    tab.refresh();
}

// key: "b<index>" a built-in template, "u<file name>" one from the Templates folder
void FileList::new_from_template(model::Tab& tab, std::string_view key)
{
    if (tab.path().empty() || tab.searching() || key.size() < 2) { return; }
    core::Template t;
    if (key[0] == 'b') {
        const auto& all = core::builtin_templates();
        const std::size_t i = static_cast<std::size_t>(std::atoi(std::string{key.substr(1)}.c_str()));
        if (i >= all.size()) { return; }
        t = all[i];
    } else {
        const std::string file{key.substr(1)};
        bool found = false;
        for (core::Template& u : core::user_templates(Settings::templates_path())) {
            if (u.stem + u.ext == file) { t = std::move(u); found = true; break; }
        }
        if (!found) { env_.cmd.toast(tr("That template is gone"), toast_kind::warning); return; }
    }
    const std::string name = core::create_from_template(tab.path(), t);
    if (name.empty()) {
        env_.cmd.toast(tr("Could not create a file here"), toast_kind::error);
        return;
    }
    const std::string made = core::path::join(tab.path(), name);
    core::FileOps*    ops  = &env_.ops;
    env_.history.push({trf("New {}", name), [ops, made] { ops->start(core::OpKind::recycle, {made}, {}, true); return true; }, {}});
    pending_rename_ = name;
    tab.reveal_name = name;
    tab.refresh();
}

void FileList::begin_rename(context&, model::Tab& tab, std::size_t row)
{
    const core::FileEntry& e = tab.listing().at(row);
    if (e.has_display_override()) { return; } // (a Recycle Bin item: its real name is the bin's own)
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
        box = rect{{name_x, cell.min.y + 1.0f}, {name_x + std::min(380.0f, std::max(140.0f, viewport_.width() - 160.0f)), cell.max.y - 1.0f}};
    }
    // never let the rename field float over the toolbar or status bar when scrolled out of the viewport
    if (box.max.y <= viewport_.min.y || box.min.y >= viewport_.max.y) { return; }
    box.min.y = std::max(box.min.y, viewport_.min.y);
    box.max.y = std::min(box.max.y, viewport_.max.y);

    const bool invalid_chars = rename_text_.find_first_of("\\/:*?\"<>|") != std::string::npos ||
                               (!rename_text_.empty() && (rename_text_.back() == '.' || rename_text_.back() == ' '));
    bool done = false;
    bool commit = false;
    int  tab_step = 0; // Tab / Shift+Tab commits and jumps straight to the next / previous item
    {
        PanelScope panel{ui, "##rename" + std::to_string(rename_gen_), box};
        shape_style s;
        s.radius       = radii(7.0f);
        s.fill_top     = s.fill_bottom = pal.raised;
        s.border       = invalid_chars ? pal.danger : pal.accent;
        s.border_width = 1.5f;
        p.dl().shape(box, s);
        ui.push_var(style_var::frame_padding_y, std::max(0.0f, (box.height() - p.line_height(env_.fonts.body)) * 0.5f));
        ui.push_color(style_color::widget_bg, color{0, 0, 0, 0});
        ui.push_color(style_color::widget_border, color{0, 0, 0, 0});
        ui.set_next_item_width(box.width());
        if (rename_age_ < 3) {
            std::size_t sel_end = rename_text_.size();
            if (env_.settings.rename_select_stem && !L.at(row).is_dir()) {
                const std::size_t dot = rename_text_.rfind('.');
                if (dot != std::string::npos && dot > 0) { sel_end = dot; }
            }
            ui.request_text_focus("##rn", 0, sel_end);
        }
        (void)ui.input_text("##rn", rename_text_);
        ui.pop_color(2);
        ui.pop_var();
        if (invalid_chars) { ui.tooltip(tr("A file name cannot contain \\ / : * ? \" < > | or end with a space or dot")); }
        ++rename_age_;
        if (ui.key_pressed(key::enter)) {
            if (!invalid_chars) { done = commit = true; }
        } else if (ui.key_pressed(key::tab) || ui.key_pressed(key::tab, false, true)) {
            if (!invalid_chars) {
                done = commit = true;
                tab_step = ui.shift_down() ? -1 : 1;
            }
        } else if (ui.key_pressed(key::escape)) {
            done = true;
        } else if (rename_age_ >= 4 && !ui.field_focused("##rn")) {
            // switching away to another OS window cancels rather than silently committing a half-typed name
            done   = true;
            commit = env_.window.active() && !invalid_chars;
        }
    }
    if (!done) { return; }
    rename_active_ = false;
    bool renamed = false;
    if (commit && rename_text_ != core::path::leaf(rename_orig_) && !rename_text_.empty() && !invalid_chars) {
        if (core::FileOps::rename(rename_path_, rename_text_)) {
            renamed = true;
            const std::string from = rename_path_;
            const std::string to   = core::path::join(core::path::parent(rename_path_), rename_text_);
            env_.history.push({trf("Rename {}", core::path::leaf(from)),
                               [from, to] { return core::FileOps::rename(to, core::path::leaf(from)); },
                               [from, to] { return core::FileOps::rename(from, core::path::leaf(to)); }});
            // search results carry relative paths (`sub\file.txt`): keep the relative folder so reveal finds it
            const std::size_t sep = rename_orig_.rfind('\\');
            tab.reveal_name = sep == std::string::npos ? rename_text_ : rename_orig_.substr(0, sep + 1) + rename_text_;
            tab.refresh();
        } else {
            env_.cmd.toast(tr("Could not rename: the name is invalid or already used"), toast_kind::error);
        }
    }
    if (tab_step != 0 && L.rows() > 1) {
        const std::size_t next = tab_step > 0 ? (row + 1) % L.rows() : (row == 0 ? L.rows() - 1 : row - 1);
        if (renamed) {
            pending_rename_ = L.at(next).name;
            tab.reveal_name = pending_rename_;
        } else {
            move_focus(tab, next, false);
            begin_rename(ui, tab, next);
        }
    }
}

// --- drag to select -------------------------------------------------------------------------------------------

void FileList::update_band(context& ui, model::Tab& tab, const Geo& g, vec2 origin, const rect& vp, float dt)
{
    if (!band_armed_) { return; }
    model::Listing& L  = tab.listing();
    const vec2      mp = ui.mouse_pos();
    if (!ui.mouse_down(mouse_button::left) || ui.key_pressed(key::escape)) {
        if (!band_active_ && !band_click_name_.empty() && !ui.key_pressed(key::escape)) {
            if (const auto r = L.find_name(band_click_name_)) {
                L.select_only(*r);
                L.set_focus(*r, true);
            }
        }
        band_click_name_.clear();
        band_armed_ = band_active_ = false;
        return;
    }
    const vec2 here{std::clamp(mp.x - origin.x, 0.0f, g.body_w), mp.y - origin.y};
    if (!band_active_) {
        const vec2 d = here - band_anchor_;
        if (d.x * d.x + d.y * d.y <= 16.0f) { return; } // (a click, not a sweep)
        band_active_ = true;
        band_click_name_.clear();
    }
    // near the top / bottom edge the list scrolls under the pointer: the faster, the further out
    const float edge = 28.0f;
    float       dy   = 0.0f;
    if (mp.y < vp.min.y + edge) { dy = -(vp.min.y + edge - mp.y); }
    else if (mp.y > vp.max.y - edge) { dy = mp.y - (vp.max.y - edge); }
    if (dy != 0.0f) {
        ui.set_scroll_y(std::max(0.0f, ui.scroll_y() + std::clamp(dy, -60.0f, 60.0f) * dt * 12.0f));
        ui.invalidate(); // (the pointer may stay still: keep frames coming while it scrolls)
    }

    const rect box{{std::min(band_anchor_.x, here.x), std::min(band_anchor_.y, here.y)}, {std::max(band_anchor_.x, here.x), std::max(band_anchor_.y, here.y)}};
    const std::size_t rows = L.rows();
    L.clear_selection();
    for (const std::size_t r : band_base_) {
        if (r < rows) { L.set_selected(r, true); }
    }
    const auto apply_hit = [&](std::size_t i) {
        if (band_ctrl_) { L.toggle(i); } else { L.set_selected(i, true); }
    };
    std::size_t last = model::Listing::npos;
    if (rows > 0 && box.max.y >= 0.0f && !g.grid) {
        for (std::size_t i = g.first_at(box.min.y, rows); i < rows && g.top_of(i) <= box.max.y; ++i) {
            if (g.top_of(i) + g.cell_h < box.min.y) { continue; }
            apply_hit(i);
            last = i;
        }
    } else if (rows > 0 && box.max.y >= 0.0f) {
        std::size_t start = g.first_at(std::max(0.0f, box.min.y - g.cell_h), rows);
        // back up to the start of the line so we don't miss cells to the left of `start`
        const auto per = static_cast<std::size_t>(g.per_row);
        start = start >= per ? start - per : 0;
        for (std::size_t i = start; i < rows && g.top_of(i) <= box.max.y; ++i) {
            const rect c = cell_rect(g, i, {0.0f, 0.0f});
            if (c.max.x < box.min.x || c.min.x > box.max.x || c.max.y < box.min.y || c.min.y > box.max.y) { continue; }
            apply_hit(i);
            last = i;
        }
    }
    if (last != model::Listing::npos) { L.set_focus(last, true); }
    env_.right_active = right_;
}

void FileList::draw_band(Painter& p, vec2 origin) const
{
    if (!band_active_) { return; }
    context&       ui  = p.ui();
    const Palette& pal = p.pal();
    const vec2     mp  = ui.mouse_pos();
    const vec2     here{std::clamp(mp.x - origin.x, 0.0f, geo_.body_w), mp.y - origin.y};
    const rect     box{{origin.x + std::min(band_anchor_.x, here.x), origin.y + std::min(band_anchor_.y, here.y)},
                       {origin.x + std::max(band_anchor_.x, here.x), origin.y + std::max(band_anchor_.y, here.y)}};
    p.dl().rect_filled(box, pal.accent.scaled_alpha(0.14f), 3.0f);
    p.dl().rect_outline(box, pal.accent.scaled_alpha(0.75f), 3.0f, 1.0f);
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

    // type-to-find: letters typed in quick succession build a prefix (Unicode-aware, Backspace edits, same-letter cycles)
    find_age_ += in.dt;
    if (find_age_ > 1.0) { find_buffer_.clear(); }
    if (!find_buffer_.empty() && ui.key_pressed(key::backspace)) {
        // pop one UTF-8 codepoint from the end of find_buffer_
        while (!find_buffer_.empty() && (static_cast<unsigned char>(find_buffer_.back()) & 0xc0u) == 0x80u) { find_buffer_.pop_back(); }
        if (!find_buffer_.empty()) { find_buffer_.pop_back(); }
        find_age_ = 0.0;
        if (!find_buffer_.empty()) {
            if (const auto hit = L.find_prefix(find_buffer_, cur)) {
                keyboard_mode_ = true;
                move_focus(tab, *hit, false);
            }
        }
    }
    bool typed_any = false;
    std::string added;
    for (const char c : in.typed) {
        if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7f) {
            if (c == ' ' && find_buffer_.empty()) { continue; } // leading Space is Quick Look, not a filename prefix
            added.push_back(c);
            typed_any = true;
        }
    }
    if (typed_any && !ui.ctrl_down() && !ui.alt_down()) {
        find_buffer_ += added;
        find_age_ = 0.0;
        const std::size_t from = find_buffer_.size() == added.size() && has_focus ? cur + 1 : cur;
        auto hit = L.find_prefix(find_buffer_, from);
        // repeated same-letter press (e.g. 'a' then 'a') cycles through items starting with 'a' when "aa" has no match
        if (!hit && find_buffer_.size() > added.size() && added.size() == 1 &&
            core::text::starts_with_icase(find_buffer_, added)) {
            const bool all_same = std::ranges::all_of(find_buffer_, [&](char ch) {
                return core::text::lower_ascii(std::string_view{&ch, 1}) == core::text::lower_ascii(added);
            });
            if (all_same) {
                find_buffer_ = added;
                hit = L.find_prefix(find_buffer_, has_focus ? cur + 1 : 0);
            }
        }
        if (hit) {
            keyboard_mode_ = true;
            move_focus(tab, *hit, false);
        }
    }
}

// --- drawing ---------------------------------------------------------------------------------------------------

void FileList::draw_recycle_bar(Painter& p, model::Tab& tab, float width, float height)
{
    context&        ui  = p.ui();
    const Palette&  pal = p.pal();
    const FontSet&  f   = env_.fonts;
    model::Listing& L   = tab.listing();

    const item_result r   = ui.custom_item("##bin_bar" + std::string{right_ ? "r" : "l"}, {width, height});
    const rect        box = r.bounds;
    const vec2        mp  = ui.mouse_pos();
    p.dl().rect_filled({{box.min.x + k_pad, box.max.y - 1.0f}, {box.max.x - k_pad, box.max.y}}, pal.divider);

    const bool has_sel   = L.selected_count() > 0;
    const bool has_items = L.rows() > 0;

    float x = box.min.x + k_pad + 6.0f;
    const float bh = height - 10.0f;
    const float by = box.center().y - bh * 0.5f;

    const auto pill = [&](std::string_view label, bool enabled, bool danger) -> bool {
        const float bw = p.measure(f.small, label).x + 22.0f;
        const rect  br = rect::from_size({x, by}, {bw, bh});
        x += bw + 8.0f;
        const bool hot = enabled && r.hovered && br.contains(mp);
        const color bg = !enabled ? pal.raised.scaled_alpha(0.35f)
                       : danger   ? (hot ? pal.danger.scaled_alpha(0.22f) : pal.danger.scaled_alpha(0.10f))
                                  : (hot ? pal.hover : pal.raised.scaled_alpha(0.7f));
        const color fg = !enabled ? pal.text_faint : (danger ? pal.danger : pal.text);
        p.dl().rect_filled(br, bg, 6.0f);
        p.dl().rect_outline(br, danger && enabled ? pal.danger.scaled_alpha(0.4f) : pal.border, 6.0f, 1.0f);
        p.text_center(br, label, fg, f.small);
        return hot && ui.mouse_clicked(mouse_button::left);
    };

    if (pill(tr("Restore selected"), has_sel, false)) {
        env_.cmd.restore_from_bin(selected_paths(tab));
    }
    if (pill(tr("Restore all"), has_items, false)) {
        std::vector<std::string> all;
        all.reserve(L.rows());
        for (std::size_t i = 0; i < L.rows(); ++i) { all.push_back(core::path::join(tab.path(), L.at(i).name)); }
        env_.cmd.restore_from_bin(std::move(all));
    }
    if (pill(tr("Empty Recycle Bin"), has_items, true)) {
        env_.cmd.empty_recycle_bin(core::recycle::drive_of(tab.path()));
    }
}

void FileList::draw_header(Painter& p, model::Listing& L, const std::string& folder, float width, float height, float body_w)
{
    context&       ui  = p.ui();
    const Palette& pal = p.pal();
    const FontSet& f   = env_.fonts;

    widths_ = {env_.settings.col_mod_w, env_.settings.col_type_w, env_.settings.col_size_w};

    const item_result r = ui.custom_item("##hdr" + std::string{right_ ? "r" : "l"}, {width, height});
    const rect  box = r.bounds;
    const Columns c = layout_columns(widths_, body_w);
    const vec2  mp  = ui.mouse_pos();
    p.dl().rect_filled({{box.min.x + k_pad, box.max.y - 1.0f}, {box.max.x - k_pad, box.max.y}}, pal.divider);

    // in the Recycle Bin the date is when it was deleted and the third column says where it came from
    const bool in_bin = core::recycle::inside(folder);
    struct Cell { SortKey key; std::string_view label; float x0, x1; bool right; };
    const Cell cells[] = {
        {SortKey::name,     tr("Name"),          box.min.x + k_pad,    box.min.x + c.mod_x,  false},
        {SortKey::modified, in_bin ? tr("Date deleted") : tr("Date modified"), box.min.x + c.mod_x,  box.min.x + c.type_x, false},
        {SortKey::type,     in_bin ? tr("Original location") : tr("Type"), box.min.x + c.type_x, box.min.x + c.size_x, false},
        {SortKey::size,     tr("Size"),          box.min.x + c.size_x, box.max.x - k_pad,    true},
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
    const auto commit_widths = [&] {
        env_.settings.col_mod_w  = widths_[col_modified];
        env_.settings.col_type_w = widths_[col_type];
        env_.settings.col_size_w = widths_[col_size];
        env_.cmd.settings_changed();
    };
    if (hot_handle >= 0 && ui.item_double_clicked()) {
        // double-click column divider to auto-fit to visible/sampled content
        float best = p.measure(f.small, cells[hot_handle + 1].label).x + 34.0f;
        const std::size_t sample = std::min<std::size_t>(L.rows(), 200);
        for (std::size_t i = 0; i < sample; ++i) {
            const core::FileEntry& e = L.at(i);
            std::string s;
            if (hot_handle == col_modified) { s = core::path::format_time(e.modified); }
            else if (hot_handle == col_type) {
                const std::string_view orig = e.original_path();
                s = orig.empty() ? std::string{core::type_label(e.kind, e.ext())} : core::path::parent(orig);
            } else if (!e.is_dir()) { s = core::path::format_size(e.size); }
            if (!s.empty()) { best = std::max(best, p.measure(f.body, s).x + 26.0f); }
        }
        widths_[static_cast<std::size_t>(hot_handle)] = std::clamp(std::ceil(best), 70.0f, 380.0f);
        commit_widths();
        resize_col_ = -1;
    } else if (hot_handle >= 0 && ui.mouse_clicked(mouse_button::left)) {
        resize_col_     = hot_handle;
        resize_start_x_ = mp.x;
        resize_start_w_ = widths_[static_cast<std::size_t>(hot_handle)];
    }
    if (resize_col_ >= 0) {
        if (!ui.mouse_down(mouse_button::left)) {
            resize_col_ = -1;
            commit_widths();
        } else {
            widths_[static_cast<std::size_t>(resize_col_)] = std::clamp(resize_start_w_ - (mp.x - resize_start_x_), 60.0f, 420.0f);
            env_.settings.col_mod_w  = widths_[col_modified];
            env_.settings.col_type_w = widths_[col_type];
            env_.settings.col_size_w = widths_[col_size];
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
            env_.cmd.set_sort(folder, s.key, s.ascending);
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
            line = tab.searching() ? tr("No results") : tr("No matching items");
            g    = glyph::search;
            if (!tab.searching()) { hint = tr("Ctrl+Enter in the box searches the subfolders too"); }
        } else {
            line = tab.searching() ? tr("No results") : tr("This folder is empty");
            if (tab.searching()) { g = glyph::search; }
        }
    } else {
        line = tab.searching() ? tr("Searching...") : tr("Loading...");
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
    const std::size_t first = g.first_at(vtop - origin.y, rows);
    std::size_t last = first;
    while (last < rows && g.top_of(last) <= vbottom - origin.y) { ++last; }

    const float x0 = origin.x;
    const float dy = (1.0f - enter) * 10.0f; // rows rise into place after navigating
    p.dl().push_alpha(enter);

    // the hover highlight glides between rows instead of jumping
    const std::string side = right_ ? "r" : "l";
    const float target_y = origin.y + g.top_of(static_cast<std::size_t>(std::max(hover, 0)));
    const float hy = hover >= 0 ? ui.animate("fl_hy" + side, target_y, 34.0f) : target_y;
    const float ha = ui.animate("fl_ha" + side, hover >= 0 ? 1.0f : 0.0f, 20.0f);
    if (ha > 0.01f && hover >= 0) {
        p.dl().rect_filled({{x0 + k_pad, hy + 1.0f + dy}, {x0 + g.body_w - k_pad, hy + rh - 1.0f + dy}}, pal.hover.scaled_alpha(ha), 8.0f);
    }

    // git: the folder's repository (looked up off the ui thread; null when none) and this folder's place in it
    const std::shared_ptr<const core::GitRepo> git = env_.settings.git_badges && !tab.searching() ? env_.git.lookup(tab.path()) : nullptr;
    const std::string git_base = git ? git->relative(tab.path()) : std::string{};
    std::string git_key;
    const float icon = m.icon;
    const int   icon_px = static_cast<int>(std::ceil(icon * ui.scale()));
    const std::vector<std::uint32_t>* starts = g.starts;
    for (std::size_t i = first; i < last; ++i) {
        if (starts != nullptr && std::binary_search(starts->begin(), starts->end(), static_cast<std::uint32_t>(i))) {
            // the group's header: its title, how many it holds, a hairline under it
            const std::size_t k    = g.group_index(i) - 1;
            const std::size_t next = k + 1 < starts->size() ? (*starts)[k + 1] : rows;
            const float       hy0  = origin.y + g.top_of(i) - g.group_h + dy;
            const rect        title{{x0 + k_pad + 12.0f, hy0 + 6.0f}, {x0 + g.body_w - k_pad, hy0 + g.group_h}};
            const float       tw = p.text_left(title, L.group_title(i), pal.text, f.bold);
            p.text_left(rect{{title.min.x + tw + 10.0f, title.min.y}, title.max}, std::format("{}", next - i), pal.text_faint, f.small);
            p.dl().rect_filled({{x0 + k_pad + 12.0f, hy0 + g.group_h - 1.0f}, {x0 + g.body_w - k_pad, hy0 + g.group_h}}, pal.border, 0.0f);
        }
        const float y = origin.y + g.top_of(i) + dy;
        const rect  row{{x0 + k_pad, y + 1.0f}, {x0 + g.body_w - k_pad, y + rh - 1.0f}};
        const core::FileEntry& e = L.at(i);
        const bool sel = L.selected(i);

        if (sel) {
            p.dl().rect_filled(row, pal.selection, 8.0f);
            p.dl().rect_filled(rect::from_size({row.min.x, row.center().y - 8.0f}, {3.0f, 16.0f}), pal.accent, 1.5f);
        }
        if (keyboard_mode_ && L.focus() == i) { p.dl().rect_outline(row, pal.selection_edge, 8.0f, 1.5f); }
        if (drop_point_ && accepts_drop(e) && static_cast<int>(i) == hover) { p.dl().rect_outline(row, pal.accent, 8.0f, 2.0f); }

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
        const rect name_cell{{tx, y}, {x0 + c.mod_x - 8.0f - (git ? 26.0f : 0.0f), y + rh}};
        const std::string_view shown_name = e.display();
        const std::size_t slash = shown_name.rfind('\\');
        float name_w = 0.0f;
        if (slash == std::string::npos) {
            name_w = p.text_left(name_cell, shown_name, name_col, f.body);
        } else {
            const float w  = p.text_left(name_cell, shown_name.substr(slash + 1), name_col, f.body);
            name_w = w;
            const float dx = tx + w + 12.0f;
            if (dx < name_cell.max.x - 40.0f) {
                p.text_left(rect{{dx, y}, name_cell.max}, shown_name.substr(0, slash), pal.text_dim, f.small);
            }
        }
        if (e.tags != 0) {
            // one dot per label, right after the name (kept inside the name column)
            float dot_x = tx + name_w + 12.0f;
            for (int t = 0; t < core::tag::count; ++t) {
                if ((e.tags & (1u << t)) == 0 || dot_x + 8.0f > name_cell.max.x) { continue; }
                p.dl().circle_filled({dot_x + 4.0f, y + rh * 0.5f}, 4.0f, tag_color(t));
                dot_x += 12.0f;
            }
        }

        if (git) {
            git_key.clear();
            if (!git_base.empty()) { git_key += git_base; git_key.push_back('\\'); }
            for (const char ch : e.name) { git_key.push_back(ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch + 32) : ch); }
            const char code = git->code_for(git_key, e.is_dir());
            if (code != 0) {
                const color bc = code == 'M' ? pal.warning : (code == 'A' || code == '?') ? pal.success : (code == 'R') ? pal.accent : pal.danger;
                const char  letter[2] = {code, 0};
                p.text_right(rect{{x0 + c.mod_x - 30.0f, y}, {x0 + c.mod_x - 8.0f, y + rh}}, letter, bc, f.bold);
            }
        }
        const color dim = pal.text_dim;
        p.text_left(rect{{x0 + c.mod_x, y}, {x0 + c.type_x - 8.0f, y + rh}}, core::path::format_time(e.modified), dim, f.body, 10.0f);
        const std::string_view orig = e.original_path();
        p.text_left(rect{{x0 + c.type_x, y}, {x0 + c.size_x - 8.0f, y + rh}},
                    orig.empty() ? std::string{core::type_label(e.kind, e.ext())} : core::path::parent(orig), dim, f.body, 10.0f);
        if (!e.is_dir()) {
            p.text_right(rect{{x0 + c.size_x, y}, {x0 + c.end_x, y + rh}}, core::path::format_size(e.size), dim, f.body, 14.0f);
        } else if (env_.settings.folder_sizes) {
            const auto total = env_.sizes.lookup(core::path::join(tab.path(), e.name));
            p.text_right(rect{{x0 + c.size_x, y}, {x0 + c.end_x, y + rh}}, total ? core::path::format_size(*total) : std::string{"..."}, pal.text_faint, f.body, 14.0f);
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
    std::size_t first = g.first_at(vtop - origin.y, rows);
    first = first >= per ? first - per : 0;
    std::size_t last = first;
    while (last < rows && g.top_of(last) <= vbottom - origin.y) { ++last; }

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

    const std::vector<std::uint32_t>* starts = g.starts;
    if (starts != nullptr && !starts->empty()) {
        for (std::size_t k = 0; k < starts->size(); ++k) {
            const std::size_t s    = (*starts)[k];
            const std::size_t next = k + 1 < starts->size() ? (*starts)[k + 1] : rows;
            const float       hy0  = origin.y + g.top_of(s) - g.group_h + dy;
            if (hy0 + g.group_h < vtop || hy0 > vbottom) { continue; }
            const rect title{{origin.x + k_pad + 12.0f, hy0 + 6.0f}, {origin.x + g.body_w - k_pad, hy0 + g.group_h}};
            const float tw = p.text_left(title, L.group_title(s), pal.text, f.bold);
            p.text_left(rect{{title.min.x + tw + 10.0f, title.min.y}, title.max}, std::format("{}", next - s), pal.text_faint, f.small);
            p.dl().rect_filled({{origin.x + k_pad + 12.0f, hy0 + g.group_h - 1.0f}, {origin.x + g.body_w - k_pad, hy0 + g.group_h}}, pal.border, 0.0f);
        }
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
        if (drop_point_ && accepts_drop(e) && static_cast<int>(i) == hover) { p.dl().rect_outline(cell, pal.accent, 11.0f, 2.0f); }

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
        p.text_center(label, core::path::leaf(e.display()), e.is_hidden() ? pal.text_dim : pal.text, f.body);
        if (e.tags != 0) {
            float dot_x = cell.min.x + 10.0f;
            for (int t = 0; t < core::tag::count; ++t) {
                if ((e.tags & (1u << t)) == 0) { continue; }
                p.dl().circle_filled({dot_x, cell.min.y + 12.0f}, 4.5f, tag_color(t));
                dot_x += 12.0f;
            }
        }
        if (cut) { p.dl().pop_alpha(); }
    }
    p.dl().pop_alpha();
}

void FileList::context_menu(Painter& p, model::Tab& tab, const rect& area)
{
    context&        ui = p.ui();
    model::Listing& L  = tab.listing();
    const MenuDensity density{ui, env_.settings.compact};
    auto menu = ui.context_menu("##file_ctx" + std::string{right_ ? "r" : "l"}, area);
    if (!menu) { return; }

    const std::vector<std::size_t> picked = L.selected_rows();
    const bool can_paste = core::clipboard_has_files();
    const bool two_panes = env_.other() != nullptr;

    // the Recycle Bin: its items are the bin's own `$R...` files, so only what makes sense for them is offered
    if (!tab.searching() && core::recycle::inside(tab.path())) {
        if (picked.empty()) {
            if (ui.menu_item(tr("Empty Recycle Bin"))) { env_.cmd.empty_recycle_bin(core::recycle::drive_of(tab.path())); }
            ui.menu_separator();
            if (ui.menu_item(tr("Refresh"), env_.binds.text("refresh"))) { tab.refresh(); }
        } else {
            if (ui.menu_item(tr("Restore"))) { env_.cmd.restore_from_bin(selected_paths(tab)); }
            if (ui.menu_item(tr("Delete permanently"), "Del")) { env_.cmd.request_delete(selected_paths(tab), true); }
            ui.menu_separator();
            if (ui.menu_item(tr("Copy"), env_.binds.text("copy"))) { copy_selection(tab, false); }
            if (ui.menu_item(tr("Properties"), env_.binds.text("properties"))) { env_.cmd.open_properties(selected_paths(tab)); }
        }
        return;
    }

    // sort submenu
    const auto sort_menu = [&] {
        if (auto sub = ui.menu(tr("Sort by"))) {
            model::SortSpec s = L.sort();
            const struct { std::string_view label; SortKey key; } keys[] = {{tr("Name"), SortKey::name}, {tr("Date modified"), SortKey::modified}, {tr("Type"), SortKey::type}, {tr("Size"), SortKey::size}};
            for (const auto& k : keys) {
                if (ui.menu_item(k.label, "", s.key == k.key)) { env_.cmd.set_sort(tab.path(), k.key, s.ascending); }
            }
            ui.menu_separator();
            if (ui.menu_item(tr("Ascending"), "", s.ascending)) { env_.cmd.set_sort(tab.path(), s.key, true); }
            if (ui.menu_item(tr("Descending"), "", !s.ascending)) { env_.cmd.set_sort(tab.path(), s.key, false); }
        }
    };

    const auto group_menu = [&] {
        if (auto sub = ui.menu(tr("Group by"))) {
            const struct { std::string_view label; const char* action; } kinds[] = {{tr("None"), "group_none"}, {tr("Name"), "group_name"}, {tr("Type"), "group_type"}, {tr("Date modified"), "group_modified"}, {tr("Size"), "group_size"}};
            for (int i = 0; i < 5; ++i) {
                if (ui.menu_item(kinds[i].label, "", env_.settings.group_by == i)) { env_.cmd.request_action(kinds[i].action); }
            }
        }
    };

    // Explorer's own entries (Open with Code, 7-Zip ...), when the setting is on and the worker has them
    std::vector<core::ShellMenuItem> shell_items;
    if (env_.settings.shell_menu && !tab.path().empty() && !tab.searching()) {
        if (picked.empty()) {
            shell_menu_.query({tab.path()}, true);
        } else {
            shell_menu_.query(selected_paths(tab), false);
        }
        shell_items = shell_menu_.items();
    }

    if (picked.empty()) {
        // Windows' "New" submenu (Word document, shortcut ...) joins ours instead of showing a second "New"
        const auto windows_new = std::ranges::find_if(shell_items, [](const core::ShellMenuItem& i) { return i.label == "New" && !i.children.empty(); });
        if (auto sub = ui.menu(tr("New"), !tab.path().empty() && !tab.searching())) {
            if (ui.menu_item(tr("Folder"), env_.binds.text("new_folder"))) { new_folder(tab); }
            if (ui.menu_item(tr("Empty file"))) { new_file(tab, std::string{tr("New file")}, ""); }
            if (ui.menu_item(tr("Text document"))) { new_file(tab, std::string{tr("New text document")}, ".txt"); }
            ui.menu_separator();
            new_template_items(ui, env_);
            if (windows_new != shell_items.end()) {
                std::vector<core::ShellMenuItem> rest;
                for (const core::ShellMenuItem& c : windows_new->children) {
                    if (c.label != "Folder" && c.label != "Text Document") { rest.push_back(c); }
                }
                if (!rest.empty()) {
                    ui.menu_separator();
                    shell_page_.draw_items(ui, rest, false);
                }
            }
        }
        if (windows_new != shell_items.end()) { shell_items.erase(windows_new); }
        if (ui.menu_item(tr("Paste"), env_.binds.text("paste"), false, can_paste)) { paste(tab); }
        ui.menu_separator();
        sort_menu();
        group_menu();
        ui.menu_separator();
        if (ui.menu_item(tr("Refresh"), env_.binds.text("refresh"))) { tab.refresh(); }
        if (tab.path().empty()) { return; } // ("This PC": no folder to act on)
        ui.menu_separator();
        menus::open_in(ui, env_, tab.path());
        menus::terminal(ui, env_, tab.path());
        ui.menu_separator();
        menus::path_items(ui, env_, {tab.path()});
        menus::pin_item(ui, env_, tab.path());
        ui.menu_separator();
        menus::properties_item(ui, env_, {tab.path()});
        menus::more_items(ui, shell_page_, shell_items);
        if (!tab.searching()) { menus::windows_items(ui, env_, {tab.path()}); }
        return;
    }
    const std::size_t row = picked.front();
    const core::FileEntry& e = L.at(row);
    const std::string full = core::path::join(tab.path(), e.name);

    const bool one_folder = picked.size() == 1 && e.is_dir(); // (the folder-only entries need exactly one)
    if (ui.menu_item(tr("Open"), "Enter")) { open_row(tab, row); }
    if (one_folder) { menus::open_in(ui, env_, full); }
    if (picked.size() == 1 && !env_.settings.browse_archives && browsable_archive(e) && ui.menu_item(tr("Browse inside"))) { env_.cmd.open_archive(full); }
    ui.menu_separator();
    if (ui.menu_item(tr("Cut"), env_.binds.text("cut"))) { copy_selection(tab, true); }
    if (ui.menu_item(tr("Copy"), env_.binds.text("copy"))) { copy_selection(tab, false); }
    if (one_folder) { menus::paste_into(ui, env_, full, can_paste); }
    if (two_panes) {
        if (ui.menu_item(tr("Copy to other pane"), env_.binds.text("copy_to_pane"))) { to_other_pane(tab, false); }
        if (ui.menu_item(tr("Move to other pane"), env_.binds.text("move_to_pane"))) { to_other_pane(tab, true); }
    }
    if (env_.settings.browse_archives && picked.size() == 1 && !e.is_dir() && core::is_archive_name(e.name)) {
        if (ui.menu_item(tr("Extract to folder"))) { env_.cmd.request_action("extract_folder"); }
        if (ui.menu_item(tr("Extract here"))) { env_.cmd.request_action("extract_here"); }
    }
    if (env_.settings.browse_archives && !tab.searching() && ui.menu_item(tr("Compress to zip"))) { env_.cmd.request_action("compress_zip"); }
    if (picked.size() == 1 && ui.menu_item(tr("Rename"), env_.binds.text("rename"))) { begin_rename(ui, tab, row); }
    if (ui.menu_item(tr("Delete"), "Del")) { env_.cmd.request_delete(selected_paths(tab), false); }
    const bool all_images = std::ranges::all_of(picked, [&](std::size_t r) { return !L.at(r).is_dir() && core::is_image_name(L.at(r).name); });
    if (all_images) {
        if (auto sub = ui.menu(tr("Picture tools"))) {
            if (ui.menu_item(tr("Rotate left"))) { env_.cmd.request_action("img_left"); }
            if (ui.menu_item(tr("Rotate right"))) { env_.cmd.request_action("img_right"); }
            ui.menu_separator();
            if (ui.menu_item(tr("Half size"))) { env_.cmd.request_action("img_50"); }
            if (ui.menu_item(tr("Quarter size"))) { env_.cmd.request_action("img_25"); }
            if (ui.menu_item(tr("Fit 1920 pixels"))) { env_.cmd.request_action("img_1920"); }
            ui.menu_separator();
            if (ui.menu_item(tr("Save as JPEG"))) { env_.cmd.request_action("img_jpg"); }
            if (ui.menu_item(tr("Save as PNG"))) { env_.cmd.request_action("img_png"); }
        }
    }
    if (!tab.searching()) {
        if (auto sub = ui.menu(tr("Create link"))) {
            if (ui.menu_item(tr("Symbolic link"))) { env_.cmd.request_action("link_symbolic"); }
            if (ui.menu_item(tr("Hard link (files)"))) { env_.cmd.request_action("link_hard"); }
            if (ui.menu_item(tr("Junction (folders)"))) { env_.cmd.request_action("link_junction"); }
        }
    }
    if (auto sub = ui.menu(tr("Label"))) {
        std::uint16_t every = core::tag::all; // the labels all selected items have
        std::uint16_t some  = 0;
        for (const std::size_t r : picked) {
            every &= L.at(r).tags;
            some  |= L.at(r).tags;
        }
        for (int t = 0; t < core::tag::count; ++t) {
            std::string name{tag_label(t)};
            if (std::wstring w = core::text::widen(name); !w.empty()) { ::CharUpperBuffW(w.data(), 1); name = core::text::narrow(w); }
            const std::string key = env_.binds.text("tag_" + std::to_string(t + 1));
            menu_item_options o;
            o.shortcut = key;
            o.selected = (every & (1u << t)) != 0;
            o.image    = tag_dot(env_.images, t).id;
            if (ui.menu_item(name + "##tag" + std::to_string(t), o)) { env_.cmd.toggle_tag(selected_paths(tab), t); }
        }
        ui.menu_separator();
        if (ui.menu_item(tr("Clear labels"), env_.binds.text("tag_clear"), false, some != 0)) { env_.cmd.clear_tags(selected_paths(tab)); }
    }
    ui.menu_separator();
    if (one_folder) {
        menus::terminal(ui, env_, full);
        ui.menu_separator();
    }
    const std::vector<std::string> chosen = selected_paths(tab);
    menus::path_items(ui, env_, chosen);
    if (one_folder) { menus::pin_item(ui, env_, full); }
    ui.menu_separator();
    menus::properties_item(ui, env_, chosen);
    menus::more_items(ui, shell_page_, shell_items);
    menus::windows_items(ui, env_, chosen);
}

void FileList::frame(Painter& p, const rect& area, model::Tab& tab, const FrameInput& in, bool active)
{
    context&        ui  = p.ui();
    const Palette&  pal = p.pal();
    const Metrics&  m   = p.metrics();
    model::Listing& L   = tab.listing();
    area_  = area;
    shown_ = &tab;
    shell_page_.begin_frame();

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
    if (enter_ < 1.0f) { p.ui().invalidate(); } // (the fade needs its frames: without them the list sleeps half faded)
    const float enter_eased = 1.0f - (1.0f - enter_) * (1.0f - enter_);

    const bool  in_bin    = !tab.searching() && core::recycle::inside(tab.path());
    const float bin_h     = in_bin ? 36.0f : 0.0f;
    if (in_bin) { draw_recycle_bar(p, tab, area.width(), bin_h); }

    const bool  grid_mode = env_.settings.grid_view;
    const float hh     = grid_mode ? 6.0f : m.header_height;
    const float body_w = area.width() - context::scrollbar_width();
    if (!grid_mode) {
        draw_header(p, L, tab.path(), area.width(), hh, body_w);
    } else {
        (void)ui.custom_item("##hdr_gap" + std::string{right_ ? "r" : "l"}, {area.width(), hh});
    }

    auto body = ui.child(right_ ? "##rows_r" : "##rows", {area.width(), area.height() - bin_h - hh - k_pad}, child_flags::no_padding);
    const rect vp = ui.content_rect();
    viewport_ = vp;

    if (grid_mode && vp.contains(ui.mouse_pos()) && ui.ctrl_down()) {
        const float wheel = ui.take_wheel();
        if (wheel != 0.0f) {
            env_.settings.grid_size = std::clamp(env_.settings.grid_size + wheel * 16.0f, 88.0f, 220.0f);
            env_.cmd.settings_changed();
        }
    }

    const Geo g = make_geo(body_w, L);
    geo_ = g;
    const float total_h = g.total_height(L.rows());
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
                    target = std::max(0.0f, g.top_of(*row) - vp.height() * 0.35f);
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
            const float top = g.top_of(L.focus());
            if (top < scroll) {
                ui.set_scroll_y(top - (g.starts != nullptr && std::binary_search(g.starts->begin(), g.starts->end(), static_cast<std::uint32_t>(L.focus())) ? g.group_h : 0.0f));
            } else if (top + g.cell_h > scroll + vp.height()) {
                ui.set_scroll_y(top + g.cell_h - vp.height());
            }
        }
        ensure_visible_ = false;
    }

    sync_cut_clipboard();
    // pointer
    const vec2 mp = drop_point_ ? *drop_point_ : ui.mouse_pos();
    int hover = -1;
    if ((cv.hovered || drop_point_) && vp.contains(mp)) { hover = index_at(g, origin, mp, L.rows()); }

    // drag edge auto-scroll & spring-loaded folder hover (~600 ms)
    if (drop_point_ && vp.contains(*drop_point_)) {
        const float edge = 30.0f;
        float dy = 0.0f;
        if (drop_point_->y < vp.min.y + edge) { dy = -(vp.min.y + edge - drop_point_->y); }
        else if (drop_point_->y > vp.max.y - edge) { dy = drop_point_->y - (vp.max.y - edge); }
        if (dy != 0.0f) {
            ui.set_scroll_y(std::max(0.0f, ui.scroll_y() + std::clamp(dy, -60.0f, 60.0f) * in.dt * 10.0f));
            ui.invalidate();
        }
        std::string hovered_dir;
        if (hover >= 0 && L.at(static_cast<std::size_t>(hover)).is_dir()) {
            hovered_dir = core::path::join(tab.path(), L.at(static_cast<std::size_t>(hover)).name);
        }
        if (!hovered_dir.empty() && hovered_dir == spring_target_) {
            spring_age_ += in.dt;
            if (spring_age_ >= 0.6) {
                tab.navigate(hovered_dir);
                spring_target_.clear();
                spring_age_ = 0.0;
            } else {
                ui.invalidate();
            }
        } else {
            spring_target_ = std::move(hovered_dir);
            spring_age_ = 0.0;
        }
    } else {
        spring_target_.clear();
        spring_age_ = 0.0;
    }

    if (cv.hovered && vp.contains(ui.mouse_pos())) {
        // a double click needs both clicks on the same item: after a delete the next file slides under the pointer
        if (ui.mouse_clicked(mouse_button::left)) {
            keyboard_mode_ = false;
            const unsigned long long now_ms = ::GetTickCount64();
            const unsigned long long elapsed = now_ms - click_ms_;
            click_ms_   = now_ms;
            prev_click_ = std::move(click_name_);
            click_name_ = hover >= 0 ? L.at(static_cast<std::size_t>(hover)).name : std::string{};
            slow_rename_armed_ = false;
            slow_rename_name_.clear();
            env_.right_active = right_;
            // in Details view, dragging on an unselected row's right columns (Date / Type / Size) starts a marquee
            // instead of an OS drag; a plain click there still selects the row on release
            const Columns cols = layout_columns(widths_, body_w);
            const bool details_blank_col = !g.grid && hover >= 0 && mp.x >= origin.x + cols.mod_x &&
                                           !L.selected(static_cast<std::size_t>(hover)) && !ui.ctrl_down() && !ui.shift_down();
            if (hover >= 0 && !details_blank_col) {
                const auto row = static_cast<std::size_t>(hover);
                const bool was_sole_selected = L.selected(row) && L.selected_count() == 1;
                if (ui.shift_down()) {
                    const std::size_t anchor = L.anchor() == model::Listing::npos ? row : L.anchor();
                    L.select_range(anchor, row, ui.ctrl_down());
                    L.set_focus(row, false);
                } else if (ui.ctrl_down()) {
                    L.toggle(row);
                    L.set_focus(row, true);
                } else if (L.selected(row) && L.selected_count() > 1) {
                    collapse_name_ = L.at(row).name; // maybe a drag of the whole selection: decided on release
                    L.set_focus(row, true);
                } else {
                    L.select_only(row);
                    L.set_focus(row, true);
                    // slow second click on an already-selected single item arms inline rename (like Explorer)
                    if (was_sole_selected && click_name_ == prev_click_ && elapsed >= 450 && elapsed <= 3000) {
                        slow_rename_armed_ = true;
                        slow_rename_name_  = click_name_;
                    }
                }
                drag_armed_  = !ui.ctrl_down() && !ui.shift_down();
                drag_origin_ = mp;
            } else {
                if (!ui.ctrl_down()) { L.clear_selection(); }
                band_click_name_ = details_blank_col ? L.at(static_cast<std::size_t>(hover)).name : std::string{};
                band_armed_  = true;
                band_active_ = false;
                band_anchor_ = mp - origin;
                band_ctrl_   = ui.ctrl_down();
                band_base_   = band_ctrl_ ? L.selected_rows() : std::vector<std::size_t>{};
            }
        }
        if (dbl && hover >= 0 && !ui.ctrl_down() && !ui.shift_down() && L.at(static_cast<std::size_t>(hover)).name == prev_click_) {
            collapse_name_.clear();
            slow_rename_armed_ = false;
            slow_rename_name_.clear();
            band_click_name_.clear();
            band_armed_ = band_active_ = false;
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
            if (!collapse_name_.empty()) {
                if (const auto r = L.find_name(collapse_name_)) { L.select_only(*r); }
                collapse_name_.clear();
            } else if (slow_rename_armed_ && !slow_rename_name_.empty()) {
                if (const auto r = L.find_name(slow_rename_name_)) { begin_rename(ui, tab, *r); }
            }
            slow_rename_armed_ = false;
            slow_rename_name_.clear();
        } else {
            const vec2 d = ui.mouse_pos() - drag_origin_;
            if (d.x * d.x + d.y * d.y > 36.0f && L.selected_count() > 0) {
                drag_armed_ = false;
                collapse_name_.clear();
                slow_rename_armed_ = false;
                slow_rename_name_.clear();
                env_.cmd.begin_drag(selected_paths(tab));
            }
        }
    }
    update_band(ui, tab, g, origin, vp, in.dt);
    if (active) { handle_keys(ui, tab, in, g, vp.height()); }

    if (g.grid) {
        draw_grid(p, tab, g, origin, vp.min.y, vp.max.y, hover, enter_eased);
    } else {
        draw_details(p, tab, g, origin, vp.min.y, vp.max.y, hover, enter_eased);
    }
    if (L.rows() == 0) { draw_empty_state(p, tab, vp); }
    draw_band(p, origin);
    if (drop_point_ && hover < 0 && area.contains(*drop_point_)) { p.dl().rect_outline(area.expanded(-1.0f), pal.accent, m.pane_radius, 2.0f); }
    context_menu(p, tab, vp);
    draw_rename(p, tab, g, origin);
}

} // namespace prism::ui
