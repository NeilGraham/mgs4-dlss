# The install check for the MGS4 DLSS add-on, as a library: which required and optional files are in place, what the
# settings say, and what the add-on reported on its last run. No UI and no dispatch of its own - tools\mgs4_dlss.ps1
# renders it, as the Install tab and as `mgs4-dlss.bat --report`.
#
# The file list, the versions this was verified against and the download links live in tools\install_manifest.json;
# this only reads them.
if (-not (Get-Command Get-Mgs4Paths -ErrorAction SilentlyContinue)) { . "$PSScriptRoot\paths.ps1" }

$script:ManifestPath = Join-Path $PSScriptRoot "install_manifest.json"


# ---------------------------------------------------------------------------------------------- checks

function Get-PeVersion($path) {
    if (-not (Test-Mgs4Path $path)) { return $null }
    try {
        $vi = (Get-Item -LiteralPath $path).VersionInfo
        $s = $vi.ProductVersion
        if (-not $s) { $s = $vi.FileVersion }
        if ($s) { return ($s -replace ',', '.').Trim() }
    } catch {}
    return $null
}

# "310,8,0,0" and "310.8.0" both normalise to 310.8.0, so the manifest can be written the readable way.
function Get-NormalVersion($s) {
    if (-not $s) { return "" }
    $parts = @(($s -replace ',', '.') -split '\.' | Where-Object { $_ -match '^\d+$' })
    if ($parts.Count -eq 0) { return "" }
    while ($parts.Count -lt 3) { $parts += "0" }
    return ($parts[0..2] -join ".")
}

# 310.8.0.0 -> 310.8.0, but 1.0.0.1 and 0.2026.0827.2036 keep every group that says something.
function Format-Version($s) {
    $p = @($s -split '\.')
    while ($p.Count -gt 3 -and $p[$p.Count - 1] -eq '0') { $p = $p[0..($p.Count - 2)] }
    return ($p -join '.')
}

function New-Row($status, $name, $detail, $value, $url) {
    return [pscustomobject]@{ Status = $status; Name = $name; Detail = $detail; Value = $value; Url = $url }
}

function Get-IniValue($path, $key) {
    if (-not (Test-Mgs4Path $path)) { return $null }
    $m = Select-String -LiteralPath $path -Pattern ("^\s*" + [regex]::Escape($key) + "\s*=\s*(.*?)\s*$") | Select-Object -First 1
    if ($m) { return $m.Matches[0].Groups[1].Value }
    return $null
}

# In place, keeping the order and the comments. The add-on owns this file while the game runs - its writes go through
# the Windows profile API, whose cache will happily undo an outside edit - so every caller checks that first.
function Set-Ini([string]$path, [hashtable]$values) {
    if (-not (Test-Mgs4Path $path)) { throw "no mgs4_dlss.ini at $path" }
    $lines = @(Get-Content -LiteralPath $path -Encoding ASCII)
    $left = @{}
    foreach ($k in $values.Keys) { $left[$k] = $values[$k] }
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match '^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=') {
            $k = $Matches[1]
            if ($left.ContainsKey($k)) {
                $comment = ""
                if ($lines[$i] -match '(\s+;.*)$') { $comment = $Matches[1] }
                $lines[$i] = "$k=$($left[$k])$comment"
                $left.Remove($k)
            }
        }
    }
    foreach ($k in @($left.Keys)) { $lines = @($lines) + "$k=$($left[$k])" }
    Set-Content -LiteralPath $path -Value $lines -Encoding ASCII
}

# What the game's own options have to say for the add-on to work. The check reads them and Set-GameSettings writes
# them, from this one list.
$script:WantedGameSettings = [ordered]@{ api = "dx12"; vsync = "false"; fpsLimiter = "60"; enableFXAA = "false" }

# Writes those four into mgs4.savedsettings, leaving everything else in the file alone. The game owns this file
# while it runs, so the caller checks that first.
function Set-GameSettings([string]$savedSettings) {
    if (-not (Test-Mgs4Path $savedSettings)) { throw "no mgs4.savedsettings to write to" }
    Set-Ini $savedSettings ([hashtable]$script:WantedGameSettings)
    return $savedSettings
}

