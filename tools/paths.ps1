# The PowerShell half of tools\paths.py: the same values, resolved the same way (env var > config.ini in the repo
# root > auto-detection), so the scripts do not care where this checkout or the game install sits.
#
#   . "$PSScriptRoot\paths.ps1"; $p = Get-Mgs4Paths       - GameDir / OutDir / Gold / FFmpeg / FFprobe / Python
#   powershell -File tools\paths.ps1                      - print the resolved paths
#
# Scripts take -GameDir / -OutDir parameters that default to these, so a one-off run can still override them.

$Mgs4Repo = Split-Path -Parent $PSScriptRoot
$Mgs4Config = Join-Path $Mgs4Repo "config.ini"
$Mgs4AppId = "2492670"          # METAL GEAR SOLID 4: Guns of the Patriots - Master Collection Version

function Get-Mgs4ConfigValues {
    $vals = @{}
    if (Test-Path $Mgs4Config) {
        foreach ($line in Get-Content $Mgs4Config) {
            $t = $line.Trim()
            if ($t -eq "" -or $t.StartsWith(";") -or $t.StartsWith("#") -or $t.StartsWith("[") -or -not $t.Contains("=")) { continue }
            $k, $v = $t.Split("=", 2)
            $v = $v.Trim().Trim('"')
            if ($v -ne "") { $vals[$k.Trim().ToUpper()] = $v }
        }
    }
    return $vals
}

function Get-Mgs4Setting($key, $values, $default) {
    $v = [Environment]::GetEnvironmentVariable($key)
    if (-not $v) { $v = $values[$key.ToUpper()] }
    if (-not $v) { $v = $default }
    if ($v) { return [Environment]::ExpandEnvironmentVariables($v) }
    return $null
}

