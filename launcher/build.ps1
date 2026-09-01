# Builds mgs4-dlss-launcher.exe (the window) and mgs4-dlss-launcher-cli.exe (the command) from
# launcher\src, both wearing the game's own icon.
#
#   powershell -ExecutionPolicy Bypass -File launcher\build.ps1
#
# Nothing has to be installed: the C# compiler used here ships with Windows as part of the .NET Framework, and the
# WPF assemblies sit beside it. There is deliberately no MSBuild and no compiled XAML - Window.xaml is embedded as
# a resource and loaded with XamlReader at startup, which csc alone can do and which keeps the markup the same file
# the PowerShell app used. The icon is read out of the mgs4.exe already on this machine, which is why the built exe
# is not in the repo: that artwork is Konami's.
param(
    [string]$GameDir = "",      # default: MGS4_DIR / config.ini / the Steam libraries (tools\paths.ps1)
    [string]$Out = "",          # default: mgs4-dlss-launcher.exe in the repo root
    [switch]$NoIcon
)
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\..\tools\paths.ps1"
. "$PSScriptRoot\..\tools\game_icon.ps1"

$repo = Split-Path -Parent $PSScriptRoot
$src = Join-Path $PSScriptRoot "src"
if (-not $Out) { $Out = Join-Path $repo "mgs4-dlss-launcher.exe" }

$net = Join-Path $env:WINDIR "Microsoft.NET\Framework64\v4.0.30319"
if (-not (Test-Path (Join-Path $net "csc.exe"))) { $net = Join-Path $env:WINDIR "Microsoft.NET\Framework\v4.0.30319" }
$csc = Join-Path $net "csc.exe"
if (-not (Test-Path $csc)) {
    throw "no C# compiler found under $env:WINDIR\Microsoft.NET - install the .NET Framework 4 target."
}
$wpf = Join-Path $net "WPF"

# ---------------------------------------------------------------------------------------------- the icon

$icoArg = @()
$ico = Join-Path ([IO.Path]::GetTempPath()) "mgs4_dlss_launcher.ico"
if (-not $NoIcon) {
    if (-not $GameDir) { try { $GameDir = Get-Mgs4GameDir } catch { $GameDir = $null } }
    $gameExe = $(if ($GameDir) { Join-Mgs4Path $GameDir "mgs4.exe" } else { $null })
    if ($gameExe -and (Test-Mgs4Path $gameExe) -and (Write-Mgs4IconFile $gameExe $ico)) {
        $icoArg = @("/win32icon:$ico")
        Write-Host "icon taken from $gameExe"
    } else {
        Write-Host "no mgs4.exe found - building without an icon (pass -GameDir, or -NoIcon to stop asking)" -ForegroundColor Yellow
    }
}

# ---------------------------------------------------------------------------------------------- compile

$refs = @(
    "System.dll", "System.Core.dll", "System.XML.dll", "System.Drawing.dll", "System.Management.dll",
    "System.Web.Extensions.dll", "System.Xaml.dll", "System.IO.Compression.dll", "System.IO.Compression.FileSystem.dll"
) | ForEach-Object { "/reference:" + (Join-Path $net $_) }
$refs += @("PresentationFramework.dll", "PresentationCore.dll", "WindowsBase.dll") |
         ForEach-Object { "/reference:" + (Join-Path $wpf $_) }

$sources = @(Get-ChildItem -Path $src -Recurse -Filter *.cs | ForEach-Object { $_.FullName })
# No explicit resource name: csc names a resource after the file, which is exactly "Window.xaml". Passing the
# name after a comma made PowerShell hand csc a third comma-separated field it read as a visibility keyword.
$resources = @(
    ("/resource:" + (Join-Path $src "Window.xaml")),
    ("/resource:" + (Join-Path $src "SceneRow.xaml"))
)

# Two binaries from the one set of sources, the way python.exe and pythonw.exe are two:
#
#   mgs4-dlss-launcher.exe      /target:winexe - what a double-click runs. No console, ever, so the window opens
#                               with nothing flashing behind it.
#   mgs4-dlss-launcher-cli.exe  /target:exe - what mgs4-dlss-launcher.bat runs. A console program, so cmd waits
#                               for it and `--report > out.txt` catches what it writes.
#
# A windowed program cannot be both: cmd does not wait for one, and `start /b /wait` - which does wait - hands the
# child its own handles, so a redirect on the command line catches nothing. The console twin is the way to have a
# command that behaves like a command.
$cli = [IO.Path]::ChangeExtension($Out, $null).TrimEnd('.') + "-cli.exe"
foreach ($build in @(@{ Target = "winexe"; Path = $Out }, @{ Target = "exe"; Path = $cli })) {
    $cscArgs = @("/nologo", "/target:$($build.Target)", "/platform:anycpu", "/optimize+", "/warn:3",
                 "/out:$($build.Path)") + $refs + $resources + $icoArg + $sources
    & $csc @cscArgs
    if ($LASTEXITCODE -ne 0) { throw "csc failed ($LASTEXITCODE)" }
    $size = [math]::Round((Get-Item -LiteralPath $build.Path).Length / 1KB)
    Write-Host "built $($build.Path) ($size KB)"
}
Remove-Item -LiteralPath $ico -ErrorAction SilentlyContinue