function Get-SavedSettingsPath($game) {
    $root = Split-Path -Parent $game           # ...\METAL GEAR SOLID 4
    $saves = Join-Mgs4Path $root "mgs4_savedata_win"
    if (-not (Test-Mgs4Path $saves)) { return $null }
    $hit = Get-ChildItem -Path $saves -Filter "mgs4.savedsettings" -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($hit) { return $hit.FullName }
    return $null
}

# What the add-on reported the last time the game ran. File presence cannot see any of this.
function Get-LastRun($game) {
    $r = [ordered]@{}
    $log = Join-Mgs4Path $game "logs\mgs4_dlss.log"
    if (-not (Test-Mgs4Path $log)) { return $r }
    $r.LogPath = $log
    $r.LogAge = (Get-Date) - (Get-Item -LiteralPath $log).LastWriteTime
    $text = Get-Content -LiteralPath $log -ErrorAction SilentlyContinue
    if (-not $text) { return $r }
    foreach ($line in $text) {
        if ($line -match 'mgs4_dlss v(\S+) registered')                                    { $r.Addon = $Matches[1] }
        if ($line -match 'NGX D3D12 Init_with_ProjectID.*-> (\S+)')                        { $r.Ngx = $Matches[1] }
        if ($line -match 'nvngx_dlss\.dll (loaded|not loaded), _nvngx (\S+), DLSS5 add-on (loaded|absent)') {
            $r.DlssDll = $Matches[1]; $r.NvngxProxy = $Matches[2]; $r.Nr = $Matches[3]
        }
        if ($line -match 'insertion: (.+?) \(PrePost=')                                    { $r.Insertion = $Matches[1] }
        if ($line -match 'frame generation: (.+?), target fps (\d+), Reflex (\d)')         { $r.Fg = $Matches[1]; $r.FgTarget = $Matches[2]; $r.Reflex = $Matches[3] }
        if ($line -match 'frame generation off at startup: Streamline not loaded')         { $r.FgNoStreamline = $true }
        if ($line -match 'Streamline initialised \(SL ([\d.]+) / DLSS-G ([\d.]+)\); DLSS-G supported: (\w+)') {
            $r.Sl = $Matches[1]; $r.DlssG = $Matches[2]; $r.FgSupported = $Matches[3]
        }
        if ($line -match 'driver ([\d.]+) detected / ([\d.]+) required')                   { $r.Driver = $Matches[1]; $r.DriverMin = $Matches[2] }
        if ($line -match 'NGX EvaluateFeature ok \(#(\d+)\)')                              { $r.Evaluations = $Matches[1] }
        if ($line -match 'NGX CreateFeature DLSS \((.+?)\)')                               { $r.Feature = $Matches[1] }
    }
    $rl = Join-Mgs4Path $game "ReShade.log"
    if (Test-Mgs4Path $rl) {
        $rtext = Get-Content -LiteralPath $rl -ErrorAction SilentlyContinue
        $r.ReShadeAddonSupport = [bool](@($rtext | Where-Object { $_ -match 'Searching for add-ons' }).Count)
        $reg = @($rtext | Where-Object { $_ -match 'Registered add-on "(.+?)" v(\S+)' })
        $names = @()
        foreach ($line in $reg) { if ($line -match 'Registered add-on "(.+?)" v(\S+)') { $names += $Matches[1] } }
        $r.ReShadeAddons = $names
        # Whether Neural Rendering actually ran. RenoDX does NR through its own NGX hook (feature 18), not through
        # Streamline, so this - not anything sl.dlss_nr says - is the signal.
        foreach ($line in $rtext) {
            if ($line -match 'signed DLSSNR ([\d.]+) D3D12 runtime initialized') { $r.NrRuntime = $Matches[1] }
            if ($line -match 'feature 18 created .* for NR input (\S+) -> output (\S+)') { $r.NrFeature = $Matches[1] }
            if ($line -match 'NGX feature create intercepted: feature=18') { $r.NrCreated = $true }
        }
    }
    return $r
}

