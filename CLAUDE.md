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
- Tests: `build/<preset>/bin/prismfs_selftest.exe` (core + model + real file ops in %TEMP%, exit 0 = pass). UI is checked by
  screenshot; `--settings` opens the settings page at start.
- Settings: `%APPDATA%\PrismFS\settings.ini`, or `prismfs.ini` next to the exe (portable). Written on every change and at exit.

## Layout

```
src/core/      no ui, no strata: text (utf8/16, natural compare), path, file_kind, file_entry, directory_loader (worker
               thread, batches, cancel), places (known folders, drives), shell_ops (open, properties, clipboard)
src/model/     listing (filtered + sorted view + stable selection), tab (history + loader), tab_set
src/platform/  window (borderless win32: nc hit test, snap layouts, dpi), gpu (d3d11 swap chain + strata renderer)
src/ui/        theme (dark / light palette, comfortable / compact metrics), painter (text, glyph, coloured icons),
               panels: title_bar (tabs + caption buttons), toolbar, sidebar, file_list, status_bar; shell composes them
src/app/       application: frame loop, idle wait, resize / dpi / device loss, font rebuilds, drop target wiring

Also: core/file_ops (IFileOperation on worker threads + clipboard), core/file_info (properties data, FolderScan,
HashJob), core/directory_loader (listing and recursive search), core/query (the search / filter language),
platform/image_cache (shell icons / thumbnails on a worker, uploaded as textures), platform/dnd (OLE drag source + drop
target), ui/widgets (the dialog widget kit), ui/properties_dialog, ui/preview_pane, ui/settings(_dialog). Views:
details or thumbnail grid (`FileList::Geo`), optional second pane (`Env::right`), preview.

- Search / filter: `core::Query::parse(text, options)` (core/query.cpp, pure, tested in selftest) compiles the query
  language once (words AND, quotes, `* ?` wildcards, `.ext`, `|`/OR, `-not`, `ext: type: kind: size: modified: hidden:
  re:`; syntax table in README.md) into a matcher; `matches(FileEntry)` is const and thread-safe. The same Query drives
  the live filter (`Listing::set_query`, "This folder" scope) and the recursive search (`Tab::search(Query)` ->
  `DirectoryLoader::start_search`, "Subfolders" scope; results are relative paths, wildcards / words test the last
  component). An invalid query never filters: it keeps its `error()` which the toolbar (red border) and status bar show.
  Regexes use an own memoised matcher inside query.cpp because std::regex throws and the project has no exceptions.
  The toolbar owns the scope / `Aa` / `.*` state (Settings: search_subfolders / search_case / search_regex).
  `hidden:yes` makes `Listing::visible` show hidden entries for that query.
- Dialogs and modals: strata modals must be shown from outside every window. Panels only record a request
  (`Commands::request_delete`) and `Shell::frame` shows the dialog at its end (`service_delete`, `properties_dialog_`,
  `settings_dialog_`). While a modal is open `Shell::dialog_has_keyboard` stops shortcuts and the file list from seeing
  keys (strata still delivers them to windows behind a modal: Delete or typing would hit the files). Strata's confirm()
  has no Enter handling: the Shell adds it.
- Widget kit (`ui/widgets.hpp`, class `Kit`): buttons (primary / secondary / ghost, icon), icon buttons, pill tabs,
  switches, chips / badges, cards, key / value rows (click copies), stat tiles, bars, swatches. All custom_items drawn
  with Painter, animated hover / press, Tab / Shift+Tab focus ring, disabled state. Dialogs run with `DialogStyle`
  (panel-coloured modal, item spacing 0): rows have fixed heights, so `card_begin(id, title, body_h)` draws its
  background first from the summed row heights (get them right or the card is too short / long). Dialog bodies are a
  fixed-then-measured child (`Kit::cards_height()`, one frame behind) so the dialog follows its content.
- Theme: dark has three surface sets (`DarkStyle`: neutral = default Windows 11-like grey, graphite, slate; Settings
  `dark_style`), each four layers (window < sidebar < card < raised) a clear step apart. Dialog cards use `content` on a
  `sidebar` modal background; pill tracks use `window`.

- Glyphs: `core::text::note_codepoints` collects characters outside the baked ranges; `Application::step` rebuilds the
  context with exactly those glyphs (+ fallback faces for han / kana / hangul / emoji). A rebuild recreates the gpu
  device (textures are forgotten), so it only happens when a new character first appears.
- Drag and drop: `FileList` arms a drag on press-and-move, `Shell::take_drag` hands it to the app, which runs
  `platform::run_drag` after the frame (it has its own message loop). Incoming drops are resolved between frames from
  last frame's rectangles (`Shell::resolve_drop`), and the drop target asks for redraws so highlights show.
- Commands: `ui/actions.cpp` is the one list of named actions (chords live in `strata::keybinds`, stored in
  settings.ini; names never change). `Shell::run_action` runs them for shortcuts and the command palette; file ones go
  to `FileList::perform`. Undo / redo: `model::UndoStack` entries carry their own inverse; `Shell::record_undo` builds
  them from finished `FileOps` jobs (jobs started by undo are `silent`, so they are not recorded again). Undoing a
  delete restores from the Recycle Bin through its own `undelete` verb (`FileOps::restore_from_recycle`).
- Icon: `tools/make_icon.ps1` composes resources/prismfs-white.png on a violet-blue plate into resources/prismfs.ico;
  src/prismfs.rc embeds it (also the window class icon). Rerun the script after changing the artwork.
- Test flags (`--light --compact --grid --preview --split --settings --properties PATH --properties-tab N`) are
  settings: they persist like any change.
- UI checks without a mouse: copy the exe next to an empty `prismfs.ini` (portable settings, your real ones stay
  untouched) and drive it with posted keys / `keybd_event` after `SetForegroundWindow` (modifiers need real key events);
  test flags only persist when something is saved. Code style here: snake_case for functions / members, PascalCase types.
- Edits to sources: use the Edit / Write tools, never perl / sed with backslashes or `\n` (it mangled paths and
  string escapes several times).
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
