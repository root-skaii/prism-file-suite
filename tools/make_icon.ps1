# usage: powershell -File tools\make_icon.ps1 [-Src resources\prismfs-white.png] [-Out resources\prismfs.ico]
# composes the logo onto a colourful rounded plate (readable on light and dark taskbars) and writes a multi-size .ico
# (png-compressed entries, windows vista+). rerun after changing the artwork; the build embeds the .ico.
param(
    [string]$Src = (Join-Path $PSScriptRoot "..\resources\prismfs-white.png"),
    [string]$Out = (Join-Path $PSScriptRoot "..\resources\prismfs.ico"),
    [string]$PreviewDir = ""   # also write icon-<size>.png there (to look at the result)
)
Add-Type -AssemblyName System.Drawing
$art = [System.Drawing.Image]::FromFile((Resolve-Path $Src))

# the artwork's bounds inside the 1254 x 1254 canvas (the rest is transparent padding and glow)
$crop = New-Object System.Drawing.Rectangle 250, 90, 820, 1070

function New-Plate([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.InterpolationMode = 'HighQualityBicubic'
    $g.PixelOffsetMode = 'Half'
    $g.Clear([System.Drawing.Color]::Transparent)

    # rounded plate with a slight vertical gradient
    $r = [single]($size * 0.22)
    $d = $r * 2
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $path.AddArc(0, 0, $d, $d, 180, 90)
    $path.AddArc($size - $d - 1, 0, $d, $d, 270, 90)
    $path.AddArc($size - $d - 1, $size - $d - 1, $d, $d, 0, 90)
    $path.AddArc(0, $size - $d - 1, $d, $d, 90, 90)
    $path.CloseFigure()
    # a vivid violet-to-blue plate (the app's accent family): the white crystal pops on it, and the colourful plate
    # stays visible on light and dark taskbars alike. small sizes get a bigger logo
    $small = $size -le 48
    $top = [System.Drawing.Color]::FromArgb(255, 88, 60, 255)
    $bottom = [System.Drawing.Color]::FromArgb(255, 20, 150, 255)
    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush ((New-Object System.Drawing.Point 0, 0), (New-Object System.Drawing.Point $size, $size), $top, $bottom)
    $g.FillPath($brush, $path)

    # the logo, aspect kept, centred with a margin
    $box = $size * $(if ($small) { 0.90 } else { 0.84 })
    $k = [Math]::Min($box / $crop.Width, $box / $crop.Height)
    $w = $crop.Width * $k
    $h = $crop.Height * $k
    $dest = New-Object System.Drawing.RectangleF (($size - $w) / 2), (($size - $h) / 2), $w, $h
    $g.DrawImage($art, $dest, $crop, [System.Drawing.GraphicsUnit]::Pixel)
    $g.Dispose()
    return $bmp
}

$sizes = 256, 128, 64, 48, 32, 24, 16
$pngs = @()
foreach ($s in $sizes) {
    $bmp = New-Plate $s
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $pngs += , ($ms.ToArray())
    if ($PreviewDir -ne "") { $bmp.Save((Join-Path $PreviewDir "icon-$s.png"), [System.Drawing.Imaging.ImageFormat]::Png) }
    $bmp.Dispose()
}

$fs = [System.IO.File]::Create($Out)
$bw = New-Object System.IO.BinaryWriter $fs
$bw.Write([UInt16]0); $bw.Write([UInt16]1); $bw.Write([UInt16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]
    $bw.Write([byte]($(if ($s -ge 256) { 0 } else { $s })))
    $bw.Write([byte]($(if ($s -ge 256) { 0 } else { $s })))
    $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([UInt16]1); $bw.Write([UInt16]32)
    $bw.Write([UInt32]$pngs[$i].Length); $bw.Write([UInt32]$offset)
    $offset += $pngs[$i].Length
}
foreach ($p in $pngs) { $bw.Write($p) }
$bw.Close()
$art.Dispose()
"wrote $Out ($($sizes -join ', ') px)"