# tools\ViGEmClient.dll plus the ViGEmBus driver: what lets the launcher tap Cross through the whole of a cutscene,
# which is what MGS4's flashback prompts want. Optional - without it the launcher can only press Enter.
function Get-VigemState {
    $dll = $null
    foreach ($c in @($env:VIGEM_CLIENT_DLL, (Join-Path $PSScriptRoot "ViGEmClient.dll"))) {
        if ($c -and (Test-Mgs4Path $c)) { $dll = $c; break }
    }
    $driver = $false
    try { $driver = [bool](Get-CimInstance Win32_SystemDriver -Filter "Name='ViGEmBus'" -ErrorAction Stop | Where-Object { $_.State -eq "Running" }) } catch {}
    if ($dll -and $driver) {
        return @{ Status = "ok"; Value = "ready"; Detail = "ViGEmBus is running and $([IO.Path]::GetFileName($dll)) is in place - the launcher can tap Cross for the flashback prompts" }
    }
    if (-not $dll -and -not $driver) {
        return @{ Status = "info"; Value = "not installed"; Detail = "optional: ViGEmBus + tools\ViGEmClient.dll let the launcher tap Cross through a cutscene; without them it can only press Enter" }
    }
    if (-not $dll) {
        return @{ Status = "warn"; Value = "no ViGEmClient.dll"; Detail = "the ViGEmBus driver is running, but tools\ViGEmClient.dll is missing (it also ships inside the vgamepad package, or set VIGEM_CLIENT_DLL)" }
    }
    return @{ Status = "warn"; Value = "driver not running"; Detail = "tools\ViGEmClient.dll is there but the ViGEmBus driver is not running" }
}

function Get-RefreshRate {
    try {
        $c = Get-CimInstance -ClassName Win32_VideoController -ErrorAction Stop |
             Where-Object { $_.CurrentRefreshRate } | Select-Object -First 1
        if ($c) { return [int]$c.CurrentRefreshRate }
    } catch {}
    return 0
}

