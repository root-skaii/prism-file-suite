# usage: tools\screenshot.ps1 -Exe build\release\bin\prismfs.exe -Out shot.png [-WaitMs 3000] [-ExeArgs "--light","--compact","C:\Windows"]
# launches the app, captures ONLY its window (PrintWindow: works while other windows cover it and never copies other
# apps' pixels), then kills it
param([string]$Exe, [string]$Out, [int]$WaitMs = 2500, [string[]]$ExeArgs = @())
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class W {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
"@
[W]::SetProcessDPIAware() | Out-Null
if ($ExeArgs.Count -gt 0) { $p = Start-Process -FilePath $Exe -ArgumentList $ExeArgs -PassThru } else { $p = Start-Process -FilePath $Exe -PassThru }
Start-Sleep -Milliseconds $WaitMs
$p.Refresh()
if ($p.HasExited) { "exited with $($p.ExitCode)"; exit 1 }
$r = New-Object W+RECT
[W]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null
$w = $r.R - $r.L; $h = $r.B - $r.T
$bmp = New-Object System.Drawing.Bitmap $w, $h
$g = [System.Drawing.Graphics]::FromImage($bmp)
$dc = $g.GetHdc()
$ok = [W]::PrintWindow($p.MainWindowHandle, $dc, 2)   # PW_RENDERFULLCONTENT
$g.ReleaseHdc($dc)
# (GDI+ resolves relative paths against the process directory, not PowerShell's current location)
if (-not [IO.Path]::IsPathRooted($Out)) { $Out = Join-Path (Get-Location).Path $Out }
$bmp.Save($Out,[System.Drawing.Imaging.ImageFormat]::Png)
"captured ${w}x${h} (PrintWindow=$ok)"
Stop-Process -Id $p.Id -Force
