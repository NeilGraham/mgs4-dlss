# Boots MGS4 straight into a stage and presses keys to set the scene up.
#
# This is now a thin shim over tools\launcher.ps1, which does the same job with more of it (a scene list, the Cross
# tapping that fires the flashback prompts, ending a scene when gameplay starts, and a window). The parameters below
# are kept because desktop shortcuts and older notes use them; new callers should use the launcher directly:
#
#   launcher.bat s00a00l                                    what -Stage s00a00l does
#   launcher.bat s00a00l --no-advance                       ... -PressFor 0
#   launcher.bat s00a00l --keys "5,ENTER,4,ENTER"           ... -Keys "5,ENTER,4,ENTER"
#   launcher.bat --help                                     everything else
#
# Log: <GameDir>\logs\launcher.log
param(
    [string]$Stage = "s00a00l",
    [int]$WaitSeconds = 0,          # 0 = just wait for the game window to appear (+ SettleSeconds)
    [double]$SettleSeconds = 2,
    [string]$Keys = "",             # explicit sequence, e.g. "5,ENTER,4,ENTER"; empty = repeated presses below
    [string]$PressKey = "ENTER",
    [double]$PressEvery = 0.5,
    [double]$PressFor = 60,
    [switch]$NoSceneDetect,         # keep pressing for the full PressFor instead of stopping at the first 3D frame
    [string]$GameDir = "",          # default: MGS4_DIR / config.ini / the Steam libraries (tools\paths.ps1)
    [switch]$NoRestart
)
$ErrorActionPreference = "Continue"

$fwd = @($Stage)
if ($Keys) {
    $fwd += @("--keys", $Keys)
} elseif ($PressFor -le 0) {
    $fwd += "--no-advance"
} else {
    $fwd += @("--start-timeout", "$PressFor", "--press-every", "$PressEvery", "--press-key", $PressKey)
    if ($NoSceneDetect) { $fwd += "--no-scene-detect" }
}
$settle = $(if ($WaitSeconds -gt 0) { $WaitSeconds } else { $SettleSeconds })
$fwd += @("--settle", "$settle")
if ($NoRestart) { $fwd += "--no-restart" }
if ($GameDir) { $fwd += @("--game-dir", $GameDir) }

& (Join-Path $PSScriptRoot "launcher.ps1") @fwd
exit $LASTEXITCODE