function Invoke-InstallChecks {
    param([string]$Game)

    $sections = @()
    $manifest = Get-Content -LiteralPath $script:ManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $run = Get-LastRun $Game

    foreach ($sec in $manifest.sections) {
        $rows = @()
        foreach ($f in $sec.files) {
            $full = Join-Mgs4Path $Game $f.path
            $present = Test-Mgs4Path $full
            $detail = $f.detail
            $value = "not found"
            if ($present) {
                $ver = Get-PeVersion $full
                $status = "ok"
                if ($ver) {
                    $value = "v" + (Format-Version $ver)
                    $want = Get-NormalVersion $f.verified
                    $have = Get-NormalVersion $ver
                    if ($want -and $have -and $want -ne $have) { $value = "v" + (Format-Version $ver) + " (verified with $($f.verified))" }
                } else {
                    $bytes = (Get-Item -LiteralPath $full).Length
                    if ($bytes -ge 1024) { $value = "{0:n0} KB" -f [math]::Round($bytes / 1KB) } else { $value = "$bytes bytes" }
                }
            } else {
                # A group says what its files are worth; a file can override that either way.
                $mustHave = $f.required -or ($sec.required -and -not $f.optional)
                if ($mustHave) { $status = "bad" } else { $status = "warn" }
                $value = "not found"
                # DLSS itself can come from the driver instead of a file in the game folder.
                if ($f.optionalIfDriverOverride -and $run.NvngxProxy -and $run.NvngxProxy -ne "0000000000000000") {
                    $status = "ok"
                    $value = "supplied by the driver"
                    $detail = "no local $($f.path); the NVIDIA app's DLSS override provided it on the last run"
                }
            }
            # ReShade without add-on support loads, finds nothing, and looks fine on disk.
            if ($present -and $f.path -eq "dxgi.dll" -and $run.Contains("ReShadeAddonSupport") -and -not $run.ReShadeAddonSupport) {
                $status = "warn"
                $detail = "ReShade loaded but never searched for add-ons - this looks like the build WITHOUT add-on support"
            }
            $ownUrl = $null
            if ($f.url -and $f.url -ne $sec.url) { $ownUrl = $f.url }
            $rows += New-Row $status $f.path $detail $value $ownUrl
        }
        $sections += [pscustomobject]@{
            Id = $sec.id; Title = $sec.title; Blurb = $sec.blurb; Rows = $rows
            Required = [bool]$sec.required; Url = $sec.url; UrlLabel = $sec.urlLabel; Guide = $sec.guide
            Accepts = @($sec.accepts); Drop = @($sec.drop)
        }
    }

    # ------------------------------------------------------------------ settings
    $rows = @()
    $ss = Get-SavedSettingsPath $Game
    $wrong = @()
    if ($ss) {
        $want = $script:WantedGameSettings
        $why = @{ api = "DLSS needs the D3D12 backend"; vsync = "off, the frame limiter paces instead";
                  fpsLimiter = "the port is built around 60"; enableFXAA = "DLAA replaces it; both together smears" }
        foreach ($k in $want.Keys) {
            if ((Get-IniValue $ss $k) -ne $want[$k]) { $wrong += $k }
            $have = Get-IniValue $ss $k
            if ($null -eq $have) {
                $rows += New-Row "warn" "Game: $k" "not written yet - set it once in the in-game options" "unset" $null
            } elseif ($have -eq $want[$k]) {
                $rows += New-Row "ok" "Game: $k" $why[$k] $have $null
            } else {
                $rows += New-Row "warn" "Game: $k" "$($why[$k]) - expected $($want[$k])" $have $null
            }
        }
    } else {
        $rows += New-Row "info" "Game settings" "mgs4.savedsettings not found; run the game once" "unknown" $null
    }

    # The add-on's own keys are the Settings tab's job; only the ones that are actually wrong belong here, so this
    # does not become a second, read-only copy of that tab.
    $ini = Join-Mgs4Path $Game "mgs4_dlss.ini"
    if (Test-Mgs4Path $ini) {
        $en = Get-IniValue $ini "Enabled"
        if ($en -ne "1") {
            $rows += New-Row "bad" "Add-on: Enabled" "the add-on loads but does nothing while this is 0 - Settings tab" "$en" $null
        }

        # FrameGen against the hardware, which is the one thing the Settings tab cannot tell you.
        $fgm = Get-IniValue $ini "FrameGen"; $fgt = Get-IniValue $ini "FGTargetFps"; $hz = Get-RefreshRate
        if ($fgm -and $fgm -ne "0" -and $hz -gt 0) {
            if ($hz -le 61) {
                $rows += New-Row "warn" "Add-on: FrameGen" "the display reports $hz Hz - generated frames cannot be shown at 60 Hz, set FrameGen=0" "$fgm, target $fgt" $null
            } elseif ($fgt -and [int]$fgt -gt $hz) {
                $rows += New-Row "warn" "Add-on: FrameGen" "target $fgt fps is above the display's $hz Hz" "$fgm, target $fgt" $null
            }
        }

        $diags = @()
        foreach ($diag in @("DebugMode", "Probe", "TraceFrames", "TraceFreeze", "DumpShaders")) {
            $v = Get-IniValue $ini $diag
            if ($v -and $v -ne "0") { $diags += "$diag=$v" }
        }
        if ($diags.Count) {
            $rows += New-Row "warn" "Add-on: diagnostics on" "these cost frames; set them to 0 for normal play - Settings tab" ($diags -join ", ") $null
        }
    }

    $rini = Join-Mgs4Path $Game "ReShade.ini"
    if (Test-Mgs4Path $rini) {
        $dis = Get-IniValue $rini "DisabledAddons"
        if ($dis -and $dis -match "mgs4_dlss") {
            $rows += New-Row "bad" "ReShade: add-on disabled" "mgs4_dlss is in ReShade's DisabledAddons list" $dis $null
        }
        $up = Get-IniValue $rini "NREnableUpscaling"
        if ($null -ne $up -and $up -ne "0") {
            $rows += New-Row "warn" "RenoDX: NREnableUpscaling" "on, on top of this add-on's own DLAA - two upscalers in a row" $up $null
        }
    }

    $asi = Join-Mgs4Path $Game "scripts\MGS4_D3D12.ini"
    if (Test-Mgs4Path $asi) {
        $e = Get-IniValue $asi "Enabled"
        $api = $null
        if ($ss) { $api = Get-IniValue $ss "api" }
        if ($e -eq "1" -and $api -eq "dx12") {
            $rows += New-Row "warn" "D3D12 switch" "the ASI forces D3D12 while the game is already set to it - leave Enabled = 0" "Enabled = 1" $null
        }
    }
    # Not part of the add-on, but the Play tab's "Keep pressing X" needs both halves of it.
    $vigem = Get-VigemState
    $rows += New-Row $vigem.Status "Virtual controller" $vigem.Detail $vigem.Value "https://github.com/nefarius/ViGEmBus/releases"

    $sections += [pscustomobject]@{ Id = "settings"; Title = "Settings"
        Blurb = "The ones the Settings tab does not cover - the game's own options, ReShade's, and anything that reads as wrong."
        Rows = $rows; SavedSettings = $ss; WrongKeys = @($wrong) }

    # ------------------------------------------------------------------ last run
    $rows = @()
    if (-not $run.LogPath) {
        $rows += New-Row "info" "No log yet" "logs\mgs4_dlss.log appears the first time the game runs with the add-on" "-" $null
    } else {
        $age = $run.LogAge
        $when = "{0:0} min ago" -f $age.TotalMinutes
        if ($age.TotalHours -ge 24) { $when = "{0:0} days ago" -f $age.TotalDays }
        elseif ($age.TotalHours -ge 1) { $when = "{0:0} h ago" -f $age.TotalHours }
        $rows += New-Row "info" "Last run" "logs\mgs4_dlss.log" $when $null

        if ($run.Addon) { $rows += New-Row "ok" "Add-on registered" "the ReShade add-on loaded and registered" "v$($run.Addon)" $null }
        if ($run.Contains("ReShadeAddonSupport")) {
            if ($run.ReShadeAddonSupport) {
                $rows += New-Row "ok" "ReShade add-on support" ("registered: " + ($run.ReShadeAddons -join ", ")) "yes" $null
            } else {
                $rows += New-Row "bad" "ReShade add-on support" "ReShade never searched for add-ons - install the add-on build" "no" $null
            }
        }
        if ($run.Ngx) {
            if ($run.Ngx -eq "0x00000001") { $rows += New-Row "ok" "NGX init" "NVIDIA NGX initialised for D3D12" "success" $null }
            else { $rows += New-Row "bad" "NGX init" "NGX did not initialise - DLSS cannot be created" $run.Ngx $null }
        }
        if ($run.DlssDll) {
            if ($run.DlssDll -eq "loaded") { $rows += New-Row "ok" "DLSS runtime" "nvngx_dlss.dll from the game folder" "loaded" $null }
            else { $rows += New-Row "ok" "DLSS runtime" "the local file was not used; the driver's _nvngx provided DLSS" "driver override" $null }
        }
        # "it loaded" is only worth its own row when it did not go on to do anything; the NR row below says both.
        if ($run.Nr -eq "loaded" -and -not ($run.NrRuntime -or $run.NrCreated)) {
            $rows += New-Row "warn" "Neural Rendering add-on" "renodx-dlss5 loaded but no NR pass followed" "loaded" $null
        } elseif ($run.Nr -eq "absent") {
            $rows += New-Row "info" "Neural Rendering add-on" "not present - DLAA runs before the HUD instead" "absent" $null
        }
        if ($run.NrRuntime -or $run.NrCreated) {
            $d = "RenoDX created its NR feature (NGX feature 18) after the DLAA one"
            if ($run.NrFeature) { $d = "NR ran on the $($run.NrFeature) DLAA output (NGX feature 18)" }
            $rows += New-Row "ok" "Neural Rendering" $d $(if ($run.NrRuntime) { "nvngx_dlssnr $($run.NrRuntime)" } else { "active" }) $null
        } elseif ($run.Nr -eq "loaded") {
            $rows += New-Row "warn" "Neural Rendering" "the add-on loaded but no NR feature was created - check nvngx_dlssnr.dll and the driver" "no NR pass" $null
        }
        if ($run.Insertion) { $rows += New-Row "ok" "Insertion point" "where DLAA runs in the frame" $run.Insertion $null }
        if ($run.Sl) {
            $st = "ok"
            if ($run.FgSupported -ne "yes") { $st = "warn" }
            $rows += New-Row $st "Streamline" "DLSS-G $($run.DlssG), supported: $($run.FgSupported)" "SL $($run.Sl)" $null
        } elseif ($run.FgNoStreamline) {
            $rows += New-Row "info" "Streamline" "not loaded - frame generation was off for this run" "absent" $null
        }
        if ($run.Driver) {
            $st = "ok"
            $d = "minimum for DLSS-G is $($run.DriverMin)"
            try { if ([version]$run.Driver -lt [version]$run.DriverMin) { $st = "bad"; $d = "below the $($run.DriverMin) DLSS-G needs" } } catch {}
            $rows += New-Row $st "NVIDIA driver" $d $run.Driver $null
        }
        if ($run.Fg) { $rows += New-Row "ok" "Frame generation" "Reflex $($run.Reflex)" "$($run.Fg), target $($run.FgTarget) fps" $null }
        if ($run.Evaluations) { $rows += New-Row "ok" "DLSS evaluations" "frames DLSS actually processed in that session" $run.Evaluations $null }
    }
    $sections += [pscustomobject]@{ Id = "lastrun"; Title = "Last run"; Blurb = "What the add-on reported the last time the game started. This is the part a file list cannot tell you."; Rows = $rows }

    return $sections
}

