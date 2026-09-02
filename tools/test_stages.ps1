# Runs the add-on through a series of stages / cutscenes: for each stage it boots the game straight into it
# (mgs4-dlss-launcher, the same code the launcher window and the desktop shortcuts use), waits, takes screenshots (normal frame + optionally the motion-vector visualizer), collects a
# digest of the add-on log (DRS, object motion, evaluation rate, crashes) and moves on. Results land in
# <MGS4_OUT>\stage_tests\<timestamp>\ : <stage>_1.png, <stage>_mv.png, <stage>.log and summary.txt.
#
#   powershell -ExecutionPolicy Bypass -File tools\test_stages.ps1 -Stages "s00a00l,s02a50l_D1,s03a10l" -HoldSeconds 40 -MvVis
#
# Stage ids: see tools\stages.md. "_D<n>" entries are the cutscenes ("demos") of a stage, "_<n>" its gameplay sections.
param(
    [string]$Stages = "s00a00l,s02a50l_D1,s02a40l,s02a60l",
    [int]$HoldSeconds = 40,        # time in the stage after the first 3D frame before the screenshots
    [int]$Screens = 1,             # normal screenshots per stage (5 s apart)
    [switch]$MvVis,                # also capture the motion-vector visualizer (DebugMode 5)
    [switch]$ObjectMV,             # test with per-object motion vectors on (restored afterwards)
    [string]$GameDir = "",         # default: MGS4_DIR / config.ini / the Steam libraries (tools\paths.ps1)
    [string]$OutDir = "",          # default: MGS4_OUT\stage_tests (tools\paths.ps1)
    [switch]$KeepRunning           # leave the last stage running
)
$ErrorActionPreference = "Continue"
. "$PSScriptRoot\paths.ps1"
if (-not $GameDir) { $GameDir = Get-Mgs4GameDir }
if (-not $OutDir) { $OutDir = Join-Path (Get-Mgs4Paths).OutDir "stage_tests" }
$tools = Split-Path -Parent $MyInvocation.MyCommand.Path
$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$out = Join-Path $OutDir $stamp; New-Item -ItemType Directory -Force $out | Out-Null
$ini = Join-Path $GameDir "mgs4_dlss.ini"; $addonLog = Join-Path $GameDir "logs\mgs4_dlss.log"
function Log($m) { $line = "[{0:HH:mm:ss}] {1}" -f (Get-Date), $m; Write-Host $line; Add-Content (Join-Path $out "summary.txt") $line }
function SetKey($k, $v) { $c = Get-Content $ini; if ($c -match "^$k=") { $c = $c -replace "^$k=.*", "$k=$v" } else { $c += "$k=$v" }; Set-Content $ini $c -Encoding ASCII }
function GetKey($k) { $m = Get-Content $ini | Select-String "^$k=(.*)$"; if ($m) { $m.Matches[0].Groups[1].Value } else { "" } }
Add-Type -Name Snap -Namespace StageTest -MemberDefinition '[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);'
function Snapshot($p, $dest) {
    $before = Get-ChildItem "$GameDir\*.png" | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    [StageTest.Snap]::PostMessage($p.MainWindowHandle, 0x100, [IntPtr]0x2C, [IntPtr]0) | Out-Null; Start-Sleep -Milliseconds 150
    [StageTest.Snap]::PostMessage($p.MainWindowHandle, 0x101, [IntPtr]0x2C, [IntPtr]0) | Out-Null; Start-Sleep 3
    $after = Get-ChildItem "$GameDir\*.png" | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($after -and (-not $before -or $after.FullName -ne $before.FullName)) { Move-Item $after.FullName $dest -Force; return $true }
    return $false
}
$prevObj = GetKey "ObjectMV"; $prevDbg = GetKey "DebugMode"
if ($ObjectMV) { SetKey "ObjectMV" 1 } else { SetKey "ObjectMV" 0 }
SetKey "DebugMode" 0
Log "stage test ${stamp}: stages [$Stages], hold $HoldSeconds s, ObjectMV=$([int][bool]$ObjectMV), MvVis=$([bool]$MvVis)"
$list = $Stages.Split(',') | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne "" }
foreach ($stage in $list) {
    Log "=== ${stage}"
    $logStart = if (Test-Path $addonLog) { (Get-Content $addonLog).Count } else { 0 }
    $t0 = Get-Date
    & (Join-Path (Split-Path -Parent $tools) "mgs4-dlss-launcher.bat") $stage --start-timeout 60 --game-dir $GameDir 2>&1 | Select-Object -Last 1 | ForEach-Object { Log "  launcher: $_" }
    $p = Get-Process mgs4 -ErrorAction SilentlyContinue
    if (-not $p) { Log "  game did not start"; continue }
    Start-Sleep $HoldSeconds
    $sceneOk = (Get-Content $addonLog | Select-Object -Skip $logStart | Select-String -SimpleMatch "NGX EvaluateFeature ok" | Measure-Object).Count -gt 0
    Log ("  3D scene: " + $(if ($sceneOk) { "rendering (DLSS evaluating)" } else { "NOT rendering after the hold (menu / black screen / stage id not bootable?)" }))
    if (-not (Get-Process mgs4 -ErrorAction SilentlyContinue)) { Log "  CRASHED during hold"; }
    else {
        $p = Get-Process mgs4
        for ($i = 1; $i -le $Screens; $i++) { if (Snapshot $p (Join-Path $out "${stage}_$i.png")) { Log "  screenshot $i" } else { Log "  screenshot $i failed" }; if ($i -lt $Screens) { Start-Sleep 5 } }
        if ($MvVis) { SetKey "DebugMode" 5; Start-Sleep 5; if (Snapshot $p (Join-Path $out "${stage}_mv.png")) { Log "  MV visualizer screenshot" }; SetKey "DebugMode" 0; Start-Sleep 2 }
    }
    $all = @(Get-Content $addonLog); if ($all.Count -lt $logStart) { $logStart = 0 }   # log was rotated
    $lines = @($all | Select-Object -Skip $logStart)
    $digest = @($lines | Where-Object { $_ -match "config: Enabled|DRS:|scene viewport|object motion|NGX CreateFeature DLSS \(|NGX EvaluateFeature ok|injecting|insertion:|FG: (options|UI layer)|crash|\[SL\].*error|failed" })
    Set-Content (Join-Path $out "$stage.log") ($(if ($digest.Count) { $digest } else { @("(no add-on log lines for this stage - " + $lines.Count + " lines total in the slice)") }))
    $evals = @($lines | Select-String "NGX EvaluateFeature ok \(#(\d+)\)" | ForEach-Object { [int]$_.Matches[0].Groups[1].Value })
    $drs = @($lines | Select-String -SimpleMatch "DRS: scene viewport").Count
    $vp = ($lines | Select-String "scene viewport \(last dynamic draw\):\s*\S*\s*\(([^)]*)\)" | Select-Object -Last 1)
    $crash = Get-ChildItem "$GameDir\crash_dumps\*.log" -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -gt $t0 }
    Log ("  evaluations " + $(if ($evals.Count) { $evals[-1] } else { 0 }) + ", DRS sub-rect frames logged " + $drs + ", last scene viewport " + $(if ($vp) { $vp.Matches[0].Groups[1].Value } else { "?" }) + $(if ($crash) { ", CRASH " + $crash[0].Name } else { "" }))
    if (-not $KeepRunning -or $stage -ne $list[-1]) { Stop-Process -Name mgs4 -Force -ErrorAction SilentlyContinue; Start-Sleep 4 }
}
SetKey "ObjectMV" $(if ($prevObj -ne "") { $prevObj } else { 0 }); SetKey "DebugMode" $(if ($prevDbg -ne "") { $prevDbg } else { 0 })
Log "done -> $out"
