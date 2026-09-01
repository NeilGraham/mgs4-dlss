# Builds mgs4-dlss-launcher.exe from tools\app_launcher.cs, wearing the game's own icon.
#
#   powershell -ExecutionPolicy Bypass -File tools\build_app_exe.ps1
#
# Nothing has to be installed: the C# compiler used here ships with Windows as part of the .NET Framework, and the
# icon is read out of the mgs4.exe already on this machine. That is also why the built exe is not in the repo - the
# icon inside it is Konami's artwork, so it is made locally rather than redistributed.
param(
    [string]$GameDir = "",      # default: MGS4_DIR / config.ini / the Steam libraries (tools\paths.ps1)
    [string]$Out = "",          # default: mgs4-dlss-launcher.exe in the repo root
    [switch]$NoIcon             # build without one (the game folder is not needed then)
)
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\paths.ps1"
. "$PSScriptRoot\game_icon.ps1"

$repo = Split-Path -Parent $PSScriptRoot
if (-not $Out) { $Out = Join-Path $repo "mgs4-dlss-launcher.exe" }
$source = Join-Path $PSScriptRoot "app_launcher.cs"
if (-not (Test-Mgs4Path $source)) { throw "missing $source" }

$csc = Join-Path $env:WINDIR "Microsoft.NET\Framework64\v4.0.30319\csc.exe"
if (-not (Test-Mgs4Path $csc)) { $csc = Join-Path $env:WINDIR "Microsoft.NET\Framework\v4.0.30319\csc.exe" }
if (-not (Test-Mgs4Path $csc)) {
    throw "no C# compiler found under $env:WINDIR\Microsoft.NET - install the .NET Framework 4 target, or run with -NoIcon from a machine that has it."
}

# ---------------------------------------------------------------------------------------------- the icon

$icoArg = ""
$ico = Join-Path ([IO.Path]::GetTempPath()) "mgs4_dlss_app.ico"
if (-not $NoIcon) {
    if (-not $GameDir) { try { $GameDir = Get-Mgs4GameDir } catch { $GameDir = $null } }
    $gameExe = $(if ($GameDir) { Join-Mgs4Path $GameDir "mgs4.exe" } else { $null })
    if ($gameExe -and (Test-Mgs4Path $gameExe) -and (Write-Mgs4IconFile $gameExe $ico)) {
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