function Get-Verdict($sections) {
    $bad = @($sections.Rows | Where-Object { $_.Status -eq "bad" }).Count
    $warn = @($sections.Rows | Where-Object { $_.Status -eq "warn" }).Count
    # "bad" is exactly the set of things a working install cannot do without, per the manifest.
    $reqBad = @($sections.Rows | Where-Object { $_.Status -eq "bad" }).Count
    if ($reqBad -gt 0) { return @{ Text = "Not ready"; Kind = "bad"; Note = "$reqBad required item(s) missing" } }
    if ($bad -gt 0) { return @{ Text = "Needs a fix"; Kind = "bad"; Note = "$bad problem(s) found" } }
    if ($warn -gt 0) { return @{ Text = "Ready"; Kind = "warn"; Note = "$warn thing(s) worth a look" } }
    return @{ Text = "Ready"; Kind = "ok"; Note = "everything checked out" }
}

function Format-TextReport($game, $sections) {
    $sb = New-Object System.Text.StringBuilder
    $v = Get-Verdict $sections
    [void]$sb.AppendLine("MGS4 DLSS - install check")
    [void]$sb.AppendLine("game    : $game")
    [void]$sb.AppendLine("checked : $(Get-Date -Format 'yyyy-MM-dd HH:mm')")
    [void]$sb.AppendLine("verdict : $($v.Text) - $($v.Note)")
    foreach ($sec in $sections) {
        [void]$sb.AppendLine("")
        [void]$sb.AppendLine("[$($sec.Title)]")
        if ($sec.Url) { [void]$sb.AppendLine("  " + $sec.Url) }
        foreach ($r in $sec.Rows) {
            $mark = switch ($r.Status) { "ok" { "ok  " } "warn" { "warn" } "bad" { "MISS" } default { "    " } }
            [void]$sb.AppendLine(("  {0}  {1,-28} {2}" -f $mark, $r.Name, $r.Value))
            if ($r.Status -ne "ok" -and $r.Detail) { [void]$sb.AppendLine("        $($r.Detail)") }
            if ($r.Status -ne "ok" -and $r.Url) { [void]$sb.AppendLine("        $($r.Url)") }
        }
    }
    return $sb.ToString()
}

