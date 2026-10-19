# usage: tools\make_screenshots.ps1 [-Exe build\release\bin\prismfs.exe] [-Out docs]
# Builds a made-up demo tree on a throwaway subst drive and runs a portable copy of the exe against it, so the
# README images never show real folders, drives or user names.
param([string]$Exe = "build\release\bin\prismfs.exe", [string]$Out = "docs")
$ErrorActionPreference = "Stop"
if (-not [IO.Path]::IsPathRooted($Out)) { $Out = Join-Path (Get-Location).Path $Out }
if (-not [IO.Path]::IsPathRooted($Exe)) { $Exe = Join-Path (Get-Location).Path $Exe }
Add-Type -AssemblyName System.Drawing
$root = "C:\PrismDemo"
$work = Join-Path $env:TEMP "prismfs-demo-app"
Remove-Item $root, $work -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory $root, $work, $Out -Force | Out-Null
$d = "$root\"

function Fill($path, [int]$kb) { $b = New-Object byte[] ($kb * 1024); (New-Object Random 7).NextBytes($b); [IO.File]::WriteAllBytes($path, $b) }
function Pic($path, [int]$h1, [int]$h2, [int]$w = 640, [int]$h = 420) {
  $bmp = New-Object Drawing.Bitmap $w, $h
  $g = [Drawing.Graphics]::FromImage($bmp)
  $c1 = [Drawing.Color]::FromArgb(255, $h1 % 256, 90, 200 - ($h1 % 120)); $c2 = [Drawing.Color]::FromArgb(255, 30, $h2 % 256, 120)
  $br = New-Object Drawing.Drawing2D.LinearGradientBrush ([Drawing.Point]::new(0,0)), ([Drawing.Point]::new($w,$h)), $c1, $c2
  $g.FillRectangle($br, 0, 0, $w, $h)
  $sun = New-Object Drawing.SolidBrush ([Drawing.Color]::FromArgb(70, 255, 255, 255))
  $g.FillEllipse($sun, $w * 0.55, $h * 0.12, $h * 0.45, $h * 0.45)
  $hill = New-Object Drawing.SolidBrush ([Drawing.Color]::FromArgb(110, 10, 20, 40))
  $g.FillPolygon($hill, @([Drawing.Point]::new(0,$h), [Drawing.Point]::new($w*0.3,$h*0.55), [Drawing.Point]::new($w*0.6,$h), [Drawing.Point]::new($w*0.8,$h*0.65), [Drawing.Point]::new($w,$h)))
  $bmp.Save($path, [Drawing.Imaging.ImageFormat]::Jpeg); $g.Dispose(); $bmp.Dispose()
}

foreach ($n in "Projects", "Photos", "Documents", "Music", "Downloads") { New-Item -ItemType Directory "$d$n" | Out-Null }
foreach ($n in "aurora", "terminal-ui", "notes-app", "website") { New-Item -ItemType Directory "${d}Projects\$n" | Out-Null }
New-Item -ItemType Directory "${d}Photos\Iceland 2026", "${d}Photos\Family", "${d}Photos\Screenshots" | Out-Null

$names = "glacier-lagoon", "black-beach", "northern-lights", "waterfall", "highlands", "campfire", "harbour", "sunrise-ridge", "old-church", "geyser", "moss-field", "city-night"
$i = 0; foreach ($n in $names) { Pic "${d}Photos\Iceland 2026\$n.jpg" (40 + $i * 37) (60 + $i * 53); $i++ }

$src = @{
  "main.cpp"   = "#include <cstdio>`n`nint main()`n{`n    std::puts(`"hello from aurora`");`n    return 0;`n}`n"
  "README.md"  = "# Aurora`n`nA tiny demo project.`n`n- fast`n- small`n- tidy`n"
  "CMakeLists.txt" = "cmake_minimum_required(VERSION 3.28)`nproject(aurora CXX)`nadd_executable(aurora main.cpp)`n"
}
foreach ($k in $src.Keys) { Set-Content "${d}Projects\aurora\$k" $src[$k] -Encoding utf8 }
Fill "${d}Projects\aurora\aurora.exe" 840
Set-Content "${d}Projects\aurora\config.json" '{ "theme": "dark", "fps": 144, "vsync": true }' -Encoding utf8

$files = @(
 @("Documents","Annual report 2026.pdf",2400), @("Documents","Budget.xlsx",180), @("Documents","Meeting notes.docx",64),
 @("Documents","Pitch deck.pptx",5200), @("Documents","Invoice 0412.pdf",310), @("Documents","Travel plan.txt",4), @("Documents","Contract draft.docx",96),
 @("Music","Midnight Drive.mp3",7400), @("Music","Slow Waves.flac",31000), @("Music","Neon Streets.mp3",6900),
 @("Downloads","installer-x64.msi",48200), @("Downloads","dataset.zip",15700), @("Downloads","backup-2026-09.7z",92000), @("Downloads","wallpaper.png",2100), @("Downloads","fonts.zip",3300),
 @("Projects","todo.txt",1), @("Projects","design-tokens.json",12)
)
foreach ($f in $files) { Fill "$d$($f[0])\$($f[1])" $f[2] }
$when = (Get-Date).AddDays(-1)
Get-ChildItem $root -Recurse -File | ForEach-Object { $when = $when.AddHours(-7.3); $_.LastWriteTime = $when }

# portable settings + a sidebar that only knows the demo tree (no drives, no real known folders)
Copy-Item $Exe "$work\prismfs.exe"
$sb = @("[sidebar]", "sections = 3", "hidden_drives = D:\|R:\", "[sidebar.0]", "title = Favourites", "kind = folders", "collapsed = 0", "items = 5")
$fav = "Projects", "Photos", "Documents", "Music", "Downloads"
for ($k = 0; $k -lt 5; $k++) { $sb += "i$k.path = $d$($fav[$k])" }
$sb += "[sidebar.1]", "title = Projects", "kind = folders", "collapsed = 0", "items = 4"
$pr = "aurora", "terminal-ui", "notes-app", "website"
for ($k = 0; $k -lt 4; $k++) { $sb += "i$k.path = ${d}Projects\$($pr[$k])" }
$sb += "[sidebar.2]", "title = Drives", "kind = drives", "collapsed = 0", "items = 0"
Set-Content "$work\prismfs-sidebar.ini" $sb -Encoding ascii

function Shot($name, $ini, $folder, $args2) {
  Set-Content "$work\prismfs.ini" (@("[behavior]", "track_history = 0", "restore_tabs = 0", "standby = 0", "tray = 0", "[layout]", "window_w = 1360", "window_h = 820") + $ini) -Encoding ascii
  Remove-Item "$work\prismfs-visits.ini" -ErrorAction SilentlyContinue
  & "$PSScriptRoot\screenshot.ps1" -Exe "$work\prismfs.exe" -Out (Join-Path $Out $name) -WaitMs 6000 -ExeArgs (@("`"$folder`"") + $args2)
}
Shot "dark-details.png"  @() "${d}Downloads" @()
Shot "dark-grid.png"     @() "${d}Photos\Iceland 2026" @("--grid")
Remove-Item $root, $work -Recurse -Force
