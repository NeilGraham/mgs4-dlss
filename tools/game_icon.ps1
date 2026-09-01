# The game's icon as a .ico file, for a build to embed. Dot-source this and call Write-Mgs4IconFile.
#
#   . "$PSScriptRoot\game_icon.ps1"
#   if (Write-Mgs4IconFile $gameExe $icoPath) { ... /win32icon:$icoPath ... }
#
# One .ico holding every size the exe wants, each stored as a PNG (the Vista-and-later icon format, which Windows
# reads at any size - not just at 256). Both builds - tools\build_app_exe.ps1 for the PowerShell app's wrapper and
# launcher\build.ps1 for the C# app - take the icon this way, so the two exes wear the same one.
#
# The icon is read out of the mgs4.exe already on the machine, which is why neither exe is committed: that artwork
# is Konami's, so it is built locally rather than redistributed.

Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices;
public static class Mgs4Icons {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)]
  public static extern int PrivateExtractIcons(string file, int index, int cx, int cy, IntPtr[] icons, int[] ids, int count, int flags);
  [DllImport("user32.dll")] public static extern bool DestroyIcon(IntPtr h);
}
"@ -ErrorAction SilentlyContinue

function Write-Mgs4IconFile([string]$fromExe, [string]$icoPath) {
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