# ---------------------------------------------------------------------------------------------- drag and drop

# What a dropped file is: which manifest group claims it, by the "accepts" patterns.
function Get-DropTarget($sections, [string]$name) {
    foreach ($sec in $sections) {
        foreach ($pat in @($sec.Accepts)) {
            if ($pat -and $name -like $pat) { return $sec }
        }
    }
    return $null
}

# The file names the drop area names, straight from the manifest so the two cannot drift.
function Get-DropNames {
    $manifest = Get-Content -LiteralPath $script:ManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $names = @()
    foreach ($sec in $manifest.sections) { foreach ($d in @($sec.drop)) { if ($d) { $names += $d } } }
    return $names
}

# Where a dropped or unpacked file belongs. Most sit next to mgs4.exe; anything the archive already put in a
# scripts folder, and any .asi, belongs in scripts\ - MGSFPSUnlock.zip ships exactly that shape.
function Get-DropFolder([string]$gameDir, [string]$insideArchive, [string]$name) {
    $parts = @($insideArchive -split '[\\/]')
    if (($parts | Where-Object { $_ -eq "scripts" }) -or ([IO.Path]::GetExtension($name) -eq ".asi")) {
        return (Join-Mgs4Path $gameDir "scripts")
    }
    return $gameDir
}

# Everything the install can legitimately receive, so a stray file in a zip is never written into the game folder.
# Anything not matching one of these is reported as skipped rather than copied.
$script:DropPatterns = @("sl.*.dll", "nvngx_*.dll", "*.addon64", "mgs4_dlss.ini", "winmm.dll", "wininet.dll",
                         "*.asi", "MGSFPSUnlock.ini", "steam_appid.txt", "*license*")

