# Builds mgs4-dlss.exe from tools\app_launcher.cs, wearing the game's own icon.
#
#   powershell -ExecutionPolicy Bypass -File tools\build_app_exe.ps1
#
# Nothing has to be installed: the C# compiler used here ships with Windows as part of the .NET Framework, and the
# icon is read out of the mgs4.exe already on this machine. That is also why the built exe is not in the repo - the
# icon inside it is Konami's artwork, so it is made locally rather than redistributed.
param(
    [string]$GameDir = "",      # default: MGS4_DIR / config.ini / the Steam libraries (tools\paths.ps1)
    [string]$Out = "",          # default: mgs4-dlss.exe in the repo root
    [switch]$NoIcon             # build without one (the game folder is not needed then)
)
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\paths.ps1"

$repo = Split-Path -Parent $PSScriptRoot
if (-not $Out) { $Out = Join-Path $repo "mgs4-dlss.exe" }
$source = Join-Path $PSScriptRoot "app_launcher.cs"
if (-not (Test-Mgs4Path $source)) { throw "missing $source" }

$csc = Join-Path $env:WINDIR "Microsoft.NET\Framework64\v4.0.30319\csc.exe"
if (-not (Test-Mgs4Path $csc)) { $csc = Join-Path $env:WINDIR "Microsoft.NET\Framework\v4.0.30319\csc.exe" }
if (-not (Test-Mgs4Path $csc)) {
    throw "no C# compiler found under $env:WINDIR\Microsoft.NET - install the .NET Framework 4 target, or run with -NoIcon from a machine that has it."
}

# ---------------------------------------------------------------------------------------------- the icon

Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices;
public static class Mgs4Icons {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)]
  public static extern int PrivateExtractIcons(string file, int index, int cx, int cy, IntPtr[] icons, int[] ids, int count, int flags);
  [DllImport("user32.dll")] public static extern bool DestroyIcon(IntPtr h);
}
"@ -ErrorAction SilentlyContinue

# One .ico holding every size the exe wants, each stored as a PNG (the Vista-and-later icon format, which Windows
# reads at any size - not just at 256).
function Write-IconFile([string]$fromExe, [string]$icoPath) {
    $sizes = @(16, 24, 32, 48, 64, 128, 256)
    $blobs = @()
    foreach ($s in $sizes) {
        $handles = New-Object IntPtr[] 1
        $ids = New-Object int[] 1
        $n = [Mgs4Icons]::PrivateExtractIcons($fromExe, 0, $s, $s, $handles, $ids, 1, 0)
        if ($n -le 0 -or $handles[0] -eq [IntPtr]::Zero) { continue }
        try {
            $icon = [System.Drawing.Icon]::FromHandle($handles[0])
            $bmp = $icon.ToBitmap()
            if ($bmp.Width -ne $s -or $bmp.Height -ne $s) {
                $scaled = New-Object System.Drawing.Bitmap $s, $s
                $g = [System.Drawing.Graphics]::FromImage($scaled)
                $g.InterpolationMode = "HighQualityBicubic"
                $g.DrawImage($bmp, 0, 0, $s, $s)
                $g.Dispose(); $bmp.Dispose(); $bmp = $scaled
            }
            $ms = New-Object System.IO.MemoryStream
            $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
            $blobs += , @{ Size = $s; Bytes = $ms.ToArray() }
            $ms.Dispose(); $bmp.Dispose(); $icon.Dispose()
        } finally { [void][Mgs4Icons]::DestroyIcon($handles[0]) }
    }
    if ($blobs.Count -eq 0) { return $false }

    $fs = [System.IO.File]::Create($icoPath)
    $bw = New-Object System.IO.BinaryWriter $fs
    try {
        $bw.Write([uint16]0); $bw.Write([uint16]1); $bw.Write([uint16]$blobs.Count)   # ICONDIR
        $offset = 6 + 16 * $blobs.Count
        foreach ($b in $blobs) {
            $dim = $(if ($b.Size -ge 256) { 0 } else { $b.Size })                      # 0 means 256
            $bw.Write([byte]$dim); $bw.Write([byte]$dim)
            $bw.Write([byte]0); $bw.Write([byte]0)
            $bw.Write([uint16]1); $bw.Write([uint16]32)
            $bw.Write([uint32]$b.Bytes.Length); $bw.Write([uint32]$offset)
            $offset += $b.Bytes.Length
        }
        foreach ($b in $blobs) { $bw.Write($b.Bytes) }
    } finally { $bw.Dispose(); $fs.Dispose() }
    return $true
}

$icoArg = ""
$ico = Join-Path ([IO.Path]::GetTempPath()) "mgs4_dlss_app.ico"
if (-not $NoIcon) {
    if (-not $GameDir) { try { $GameDir = Get-Mgs4GameDir } catch { $GameDir = $null } }
    $gameExe = $(if ($GameDir) { Join-Mgs4Path $GameDir "mgs4.exe" } else { $null })
    if ($gameExe -and (Test-Mgs4Path $gameExe) -and (Write-IconFile $gameExe $ico)) {
        $icoArg = "/win32icon:$ico"
        Write-Host "icon taken from $gameExe"
    } else {
        Write-Host "no mgs4.exe found - building without an icon (pass -GameDir, or -NoIcon to stop asking)" -ForegroundColor Yellow
    }
}

# ---------------------------------------------------------------------------------------------- compile

$args = @("/nologo", "/target:winexe", "/platform:anycpu", "/optimize+", "/out:$Out",
          "/reference:System.dll", "/reference:System.Drawing.dll")
if ($icoArg) { $args += $icoArg }
$args += $source

& $csc @args
if ($LASTEXITCODE -ne 0) { throw "csc failed ($LASTEXITCODE)" }
Remove-Item -LiteralPath $ico -ErrorAction SilentlyContinue

$size = [math]::Round((Get-Item -LiteralPath $Out).Length / 1KB)
Write-Host "built $Out ($size KB)"
