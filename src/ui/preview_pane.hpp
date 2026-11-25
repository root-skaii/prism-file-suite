#pragma once

// the right-hand pane: a large thumbnail (or icon) of the selected item, its facts, and the start of a text file

#include "core/file_info.hpp"
#include "core/syntax.hpp"
#include "ui/env.hpp"
#include "ui/painter.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace prism::ui {

// the first `max_bytes` of a file as lines (tabs expanded); empty if it does not look like text (a NUL byte)
[[nodiscard]] std::vector<std::string> read_text_head(const std::string& path, std::size_t max_bytes, std::size_t max_lines);
// the first `max_bytes` of a file as raw bytes (for hex preview of binary files)
[[nodiscard]] std::vector<unsigned char> read_raw_head(const std::string& path, std::size_t max_bytes);
// theme-harmonised colour for a syntax token kind
[[nodiscard]] strata::color token_color(core::TokenKind kind, const Palette& pal, bool dark) noexcept;
// draws one syntax-highlighted line clipped to `bounds`
void draw_highlighted_line(Painter& p, const strata::rect& bounds, const core::HighlightedLine& line, strata::font_id font, bool dark);

class PreviewPane {
public:
    explicit PreviewPane(Env& env) : env_{env} {}
    ~PreviewPane() { cancel_load(); }
    void frame(Painter& p, const strata::rect& area);

private:
    struct LoadResult {
        std::string                        path;
        core::FileInfo                     info;
        core::FileKind                     kind{core::FileKind::other};
        std::vector<core::HighlightedLine> lines;
        std::vector<std::string>           hex_lines;
        std::atomic<bool>                  done{false};
        std::atomic<bool>                  cancel{false};
    };

    void load(const std::string& path);
    void cancel_load() noexcept;
    void poll_load();

    Env&                               env_;
    std::string                        shown_;      // target currently displayed or loading
    core::FileInfo                     info_;
    core::FileKind                     kind_{core::FileKind::other};
    std::vector<core::HighlightedLine> lines_;      // syntax-highlighted text head
    std::vector<std::string>           hex_lines_;  // hex dump fallback for non-pictured binary files
    std::unique_ptr<core::FolderScan>  scan_;
    std::shared_ptr<LoadResult>        pending_;
};

} // namespace prism::ui
