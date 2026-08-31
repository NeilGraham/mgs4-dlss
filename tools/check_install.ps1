# The install check moved into the app: it is the Install tab of tools\mgs4_dlss.ps1, and the checks themselves are
# tools\install_checks.ps1. This is a shim, so the invocation the README and the releases document keeps working:
#
#   powershell -ExecutionPolicy Bypass -File tools\check_install.ps1 [-GameDir "..."] [-Report]
#
# New callers should use check-install.bat (the window, on the Install tab) or `mgs4-dlss.bat --report` (the text).
param(
    [string]$GameDir = "",      # default: MGS4_DIR / config.ini / the Steam libraries (tools\paths.ps1)
    [switch]$Report             # text to stdout instead of a window
)
$ErrorActionPreference = "Continue"

$fwd = @($(if ($Report) { "--report" } else { "--install" }))
if ($GameDir) { $fwd += @("--game-dir", $GameDir) }

& (Join-Path $PSScriptRoot "mgs4_dlss.ps1") @fwd
exit $LASTEXITCODE
