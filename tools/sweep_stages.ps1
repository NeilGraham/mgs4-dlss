# Boots every stage id in turn, records the run to video, and cuts stills out of it, so the catalog is measured
# rather than guessed. There is no rule to guess with: s01a00l_1 crashes and s02a20l_1 boots, and the two are
# identical on every readable signal (see docs/launcher.md).
#
#   powershell -File tools\sweep_stages.ps1                 every catalog id (420)
#   powershell -File tools\sweep_stages.ps1 -Ids my.txt     one id per line
#   powershell -File tools\sweep_stages.ps1 -NoVideo        stills only, no recording kept
#
# Takes over the display and the keyboard for the whole run, so run it unattended. Resumable: ids already in the
# output file are skipped, so Ctrl+C and re-run is safe.
#
# Why video rather than two screen grabs. A recording can be re-cut at any offset afterwards without booting the
# game again, which is the expensive part - the first pass of this sweep was thrown away entirely because the
# grabs were wrong, and that cost hours that a recording would have made a one-minute re-extract.
#
# Output
#   tools\stage_probe.csv            id,result,seconds,scene_offset,video,frames,detail
#   <MGS4_OUT>\sweep\video\<id>.mkv  the run, from launch
#   <MGS4_OUT>\sweep\<id>_t01.jpg    1 s after the first 3D frame - the location banner ("CHECKPOINT >> ...")
#   <MGS4_OUT>\sweep\<id>_t10.jpg    10 s in
#   <MGS4_OUT>\sweep\<id>_t20.jpg    20 s in - the frame the classifier judges
#   <MGS4_OUT>\sweep\<id>_t30.jpg    30 s in - a cutscene has usually moved on by here
param(
    [string]$Ids = "",
    [string]$Out = "",
    [string]$ShotDir = "",
    [string]$VideoDir = "",
    [switch]$NoVideo,
    [string]$Offsets = "1,10,20,30",
    [int]$RecordSeconds = 33,
    [int]$BootTimeout = 55,
    [int]$Width = 1920,
    [int]$Fps = 10
)
$ErrorActionPreference = "Continue"

# DPI awareness before anything reads a window size. Without it this process is told the screen is 2560x1440 when
# it is 3840x2160, and every capture is the top-left two thirds of it - undistorted, because that region is 16:9
# too, which is what let the first pass of this sweep go unnoticed for hours.
Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices;
public class SweepDpi {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
}
"@
[SweepDpi]::SetProcessDPIAware() | Out-Null
$scrW = [SweepDpi]::GetSystemMetrics(0); $scrH = [SweepDpi]::GetSystemMetrics(1)

. "$PSScriptRoot\paths.ps1"
$p = Get-Mgs4Paths
$repo = $p.Repo; $game = $p.GameDir; $ff = $p.FFmpeg
if (-not $game -or -not (Test-Path (Join-Path $game "mgs4.exe"))) { throw "game folder not found (set MGS4_DIR in config.ini)" }
if (-not $ff -or -not (Test-Path $ff)) { throw "ffmpeg not found (set MGS4_FFMPEG in config.ini)" }
$exe = Join-Path $repo "mgs4-dlss-launcher.exe"
if (-not (Test-Path $exe)) { throw "$exe not found - build it with launcher\build.ps1" }
if (-not $Out) { $Out = Join-Path $PSScriptRoot "stage_probe.csv" }
if (-not $ShotDir) { $ShotDir = Join-Path $p.OutDir "sweep" }
if (-not $VideoDir) { $VideoDir = Join-Path $ShotDir "video" }
New-Item -ItemType Directory -Force $ShotDir | Out-Null
New-Item -ItemType Directory -Force $VideoDir | Out-Null
$offsetList = @($Offsets -split "," | ForEach-Object { [int]$_.Trim() })

if ($Ids) {
    $list = Get-Content $Ids | ForEach-Object { $_.Trim() } | Where-Object { $_ -and -not $_.StartsWith("#") }
} else {
    $list = Get-Content (Join-Path $PSScriptRoot "scenes.csv") | Select-Object -Skip 1 |
            ForEach-Object { ($_ -split ",")[0].Trim() } | Where-Object { $_ }
}
$list = $list | Select-Object -Unique

