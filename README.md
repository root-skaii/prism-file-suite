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

## Build

Needs CMake 4.3+, Visual Studio 2022 with the C++ workload and the LLVM/clang-cl component, and a strata checkout next
to this folder (`../DxStrataFramework`, or `-DPRISM_STRATA_DIR=...`).

```
cmd /c ".\build_vs.cmd release"       # also: debug, dev (no LTO), relwithdebinfo
build\release\bin\prismfs.exe
```

See [CLAUDE.md](CLAUDE.md) for the architecture and the things that are not obvious from the code.
