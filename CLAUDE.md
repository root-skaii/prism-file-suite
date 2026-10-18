# PrismFS

Fast, modern file explorer for Windows 10/11. C++latest, CMake 4.3+, clang-cl + lld-link, x64. UI is the
[strata](../DxStrataFramework) immediate-mode framework (Direct3D 11), consumed as a library via `add_subdirectory`
(`PRISM_STRATA_DIR`, default `../DxStrataFramework`). Window title / product name is **PrismFS**; the repo folder and
cmake project are still `prism`, the exe is `prismfs.exe`.

## Build / run

- PowerShell only: `cmd /c ".\build_vs.cmd release"` (presets: `debug`, `release` = ThinLTO, `dev` = no LTO for fast
  relinks, `relwithdebinfo`). Bash's `cmd //c` cannot find the batch file. Output: `build/<preset>/bin/prismfs.exe`.
- clang-cl needs the VS environment (INCLUDE / LIB): `build_vs.cmd` loads vcvars; a plain `cmake --preset` from a normal
  shell will not find the Windows SDK.
- Flags: `prismfs.exe [folder] [--light] [--compact]`.
- Look at it: `& .\tools\screenshot.ps1 -Exe build\release\bin\prismfs.exe -Out x.png -ExeArgs @("--light","C:\Windows")`
  (PrintWindow of the app's own window only; never use `CopyFromScreen`, it grabs whatever covers the window).
- No tests yet. Verify behaviour by running it; sort / selection logic is in `model/listing.cpp` (pure, testable).

## Layout

```
src/core/      no ui, no strata: text (utf8/16, natural compare), path, file_kind, file_entry, directory_loader (worker
               thread, batches, cancel), places (known folders, drives), shell_ops (open, properties, clipboard)
src/model/     listing (filtered + sorted view + stable selection), tab (history + loader), tab_set
src/platform/  window (borderless win32: nc hit test, snap layouts, dpi), gpu (d3d11 swap chain + strata renderer)
src/ui/        theme (dark / light palette, comfortable / compact metrics), painter (text, glyph, coloured icons),
               panels: title_bar (tabs + caption buttons), toolbar, sidebar, file_list, status_bar; shell composes them
src/app/       application: frame loop, idle wait, resize / dpi / device loss
```

## Non-obvious things

- **Strata change**: `window_flags::placed` was added to `../DxStrataFramework` (context.hpp + context_window.cpp, ~10
  lines, uncommitted there). Strata windows keep their first size forever; `placed` makes position / size follow the call
  each frame, which is how every panel follows the OS window (`ui/panel.hpp`). Without it nothing resizes.
- No exceptions, no RTTI (matches strata): errors are `bool` / `std::expected`; win32 APIs, not `std::filesystem`.
- The title bar is ours: window is `WS_POPUP|WS_THICKFRAME|...` with `WM_NCCALCSIZE` returning 0. `TitleBar::hit_test`
  answers `WM_NCHITTEST` (caption / maximize button) from last frame's rects. The maximize button is answered by the
  system (snap layouts), so its hover comes from `Window::maximize_hovered()`, not strata.
- Panels lay out by hand (rect arithmetic in `Shell::frame`); interactive things are one `custom_item` canvas per panel
  hit-tested manually, because strata flow layout cannot place at absolute positions. The file list draws only visible
  rows inside a strata `child` that provides scrolling.
- Tab list changes (open / close) are deferred to the end of the frame (`Shell::apply_deferred`) so panels never hold a
  dangling `Tab&`.
- Strata fills text fields with `darken(widget_bg)`, which is grey on the light theme: fields get a transparent fill and
  `Toolbar::field_background` draws the real one at the layout-computed position.
- Glyph sets are baked at start (latin / greek / cyrillic). CJK names would draw `?` until the atlas is rebuilt with more
  ranges: planned (rescan on load, `ui.set_scale`-style rebuild).
- Fonts: Segoe UI Variable Text if `SegUIVar.ttf` exists (Win11), else Segoe UI; icons from Segoe Fluent Icons or Segoe
  MDL2 Assets. Coloured file icons = tinted glyph tiles + a hand-drawn folder (`Painter`).