$done = @{}
if (Test-Path $Out) { Import-Csv $Out | ForEach-Object { $done[$_.id] = $_.result } }
else { "id,result,seconds,scene_offset,video,frames,state,draws,hud,detail" | Set-Content $Out -Encoding ascii }
$todo = @($list | Where-Object { -not $done.ContainsKey($_) })

$dumps = Join-Path $game "crash_dumps"
$log = Join-Path $game "logs\launcher.log"
Write-Host ("sweep: {0} ids, {1} recorded, {2} to go (~{3:N0} min) - screen {4}x{5}" -f
            $list.Count, ($list.Count - $todo.Count), $todo.Count, ($todo.Count * 34 / 60.0), $scrW, $scrH)

function Read-Since([string]$file, [long]$mark) {
    if (-not (Test-Path $file)) { return "" }
    if ((Get-Item $file).Length -le $mark) { return "" }
    $fs = [IO.File]::Open($file, "Open", "Read", "ReadWrite")
    $fs.Seek($mark, "Begin") | Out-Null
    $t = (New-Object IO.StreamReader($fs)).ReadToEnd()
    $fs.Close()
    return $t
}
function Stop-Everything {
    # mgs1 as well: s04a05l starts the bundled MGS1 as its own process, which nothing else here closes.
    Get-Process mgs4, mgs1 -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
}

