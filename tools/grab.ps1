# Screen grab for the stage sweep: captures the game window and writes a downscaled JPEG.
# Kept separate from sweep_stages.ps1 so the capture can be tested on its own:
#
#   powershell -File tools\grab.ps1 -Path shot.jpg -Width 640
#
# GDI (CopyFromScreen) is used rather than OBS: the port presents through a borderless window, not exclusive
# fullscreen, so the desktop compositor has the frame and there is no websocket dependency for an unattended run.
param(
    [Parameter(Mandatory = $true)][string]$Path,
    [int]$Width = 640,
    [int]$Quality = 70,
    [string]$Process = "mgs4"      # which window to capture; the whole screen if it is not up
)
# DPI awareness FIRST, before any window metric is read or any pixel copied. Without it this process is told the
# screen is 2560x1440 when it is really 3840x2160 (150% scaling), while CopyFromScreen still BitBlts the real
# desktop in physical pixels - so every grab was the top-left two thirds of the screen, undistorted (that region
# is 16:9 too) and therefore easy to mistake for a whole frame. It was not.
Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices;
public class Dpi { [DllImport("user32.dll")] public static extern bool SetProcessDPIAware(); }
"@
[Dpi]::SetProcessDPIAware() | Out-Null

Add-Type -AssemblyName System.Drawing, System.Windows.Forms

function Get-Mgs4WindowRect([string]$name) {
    $sig = @"
using System;
using System.Runtime.InteropServices;
public class W {
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
    [StructLayout(LayoutKind.Sequential)] public struct R { public int L, T, Rt, B; }
}
"@
    if (-not ("W" -as [type])) { Add-Type -TypeDefinition $sig }
    $p = Get-Process $name -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    if (-not $p) { return $null }
    $r = New-Object W+R
    if (-not [W]::GetWindowRect($p.MainWindowHandle, [ref]$r)) { return $null }
    if ($r.Rt -le $r.L -or $r.B -le $r.T) { return $null }
    return @{ X = $r.L; Y = $r.T; W = ($r.Rt - $r.L); H = ($r.B - $r.T) }
}

$rect = Get-Mgs4WindowRect $Process
if (-not $rect) {
    $b = [Windows.Forms.Screen]::PrimaryScreen.Bounds
    $rect = @{ X = $b.X; Y = $b.Y; W = $b.Width; H = $b.Height }
}

$bmp = New-Object Drawing.Bitmap($rect.W, $rect.H)
$g = [Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($rect.X, $rect.Y, 0, 0, (New-Object Drawing.Size($rect.W, $rect.H)))
$g.Dispose()

# Downscale on the way out: these are read back by a classifier and by eye, not archived at 7680 wide.
$h = [int][Math]::Round($Width * $bmp.Height / $bmp.Width)
$small = New-Object Drawing.Bitmap($Width, $h)
$gs = [Drawing.Graphics]::FromImage($small)
$gs.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$gs.DrawImage($bmp, 0, 0, $Width, $h)
$gs.Dispose(); $bmp.Dispose()

$codec = [Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | Where-Object { $_.MimeType -eq 'image/jpeg' }
$ps = New-Object Drawing.Imaging.EncoderParameters(1)
$ps.Param[0] = New-Object Drawing.Imaging.EncoderParameter([Drawing.Imaging.Encoder]::Quality, [long]$Quality)
$dir = Split-Path -Parent $Path
if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
$small.Save($Path, $codec, $ps)
$small.Dispose()
"{0}  {1}x{2} -> {3} ({4:N0} bytes)" -f $Path, $rect.W, $rect.H, $Width, (Get-Item $Path).Length