function Get-Mgs4SteamLibraries {
    $roots = @()
    foreach ($reg in @("HKCU:\SOFTWARE\Valve\Steam", "HKLM:\SOFTWARE\WOW6432Node\Valve\Steam")) {
        try {
            $k = Get-ItemProperty -Path $reg -ErrorAction Stop
            foreach ($name in @("SteamPath", "InstallPath")) { if ($k.$name) { $roots += $k.$name } }
        } catch {}
    }
    $roots += @("C:\Program Files (x86)\Steam", "C:\Program Files\Steam")

    # Steam's own registry entry and libraryfolders.vdf cover the normal case, but a library on a drive Steam has
    # forgotten (or a copied install) is still worth finding, so every ready drive is tried in letter order for the
    # handful of places a library actually sits.
    foreach ($drive in ([IO.DriveInfo]::GetDrives() | Where-Object { $_.IsReady } | Sort-Object Name)) {
        $letter = $drive.Name.TrimEnd('\')
        foreach ($sub in @("", "\SteamLibrary", "\Steam", "\Games\Steam", "\Program Files (x86)\Steam", "\Program Files\Steam")) {
            $roots += ($letter + $sub)
        }
    }

    $libs = @()
    foreach ($root in $roots) {
        $root = $root -replace '/', '\'
        if ((Test-Mgs4Path $root) -and ($libs -notcontains $root)) { $libs += $root }
        $vdf = Join-Mgs4Path $root "steamapps\libraryfolders.vdf"
        if (-not (Test-Mgs4Path $vdf)) { continue }
        foreach ($line in Get-Content $vdf) {
            if ($line -match '^\s*"path"\s+"(.+)"\s*$') {
                $lib = $Matches[1] -replace '\\\\', '\'
                if ((Test-Mgs4Path $lib) -and ($libs -notcontains $lib)) { $libs += $lib }
            }
        }
    }
    return $libs
}

function Find-Mgs4GameDir {
    foreach ($lib in Get-Mgs4SteamLibraries) {
        $apps = Join-Mgs4Path $lib "steamapps"
        $names = @("METAL GEAR SOLID 4")
        $manifest = Join-Mgs4Path $apps "appmanifest_$Mgs4AppId.acf"
        if (Test-Mgs4Path $manifest) {
            $m = Select-String -Path $manifest -Pattern '^\s*"installdir"\s+"(.+)"' | Select-Object -First 1
            if ($m) { $names = @($m.Matches[0].Groups[1].Value) + $names }
        }
        foreach ($name in $names) {
            $cand = Join-Mgs4Path $apps "common\$name\MGS4"
            if (Test-Mgs4Path (Join-Mgs4Path $cand "mgs4.exe")) { return $cand }
        }
    }
    return $null
}

# Exists? Without Test-Path's error on a drive letter that is not mounted.
function Test-Mgs4Path($path) {
    if (-not $path) { return $false }
    return ([IO.File]::Exists($path) -or [IO.Directory]::Exists($path))
}

# Join without touching the filesystem, so a path on a drive that is not there does not raise.
function Join-Mgs4Path($a, $b) {
    if (-not $a) { return $null }
    return [IO.Path]::Combine($a, $b)
}

# Absolute, with an upper-case drive letter (the registry hands Steam's path back in lower case).
function Format-Mgs4Path($path) {
    if (-not $path) { return $null }
    $path = $path -replace '/', '\'
    if ($path -match '^[a-z]:') { $path = $path.Substring(0, 1).ToUpper() + $path.Substring(1) }
    return $path.TrimEnd('\')
}

function Resolve-Mgs4Exe($configured, $name) {
    if ($configured) { return $configured }
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return $name
}

# A folder someone points at is the game folder if mgs4.exe is in it - or if mgs4.exe is in an MGS4 folder inside
# it, because "METAL GEAR SOLID 4" is the install root and MGS4 is the part that matters. $null when it is neither.
function Resolve-Mgs4GameDir($path) {
    if (-not $path) { return $null }
    $path = Format-Mgs4Path $path
    if (Test-Mgs4Path (Join-Mgs4Path $path "mgs4.exe")) { return $path }
    $inner = Join-Mgs4Path $path "MGS4"
    if (Test-Mgs4Path (Join-Mgs4Path $inner "mgs4.exe")) { return $inner }
    return $null
}

function Get-Mgs4Paths {
    $v = Get-Mgs4ConfigValues
    $game = Get-Mgs4Setting "MGS4_DIR" $v $null
    if (-not $game) { $game = Find-Mgs4GameDir }
    $resolved = Resolve-Mgs4GameDir $game
    if ($resolved) { $game = $resolved } else { $game = Format-Mgs4Path $game }
    $out = Format-Mgs4Path (Get-Mgs4Setting "MGS4_OUT" $v (Join-Path $Mgs4Repo "work"))
    return [ordered]@{
        Repo    = $Mgs4Repo
        GameDir = $game
        OutDir  = $out
        Gold    = (Join-Mgs4Path $out "gold")
        FFmpeg  = (Resolve-Mgs4Exe (Get-Mgs4Setting "MGS4_FFMPEG" $v $null) "ffmpeg")
        FFprobe = (Resolve-Mgs4Exe (Get-Mgs4Setting "MGS4_FFPROBE" $v $null) "ffprobe")
        Python  = (Resolve-Mgs4Exe (Get-Mgs4Setting "MGS4_PYTHON" $v $null) "python")
    }
}

# Convenience for scripts that only want the game folder as a parameter default.
function Get-Mgs4GameDir {
    $g = (Get-Mgs4Paths).GameDir
    if ($g -and -not (Test-Mgs4Path (Join-Mgs4Path $g "mgs4.exe"))) { $g = $null }
    if (-not $g) {
        throw "mgs4.exe not found in any Steam library. Set MGS4_DIR in $Mgs4Config (copy config.example.ini), in the environment, or pass -GameDir."
    }
    return $g
}

if ($MyInvocation.InvocationName -ne '.') {
    $p = Get-Mgs4Paths
    Write-Host ("config     {0}{1}" -f $Mgs4Config, $(if (Test-Mgs4Path $Mgs4Config) { "" } else { "  (absent - defaults / detection)" }))
    foreach ($k in $p.Keys) {
        $val = $p[$k]
        $miss = ""
        if (-not (Test-Mgs4Path $val)) { $miss = "   <- missing" }
        Write-Host ("{0,-10} {1,-70}{2}" -f $k, $val, $miss)
    }
}
