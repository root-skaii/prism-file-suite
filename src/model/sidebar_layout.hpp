#pragma once

// what the sidebar shows, as the user arranged it: ordered sections, each with pinned folders (or the drive list).
// pure data and edits, no ui. every edit that changes something bumps revision(), which is how the owner knows to save

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace strata { class config; }

namespace prism::model {

struct SidebarItem {
    std::string   path;      // the folder, normalized
    std::string   name;      // label; empty: the folder's own name
    std::string   known;     // "downloads": the path is looked up again on every start, so it follows a moved folder
    char32_t      glyph{};   // icon font code point, 0: automatic
    std::uint32_t color{};   // 0xAARRGGBB, 0: automatic
    std::string   query{};   // a saved search: the query text, run below `path` (empty: a plain pinned folder)

    [[nodiscard]] std::string label() const;
};

struct SidebarSection {
    std::string              title;
    bool                     collapsed{};
    bool                     drives{};   // the drive list (This PC) instead of pinned folders
    enum class Auto : std::uint8_t { none, recent, frequent };
    Auto                     auto_kind{}; // a list the app fills from where you have been (never holds pins)
    std::vector<SidebarItem> items;

    // a section the app fills: it takes no pins and nothing is dragged into it
    [[nodiscard]] bool dynamic() const noexcept { return drives || auto_kind != Auto::none; }
};

struct ItemPos {
    std::size_t section{};
    std::size_t index{};
};

class SidebarLayout {
public:
    // Quick access (the well-known folders) and This PC
    [[nodiscard]] static SidebarLayout defaults();

    [[nodiscard]] const std::vector<SidebarSection>& sections() const noexcept { return sections_; }
    [[nodiscard]] std::size_t revision() const noexcept { return revision_; }

    [[nodiscard]] std::optional<ItemPos> find(std::string_view path) const;
    // where a plain "pin this" goes: the first folder section (one is made when there is none)
    [[nodiscard]] std::size_t pin_section();

    // --- edits. index / slot arguments are insertion slots (0 = before the first); out of range = the end -----------

    // false: not a folder section, or the folder is already pinned somewhere
    bool pin(std::size_t section, std::size_t slot, std::string path, std::string name = {});
    // pins a search: clicking it opens `folder` and runs `query` below it. false: not a folder section / already saved
    bool pin_search(std::size_t section, std::string folder, std::string query, std::string name = {});
    bool unpin(std::string_view path);
    bool remove_item(ItemPos at);
    // false: nothing changed (same place, or the target is the drive list)
    bool move_item(ItemPos from, std::size_t to_section, std::size_t to_slot);
    bool rename_item(ItemPos at, std::string name);
    bool set_item_style(ItemPos at, char32_t glyph, std::uint32_t color);

    std::size_t add_section(std::size_t slot, std::string title);
    // the pins of a removed folder section move to its neighbour (they are dropped when it was the only one, or when
    // `keep_items` is false)
    bool remove_section(std::size_t section, bool keep_items = true);
    // whether another folder section could take the pins of `section` when it is removed
    [[nodiscard]] bool has_neighbour(std::size_t section) const;
    bool rename_section(std::size_t section, std::string title);
    bool set_collapsed(std::size_t section, bool collapsed);
    bool move_section(std::size_t from, std::size_t slot);
    bool add_drives_section(std::size_t slot);
    bool add_auto_section(std::size_t slot, SidebarSection::Auto kind);
    [[nodiscard]] bool has_auto(SidebarSection::Auto kind) const noexcept;

    // drives left out of the drive list (a card reader, a backup disk): by root, "E:\\"
    bool hide_drive(std::string_view root);
    bool show_drive(std::string_view root);
    [[nodiscard]] bool drive_hidden(std::string_view root) const;
    [[nodiscard]] const std::vector<std::string>& hidden_drives() const noexcept { return hidden_drives_; }

    // takes another layout whole (a pasted one), collapsed state included
    void replace(const SidebarLayout& other);
    [[nodiscard]] bool has_drives() const noexcept;

    // back to Quick access + This PC
    void reset();
    // puts back an older shape (undo / redo) but keeps what is expanded now: collapsing is not an edit to take back
    void restore(const SidebarLayout& snapshot);

    void store(strata::config& c) const;
    // false: no layout in there
    [[nodiscard]] bool load(const strata::config& c);
    [[nodiscard]] bool load_file(const std::string& path);
    [[nodiscard]] bool save_file(const std::string& path) const;

private:
    std::vector<SidebarSection> sections_;
    std::vector<std::string>    hidden_drives_;
    std::size_t                 revision_{};
};

} // namespace prism::model
