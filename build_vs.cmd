@echo off
rem usage: build_vs.cmd <preset> [target]      (debug | release | dev | relwithdebinfo)
rem loads the visual studio x64 environment (clang-cl needs its INCLUDE / LIB), then configures + builds.
setlocal

if defined VSCMD_VER goto :build

set "VSINSTALLER=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
if exist "%VSINSTALLER%\vswhere.exe" set "PATH=%VSINSTALLER%;%PATH%"

set "VSROOT="
for /f "usebackq delims=" %%i in (`vswhere.exe -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do set "VSROOT=%%i"
if not defined VSROOT (
    echo build_vs: no visual studio with the C++ x64 tools found 1>&2
    exit /b 1
)
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

:build
cd /d "%~dp0"
rem a PrismFS kept alive in the background (tray / "start faster", no window) keeps the exe locked: stop this tree's windowless ones before linking
powershell -NoProfile -Command "Get-Process prismfs -ErrorAction SilentlyContinue | Where-Object { $_.Path -like '%~dp0build\*' -and $_.MainWindowHandle -eq 0 } | Stop-Process -Force" >nul 2>&1
cmake --preset %1 || exit /b 1
if "%2"=="" (cmake --build --preset %1) else (cmake --build --preset %1 --target %2)