function Test-DropAllowed([string]$name) {
    foreach ($pat in $script:DropPatterns) { if ($name -like $pat) { return $true } }
    return $false
}

# Puts dropped files where the manifest says they go. Zips are unpacked flat into the game folder, which is what
# streamline.zip wants; a ReShade setup is an interactive installer, so it is started rather than copied.
# Returns lines describing what happened - the caller shows them.
function Copy-DroppedFiles($sections, [string]$gameDir, [string[]]$paths) {
    $log = New-Object System.Collections.Generic.List[string]
    if (-not $gameDir) { $log.Add("no game folder set - pick one above first"); return $log }

    foreach ($path in $paths) {
        if (-not (Test-Mgs4Path $path)) { $log.Add("gone: $path"); continue }
        $name = [IO.Path]::GetFileName($path)

        if ($name -like "ReShade_Setup*.exe") {
            try {
                Start-Process -FilePath $path | Out-Null
                $log.Add("started $name - point it at mgs4.exe, pick Direct3D 10/11/12, and tick no shader packs")
            } catch { $log.Add("could not start ${name}: $($_.Exception.Message)") }
            continue
        }

        if ([IO.Path]::GetExtension($name) -eq ".zip") {
            try { Add-Type -AssemblyName System.IO.Compression.FileSystem -ErrorAction SilentlyContinue } catch { }
            $zip = $null
            try {
                $zip = [System.IO.Compression.ZipFile]::OpenRead($path)
                $took = 0; $skipped = 0
                foreach ($entry in $zip.Entries) {
                    if (-not $entry.Name) { continue }                      # a directory entry
                    if (-not (Test-DropAllowed $entry.Name)) { $skipped++; continue }
                    $into = Get-DropFolder $gameDir $entry.FullName $entry.Name
                    if (-not (Test-Mgs4Path $into)) { New-Item -ItemType Directory -Force $into | Out-Null }
                    $dest = Join-Mgs4Path (Get-DropFolder $gameDir $entry.FullName $entry.Name) $entry.Name
                    [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $dest, $true)
                    $took++
                }
                $log.Add("$name -> $took file(s) into the game folder" + $(if ($skipped) { ", $skipped skipped" } else { "" }))
            } catch {
                $log.Add("could not read ${name}: $($_.Exception.Message)")
            } finally { if ($zip) { $zip.Dispose() } }
            continue
        }

        if (-not (Test-DropAllowed $name)) { $log.Add("skipped $name - not part of the install"); continue }
        $sec = Get-DropTarget $sections $name
        # .asi files live in scripts\, everything else sits next to mgs4.exe
        $dest = Get-DropFolder $gameDir $name $name
        if (-not (Test-Mgs4Path $dest)) { New-Item -ItemType Directory -Force $dest | Out-Null }
        try {
            Copy-Item -LiteralPath $path -Destination (Join-Mgs4Path $dest $name) -Force
            $log.Add("$name -> " + $(if ($sec) { $sec.Title } else { "the game folder" }))
        } catch {
            $log.Add("could not copy ${name}: $($_.Exception.Message)")
        }
    }
    return $log
}