$tally = @{ boot = 0; crash = 0; "no-scene" = 0 }
foreach ($r in $done.Values) { if ($tally.ContainsKey($r)) { $tally[$r]++ } }
$i = 0
foreach ($id in $todo) {
    $i++
    $before = @(Get-ChildItem "$dumps\*.dmp" -ErrorAction SilentlyContinue).Count
    $mark = if (Test-Path $log) { (Get-Item $log).Length } else { 0 }
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $video = Join-Path $VideoDir "$id.mkv"

    $proc = Start-Process -FilePath $exe -PassThru -ArgumentList @(
        $id, "--hold", ($RecordSeconds + 10), "--max-minutes", "3")

    # Recording starts with the launch, not at the first 3D frame: the offset to the scene is measured below and
    # the stills are cut relative to it, so nothing is lost if detection is late. Matroska because the file has
    # to survive the recorder being killed - an mp4 killed mid-write has no moov atom and will not open.
    $capArgs = @("-hide_banner", "-loglevel", "error", "-f", "gdigrab", "-framerate", "$Fps",
                 "-video_size", "${scrW}x${scrH}", "-offset_x", "0", "-offset_y", "0", "-i", "desktop",
                 "-t", "$($BootTimeout + $RecordSeconds + 5)",
                 "-vf", "scale=${Width}:-2", "-c:v", "libx264", "-preset", "veryfast", "-crf", "30",
                 "-pix_fmt", "yuv420p", "-flush_packets", "1", "-y", $video)
    # Started through ProcessStartInfo so stdin stays open: ffmpeg has to be stopped by sending it "q", not by
    # killing it. A killed ffmpeg loses whatever is still in the write buffer - the first attempt at this left
    # every recording exactly 262144 bytes long ("File ended prematurely"), which is the buffer, not the run.
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $ff
    $psi.Arguments = (($capArgs | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' ')
    $psi.UseShellExecute = $false
    $psi.RedirectStandardInput = $true
    $psi.CreateNoWindow = $true
    $rec = [Diagnostics.Process]::Start($psi)
    Start-Sleep -Milliseconds 700          # let gdigrab attach before anything else happens
    $recStart = [Diagnostics.Stopwatch]::StartNew()

    $sceneAt = $null; $sceneOffset = -1; $sawGame = $false
    while ($sw.Elapsed.TotalSeconds -lt $BootTimeout) {
        if (Read-Since $log $mark | Select-String -Quiet "scene running") {
            $sceneOffset = [Math]::Round($recStart.Elapsed.TotalSeconds, 2)
            $sceneAt = [Diagnostics.Stopwatch]::StartNew(); break
        }
        if (@(Get-ChildItem "$dumps\*.dmp" -ErrorAction SilentlyContinue).Count -gt $before) { break }
        $up = [bool](Get-Process mgs4 -ErrorAction SilentlyContinue)
        if ($up) { $sawGame = $true } elseif ($sawGame) { break }
        if ($proc.HasExited) { break }
        Start-Sleep -Milliseconds 250
    }

    if ($sceneAt) {
        while ($sceneAt.Elapsed.TotalSeconds -lt $RecordSeconds -and (Get-Process mgs4 -ErrorAction SilentlyContinue)) {
            Start-Sleep -Milliseconds 400
        }
    } elseif (Get-Process mgs4 -ErrorAction SilentlyContinue) {
        # No 3D frame, but something may be on screen: a pre-rendered video plays without the engine drawing a
        # scene, and so does the bundled MGS1 that s04a05l starts. Keep recording a little so it can be told from
        # a black screen, and take the stills from where the boot would have finished.
        $sceneOffset = 12
        Start-Sleep -Seconds 12
    }

    if (-not $rec.HasExited) {
        try { $rec.StandardInput.WriteLine("q"); $rec.StandardInput.Flush() } catch {}
        if (-not $rec.WaitForExit(8000)) { try { $rec.Kill() } catch {} }
    }
    if (-not $proc.HasExited) { try { $proc.Kill() } catch {} }
    Stop-Everything
    Start-Sleep -Seconds 2

    # What the engine itself thought it was drawing. The add-on's own classifier line carries the scene-draw
    # count, and that is what separates a pre-rendered video from a rendered scene: a video is a few blits (45 on
    # s02a25l_D3) where the engine draws hundreds (873 on s02a25l_D1 next door). The add-on rewrites this log on
    # every launch, so it is copied out per id while it still belongs to this run.
    $addonLog = Join-Path $game "logs\mgs4_dlss.log"
    $state = ""; $draws = -1; $hud = -1
    if (Test-Path $addonLog) {
        Copy-Item $addonLog (Join-Path $VideoDir "$id.addon.log") -Force -ErrorAction SilentlyContinue
        $line = Get-Content $addonLog -ErrorAction SilentlyContinue |
                Select-String "SCENE-STATE(-TICK)? (cutscene|gameplay|no-3d) \(frame \d+, scene draws (\d+), HUD draws (\d+)" |
                Select-Object -Last 1
        if ($line -and $line.Matches.Count) {
            $m = $line.Matches[0]
            $state = $m.Groups[2].Value; $draws = [int]$m.Groups[3].Value; $hud = [int]$m.Groups[4].Value
        }
    }

    $after = @(Get-ChildItem "$dumps\*.dmp" -ErrorAction SilentlyContinue).Count
    $tail = Read-Since $log $mark
    if ($after -gt $before)               { $result = "crash";    $detail = "new minidump" }
    elseif ($tail -match "scene running") { $result = "boot";     $detail = "3D frame reached" }
    else                                  { $result = "no-scene"; $detail = "no 3D frame in $([int]$sw.Elapsed.TotalSeconds)s" }

    # Cut the stills out of the recording, at each offset past the first 3D frame.
    $frames = 0
    if ((Test-Path $video) -and $sceneOffset -ge 0) {
        foreach ($o in $offsetList) {
            $at = $sceneOffset + $o
            $shot = Join-Path $ShotDir ("{0}_t{1:D2}.jpg" -f $id, $o)
            & $ff -hide_banner -loglevel error -ss $at -i $video -frames:v 1 -q:v 3 -y $shot 2>$null | Out-Null
            if (Test-Path $shot) { $frames++ }
        }
    }
    if ($NoVideo -and (Test-Path $video)) { Remove-Item $video -Force -ErrorAction SilentlyContinue }

    $tally[$result]++
    ('{0},{1},{2:N0},{3},{4},{5},{6},{7},{8},"{9}"' -f $id, $result, $sw.Elapsed.TotalSeconds, $sceneOffset,
        $(if ($NoVideo) { "" } else { "$id.mkv" }), $frames, $state, $draws, $hud, $detail) | Add-Content $Out -Encoding ascii
    Write-Host ("[{0,4}/{1}] {2,-18} {3,-9} {4,3}s  scene@{5,-6} f:{6} {7,-8} draws:{8,-5} boot {9} / crash {10} / no-scene {11}" -f
                $i, $todo.Count, $id, $result, [int]$sw.Elapsed.TotalSeconds, $sceneOffset, $frames, $state, $draws,
                $tally.boot, $tally.crash, $tally."no-scene")
}
Write-Host ""
Write-Host ("done: boot {0}, crash {1}, no-scene {2}  ->  {3}" -f $tally.boot, $tally.crash, $tally."no-scene", $Out)
Write-Host ("stills in {0}, recordings in {1}" -f $ShotDir, $VideoDir)
