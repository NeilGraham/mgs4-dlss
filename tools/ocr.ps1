# Reads text out of images with the OCR engine built into Windows (Windows.Media.Ocr). No install and no service:
# the engine ships with the OS and is the same one Snipping Tool uses.
#
#   powershell -File tools\ocr.ps1 -Images shot.jpg
#   powershell -File tools\ocr.ps1 -List paths.txt -Json out.json
#   powershell -File tools\ocr.ps1 -Images shot.jpg -Top 0.0 -Bottom 0.18      only the top of the frame
#
# One process handles the whole list: creating the engine and the WinRT plumbing costs about a second, which is
# not worth paying per image when there are a few hundred of them.
#
# -Top/-Bottom/-Left/-Right crop before recognising, as fractions of the frame. Cropping matters: the engine
# returns everything it can find, and a scene full of signage drowns the one line that was wanted. It also helps
# accuracy, because the crop is upscaled first - small HUD text reads far better at 3x.
param(
    [string[]]$Images = @(),
    [string]$List = "",
    [string]$Json = "",
    [double]$Top = 0.0,
    [double]$Bottom = 1.0,
    [double]$Left = 0.0,
    [double]$Right = 1.0,
    [int]$Upscale = 3
)
$ErrorActionPreference = "Stop"

Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices;
public class OcrDpi { [DllImport("user32.dll")] public static extern bool SetProcessDPIAware(); }
"@
[OcrDpi]::SetProcessDPIAware() | Out-Null
Add-Type -AssemblyName System.Drawing

$null = [Windows.Storage.StorageFile, Windows.Storage, ContentType = WindowsRuntime]
$null = [Windows.Graphics.Imaging.BitmapDecoder, Windows.Graphics.Imaging, ContentType = WindowsRuntime]
$null = [Windows.Media.Ocr.OcrEngine, Windows.Media.Ocr, ContentType = WindowsRuntime]
Add-Type -AssemblyName System.Runtime.WindowsRuntime
$asTask = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
        $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
        $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' })[0]
function Await($op, $type) {
    $t = $asTask.MakeGenericMethod($type).Invoke($null, @($op)); $t.Wait(-1) | Out-Null; $t.Result
}

$engine = [Windows.Media.Ocr.OcrEngine]::TryCreateFromUserProfileLanguages()
if (-not $engine) { throw "no OCR language pack is installed for this user" }

if ($List) { $Images = @(Get-Content $List | Where-Object { $_.Trim() }) }
$tmpDir = Join-Path $env:TEMP "mgs4ocr"
New-Item -ItemType Directory -Force $tmpDir | Out-Null

$out = @{}
foreach ($img in $Images) {
    $text = ""
    try {
        $src = [Drawing.Image]::FromFile((Resolve-Path $img).Path)
        $x = [int]($src.Width * $Left); $y = [int]($src.Height * $Top)
        $w = [Math]::Max(1, [int]($src.Width * ($Right - $Left)))
        $h = [Math]::Max(1, [int]($src.Height * ($Bottom - $Top)))
        # Crop, then upscale: the engine has a minimum size it will look at, and HUD text on a 1080p frame is
        # under it. Bicubic rather than nearest - the glyphs are anti-aliased already.
        # Each argument parenthesised: in PowerShell "," binds tighter than "*", so ($w * $Upscale, $h * $Upscale)
        # parses as $w * ($Upscale, $h) * $Upscale - a multiply against an array, which fails at runtime.
        $crop = New-Object Drawing.Bitmap(($w * $Upscale), ($h * $Upscale))
        $g = [Drawing.Graphics]::FromImage($crop)
        $g.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $g.DrawImage($src, (New-Object Drawing.Rectangle(0, 0, ($w * $Upscale), ($h * $Upscale))),
                     (New-Object Drawing.Rectangle($x, $y, $w, $h)), [Drawing.GraphicsUnit]::Pixel)
        $g.Dispose(); $src.Dispose()
        $tmp = Join-Path $tmpDir ("c_" + [guid]::NewGuid().ToString("N") + ".png")
        $crop.Save($tmp, [Drawing.Imaging.ImageFormat]::Png); $crop.Dispose()

        $sf = Await ([Windows.Storage.StorageFile]::GetFileFromPathAsync($tmp)) ([Windows.Storage.StorageFile])
        $st = Await ($sf.OpenAsync([Windows.Storage.FileAccessMode]::Read)) ([Windows.Storage.Streams.IRandomAccessStream])
        $dec = Await ([Windows.Graphics.Imaging.BitmapDecoder]::CreateAsync($st)) ([Windows.Graphics.Imaging.BitmapDecoder])
        $bmp = Await ($dec.GetSoftwareBitmapAsync()) ([Windows.Graphics.Imaging.SoftwareBitmap])
        $res = Await ($engine.RecognizeAsync($bmp)) ([Windows.Media.Ocr.OcrResult])
        $text = $res.Text
        $st.Dispose(); Remove-Item $tmp -ErrorAction SilentlyContinue
    } catch {
        $text = ""
        if ($env:MGS4_OCR_DEBUG) { Write-Host ("ocr error on {0}: {1}" -f $img, $_.Exception.Message) }
    }
    $key = [IO.Path]::GetFileNameWithoutExtension($img)
    $out[$key] = $text
    if (-not $Json) { "{0}`t{1}" -f $key, $text }
}

if ($Json) {
    $out | ConvertTo-Json -Depth 3 | Set-Content $Json -Encoding utf8
    "{0} images -> {1}" -f $Images.Count, $Json
}
