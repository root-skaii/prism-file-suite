# PrismFS

A fast, modern file explorer for Windows, built on [strata](../DxStrataFramework) (Direct3D 11, immediate mode).

- Tabs in a custom title bar (drag to reorder, middle click closes), snap-layout friendly borderless window
- Async directory listing: a 200k-file folder shows its first rows immediately and never blocks the ui
- Only visible rows are drawn, so huge folders scroll like small ones
- Dark (default) and light theme, comfortable and compact density, colour-coded icons
- Keyboard first: arrows / Home / End / PageUp / PageDown, type to find, `Ctrl+T/W/Tab`, `Ctrl+L`, `Alt+←/→/↑`, `F5`

```
prismfs.exe [folder] [--light] [--compact]
```

## Search and filter

The box in the toolbar (`Ctrl+F` focuses it, `Esc` clears it) has two scopes, chosen with the two icon buttons to its
left (hover for the tooltip):

| Scope | What it does |
| --- | --- |
| **This folder** (folder icon) | filters the list you are looking at, as you type |
| **Subfolders** (tree icon) | searches everything below the current folder: press `Enter`; results stream in with the folder they live in shown dimmed after the name. The status bar shows `Searching ... N found` with **Cancel search**, and **Back to folder** afterwards |

`Ctrl+Enter` always searches the subfolders, whichever scope is selected. Open, rename, delete, drag and properties work
on results like on any file. `Aa` matches case, `.*` reads plain words as regular expressions, `?` opens the syntax help
(click an example to try it). A query that cannot be understood turns the box red and says why in the status bar; it never
hides everything silently.

The same query language is used by both scopes. Words are combined with AND; everything is case-insensitive unless `Aa` is on.

| You type | Matches |
| --- | --- |
| `report 2026` | names containing both words |
| `"annual report"` | the exact phrase (inside quotes `*` and `?` are ordinary characters) |
| `*.cpp`, `report?.txt`, `ma*` | wildcards: `*` any run of characters, `?` one character. A wildcard pattern must match the **whole** name; use `*draft*` for "contains" |
| `.cpp`, `.tar.gz` | names that end with it |
| `src\*.h`, `src/util` | contains a path separator: matched against the relative path (useful in Subfolders) |
| `a \| b`, `a OR b` | either side (`a b \| c` means `(a and b) or c`) |
| `-draft`, `-ext:obj` | not (put a `-` in front of any term; quote it, `"-x"`, to search for a real leading dash) |
| `ext:cpp,h` | extension is one of the list |
| `type:image` | `image video audio archive doc text pdf sheet slides code config exe lib font disk shortcut folder` (several: `type:image,video`; `doc` = documents, sheets, slides, pdf and text) |
| `kind:folder`, `kind:file` | folders only / files only |
| `size:>10mb`, `size:<=500kb`, `size:1kb..1mb`, `size:..1gb` | file size; units `b kb mb gb tb` (1024-based, decimals allowed). Folders never match a size |
| `modified:today`, `yesterday` | calendar day (local time) |
| `modified:week`, `month`, `year`, `3d`, `12h`, `2w` | within the last 7 / 30 / 365 days, or the last N days / hours / weeks |
| `modified:2026`, `2026-10`, `2026-10-19` | that year / month / day |
| `modified:>2026-01-01`, `<=2026-03`, `2026-01-01..2026-03-31` | after / before / between (`>` and `<` exclude the date's own period; `>=` and `<=` include it) |
| `hidden:yes` | hidden and system items; they are shown for this query even when hidden files are off |
| `re:^IMG_\d+\.(jpg\|png)$` | a regular expression on the file name (see below) |

Examples: `*.cpp size:>10kb modified:month` · `type:image -ext:gif` · `report* | invoice* kind:file` · `-hidden:yes ext:log`.

Regular expressions (`re:` or the `.*` switch) support literals, `.`, `[a-z]`, `[^x]`, `^`, `$`, `|`, `(...)`, `(?:...)`,
`* + ?`, `{n}`, `{n,m}`, lazy forms (`*?`), `\d \w \s \D \W \S` and escapes. There are no back-references or look-around.
Matching cannot blow up on nasty patterns. With `.*` on, a plain word is a regular expression (so `*.cpp` is an error:
write `\.cpp$`), `|` belongs to the pattern, and a quoted phrase stays literal. Case and Unicode: letter case is folded for
ASCII only, other characters must match exactly.

## Keyboard shortcuts

`Ctrl+Shift+P` opens the command palette: every action with its shortcut, searchable. All shortcuts can be rebound in
**Settings** (they are stored in `settings.ini`).

## Build

Needs CMake 4.3+, Visual Studio 2022 with the C++ workload and the LLVM/clang-cl component, and a strata checkout next
to this folder (`../DxStrataFramework`, or `-DPRISM_STRATA_DIR=...`).

```
cmd /c ".\build_vs.cmd release"       # also: debug, dev (no LTO), relwithdebinfo
build\release\bin\prismfs.exe
```

See [CLAUDE.md](CLAUDE.md) for the architecture and the things that are not obvious from the code.
