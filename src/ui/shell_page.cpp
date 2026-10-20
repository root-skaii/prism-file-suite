#include "ui/shell_page.hpp"

#include <algorithm>
#include <string>

namespace prism::ui {

using namespace strata;

void ShellPage::begin_frame()
{
    if (!drawn_) { path_.clear(); } // ("More options" was closed: next time it opens at the top)
    drawn_ = false;
}

// strata menus cannot leave the window, scroll, or nest well (a third level flips back over its parent and the pointer
// loses it), so: a list taller than the window sits in a scrolling child, and an entry with children (7-Zip, Send to ...)
// opens its list in place, with a row on top that goes back
void ShellPage::draw(context& ui, const std::vector<core::ShellMenuItem>& root)
{
    drawn_ = true;

    // follow the path of opened entries (it can be stale when the entries were read again)
    const std::vector<core::ShellMenuItem>* items = &root;
    std::string title;
    std::size_t depth = 0;
    for (; depth < path_.size(); ++depth) {
        const std::size_t i = path_[depth];
        if (i >= items->size() || (*items)[i].children.empty()) { break; }
        title = (*items)[i].label;
        items = &(*items)[i].children;
    }
    path_.resize(depth);

    if (depth > 0) {
        menu_item_options back;
        back.keep_open = true;
        std::string label = "< " + title;
        for (std::size_t i = 0; (i = label.find('&', i)) != std::string::npos; i += 2) { label.insert(i, 1, '&'); }
        if (ui.menu_item(label + "##back", back)) { path_.pop_back(); }
        ui.menu_separator();
    }

    const float row_h  = env_.settings.compact ? 26.0f : 30.0f;
    const float room   = std::max(row_h * 6.0f, ui.display_size().y - 110.0f);
    const float wanted = row_h * static_cast<float>(items->size());
    if (wanted <= room) {
        draw_items(ui, *items, true);
    } else if (auto body = ui.child("##shell_more" + std::to_string(depth), {0.0f, room}, child_flags::no_padding)) {
        draw_items(ui, *items, true);
    }
}

void ShellPage::draw_items(context& ui, const std::vector<core::ShellMenuItem>& items, bool drill)
{
    for (std::size_t n = 0; n < items.size(); ++n) {
        const core::ShellMenuItem& it = items[n];
        if (it.separator) {
            ui.menu_separator();
            continue;
        }
        std::string label; // ('&' marks a mnemonic in strata menus)
        for (const char c : it.label) {
            label.push_back(c);
            if (c == '&') { label.push_back('&'); }
        }
        if (!it.children.empty() && drill) {
            menu_item_options o;
            o.enabled   = it.enabled;
            o.keep_open = true;
            o.shortcut  = ">";
            if (ui.menu_item(label + "##d" + std::to_string(n), o)) { path_.push_back(n); }
        } else if (!it.children.empty()) {
            if (auto sub = ui.menu(label + "##s" + std::to_string(n), it.enabled)) { draw_items(ui, it.children, false); }
        } else {
            menu_item_options o;
            o.enabled = it.enabled;
            if (it.icon != nullptr) { o.image = env_.images.raw(it.icon->key, it.icon->w, it.icon->h, it.icon->bgra).id; }
            // (entries can share a label: the id keeps their hover animation and clicks apart)
            if (ui.menu_item(label + "##c" + std::to_string(it.id), o)) { menu_.invoke(it.id, env_.window.handle()); }
        }
    }
}

} // namespace prism::ui
