# Builds mgs4-dlss-launcher.exe from launcher\src: one file, with the add-on inside it.
#
#   powershell -ExecutionPolicy Bypass -File launcher\build.ps1              a local build, wearing the game's icon
#   powershell -ExecutionPolicy Bypass -File launcher\build.ps1 -Release     the release exe, wearing its own
#
# Nothing has to be installed: the C# compiler used here ships with Windows as part of the .NET Framework, and the
# WPF assemblies sit beside it. There is deliberately no MSBuild and no compiled XAML - Window.xaml is embedded as
# a resource and loaded with XamlReader at startup, which csc alone can do and which keeps the markup the same file
# the PowerShell app used.
#
# Everything the exe needs rides inside it as resources: the XAML, the tools\ data it reads (the scene table, the
# thumbnails, the install file list), and - when they have been built - mgs4_dlss.addon64 and mgs4_dlss.ini, so the
# one file a release carries can put the add-on next to mgs4.exe by itself. A checkout's copies on disk still win
# over the built-in ones when they are there (Paths.DataText, Install.FindBundled).
#
# The icon: a local build reads it out of the mgs4.exe already on this machine, which looks right beside the game
# but is Konami's artwork - so that exe stays out of the repo and out of the releases. -Release (or no mgs4.exe to
# read from) embeds the launcher's own icon, drawn by tools\launcher_icon.ps1, and that exe is what a release ships.
param(
    [string]$GameDir = "",      # default: MGS4_DIR / config.ini / the Steam libraries (tools\paths.ps1)
    [string]$Out = "",          # default: mgs4-dlss-launcher.exe in the repo root
    [switch]$NoIcon,            # do not look for mgs4.exe; use the launcher's own icon
    [switch]$Release            # own icon, and the add-on + ini must be there to embed
)
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\..\tools\paths.ps1"
. "$PSScriptRoot\..\tools\game_icon.ps1"
. "$PSScriptRoot\..\tools\launcher_icon.ps1"

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
$haveIcon = $false
if (-not $NoIcon -and -not $Release) {
    if (-not $GameDir) { try { $GameDir = Get-Mgs4GameDir } catch { $GameDir = $null } }
    $gameExe = $(if ($GameDir) { Join-Mgs4Path $GameDir "mgs4.exe" } else { $null })
    if ($gameExe -and (Test-Mgs4Path $gameExe) -and (Write-Mgs4IconFile $gameExe $ico)) {
        $haveIcon = $true
        Write-Host "icon taken from $gameExe"
    } else {
        Write-Host "no mgs4.exe found - using the launcher's own icon (pass -GameDir for the game's)" -ForegroundColor Yellow
    }
}
if (-not $haveIcon -and (Write-LauncherIconFile $ico)) {
    $haveIcon = $true
    Write-Host "icon: the launcher's own (tools\launcher_icon.ps1)"
}
if ($haveIcon) { $icoArg = @("/win32icon:$ico") }

# ---------------------------------------------------------------------------------------------- the add-on

# The two files the Setup tab's "Install the add-on" copies next to mgs4.exe. A checkout that has not run
# dlss-addon\build.bat has no addon64 yet; that build still works, and its Setup tab says the add-on is not here.
# A release build refuses instead: an exe that cannot install the add-on is not a release.
$bundled = @()
$addon = Join-Path $repo "build\mgs4_dlss.addon64"
$ini = Join-Path $repo "dlss-addon\mgs4_dlss.ini"
foreach ($f in @($addon, $ini)) {
    if (Test-Path -LiteralPath $f) {
        $bundled += "/resource:$f"
        Write-Host ("embedding " + (Split-Path -Leaf $f) + " (" + [math]::Round((Get-Item -LiteralPath $f).Length / 1KB) + " KB)")
    } elseif ($Release) {
        throw "a release build needs $f" + $(if ($f -eq $addon) { " - run dlss-addon\build.bat first" } else { "" })
    } else {
        Write-Host ("no " + (Split-Path -Leaf $f) + " to embed - this exe will not be able to install the add-on") -ForegroundColor Yellow
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
# The tools files go in too. Paths.DataText reads tools\<name> when it is there and this copy when it is not, so
# an exe carried off on its own still knows every scene and can still check an install, instead of coming up with
# two entries and falling over the moment Setup is opened.
$resources = @(
    ("/resource:" + (Join-Path $src "Window.xaml")),
    ("/resource:" + (Join-Path $src "SceneRow.xaml"))
)
$resources += @("install_manifest.json", "scenes.csv", "scene_info.json") |
              ForEach-Object { "/resource:" + (Join-Path $repo "tools\$_") }

# The banner behind the header: tools\art\banner.jpg, the title screen's Snake cut at 4K from the sweep's recording
# of it (tools\make_banner.py). Optional, like the thumbnails: without it the window wears Steam's key art.
$banner = Join-Path $repo "tools\art\banner.jpg"
if (Test-Path -LiteralPath $banner) {
    $resources += "/resource:$banner"
    Write-Host ("embedding banner.jpg (" + [math]::Round((Get-Item -LiteralPath $banner).Length / 1KB) + " KB)")
}

# One frame per scene, for the banner on each row and beside the description: tools\thumbs\<id>.jpg, kept as
# individual files in the repo so a re-pick changes one file, zipped here into the single resource Thumbs.cs
# reads. Optional: a checkout with no pictures just gets a launcher whose rows have no banner.
$thumbDir = Join-Path $repo "tools\thumbs"
if ((Test-Path -LiteralPath $thumbDir) -and (Get-ChildItem -LiteralPath $thumbDir -Filter *.jpg | Select-Object -First 1)) {
    $thumbs = Join-Path $repo "build\scene_thumbs.zip"
    New-Item -ItemType Directory -Force (Split-Path $thumbs) | Out-Null
    if (Test-Path -LiteralPath $thumbs) { Remove-Item -LiteralPath $thumbs -Force }
    # Stored, not deflated: the JPEGs are already compressed, and Thumbs.cs inflates the whole thing at startup.
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::CreateFromDirectory($thumbDir, $thumbs, [IO.Compression.CompressionLevel]::NoCompression, $false)
    $resources += "/resource:$thumbs"
    Write-Host ("embedding scene_thumbs.zip (" + (Get-ChildItem -LiteralPath $thumbDir -Filter *.jpg).Count + " frames, " +
                [math]::Round((Get-Item -LiteralPath $thumbs).Length / 1KB) + " KB)")
}

# One binary, windowed. It behaves like a command anyway: cmd waits for it and passes its handles through, so
# `mgs4-dlss-launcher --report > out.txt` catches what it writes. That only works because Program.KeepCallersOutput
# does not let AttachConsole throw the caller's redirect away - see the comment there. A console twin was built
# here for one commit before that was understood; it is not needed.
$cscArgs = @("/nologo", "/target:winexe", "/platform:anycpu", "/optimize+", "/warn:3", "/out:$Out") +
           $refs + $resources + $bundled + $icoArg + $sources
& $csc @cscArgs
if ($LASTEXITCODE -ne 0) { throw "csc failed ($LASTEXITCODE)" }
$size = [math]::Round((Get-Item -LiteralPath $Out).Length / 1KB)
Write-Host "built $Out ($size KB)"
Remove-Item -LiteralPath $ico -ErrorAction SilentlyContinue
