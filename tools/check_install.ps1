# Install check for the MGS4 DLSS add-on: a window listing every required and optional file, the settings that
# matter, and what the add-on actually did on its last run. Refresh (F5) re-runs every check with the window open,
# so it can be left up while files are dropped into the game folder.
#
#   check-install.bat                                        (double-click, or from a terminal)
#   powershell -ExecutionPolicy Bypass -File tools\check_install.ps1 [-GameDir "..."] [-Report]
#
# -Report prints the same findings as text and exits (no window) - that is the form to paste into an issue.
# The file list, the versions this was verified against and the download links live in tools\install_manifest.json.
param(
    [string]$GameDir = "",      # default: MGS4_DIR / config.ini / the Steam libraries (tools\paths.ps1)
    [switch]$Report             # text to stdout instead of a window
)
$ErrorActionPreference = "Continue"
. "$PSScriptRoot\paths.ps1"

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
    $m = Select-String -Path $path -Pattern ("^\s*" + [regex]::Escape($key) + "\s*=\s*(.*?)\s*$") | Select-Object -First 1
    if ($m) { return $m.Matches[0].Groups[1].Value }
    return $null
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
        if ($line -match 'DLSS-NR feature is not supported')                               { $r.NrUnsupported = $true }
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
    }
    return $r
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
            $value = $f.path
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
                if ($sec.id -eq "required") { $status = "bad" } else { $status = "warn" }
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
            $rows += New-Row $status $f.name $detail $value $f.url
        }
        $sections += [pscustomobject]@{ Id = $sec.id; Title = $sec.title; Blurb = $sec.blurb; Rows = $rows }
    }

    # ------------------------------------------------------------------ settings
    $rows = @()
    $ss = Get-SavedSettingsPath $Game
    if ($ss) {
        $want = [ordered]@{ api = "dx12"; vsync = "false"; fpsLimiter = "60"; enableFXAA = "false" }
        $why = @{ api = "DLSS needs the D3D12 backend"; vsync = "off, the frame limiter paces instead";
                  fpsLimiter = "the port is built around 60"; enableFXAA = "DLAA replaces it; both together smears" }
        foreach ($k in $want.Keys) {
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

    $ini = Join-Mgs4Path $Game "mgs4_dlss.ini"
    if (Test-Mgs4Path $ini) {
        $en = Get-IniValue $ini "Enabled"
        if ($en -eq "1") { $rows += New-Row "ok" "Add-on: Enabled" "DLSS is switched on in the ini" "1" $null }
        else { $rows += New-Row "bad" "Add-on: Enabled" "the add-on loads but does nothing while this is 0" "$en" $null }

        $mode = Get-IniValue $ini "Mode"; $res = Get-IniValue $ini "InternalRes"
        $rows += New-Row "info" "Add-on: Mode" "InternalRes $res - must match the resolution the game renders at" "$mode" $null

        $fgm = Get-IniValue $ini "FrameGen"; $fgt = Get-IniValue $ini "FGTargetFps"; $hz = Get-RefreshRate
        if ($fgm -and $fgm -ne "0") {
            $d = "frame generation on"
            $st = "ok"
            if ($hz -gt 0) {
                $d = "display reports $hz Hz"
                if ($hz -le 61) { $st = "warn"; $d = "display reports $hz Hz - frame generation cannot show extra frames at 60 Hz, set FrameGen=0" }
                elseif ($fgt -and [int]$fgt -gt $hz) { $st = "warn"; $d = "target $fgt fps is above the display's $hz Hz" }
            }
            $rows += New-Row $st "Add-on: FrameGen" $d "$fgm, target $fgt" $null
        } else {
            $rows += New-Row "info" "Add-on: FrameGen" "off; the Streamline files are then not needed" "0" $null
        }

        $pd = Get-IniValue $ini "PostDof"
        if ($pd -eq "1") { $rows += New-Row "ok" "Add-on: PostDof" "the game's depth of field is re-applied after DLSS / NR" "1" $null }
        else { $rows += New-Row "info" "Add-on: PostDof" "off - the game's own DoF blurs the pre-DLSS image" "$pd" $null }

        foreach ($diag in @("DebugMode", "Probe", "TraceFrames", "TraceFreeze", "DumpShaders")) {
            $v = Get-IniValue $ini $diag
            if ($v -and $v -ne "0") {
                $rows += New-Row "warn" "Add-on: $diag" "a diagnostic is on - it costs frames, set it to 0 for normal play" $v $null
            }
        }
    }

    $rini = Join-Mgs4Path $Game "ReShade.ini"
    if (Test-Mgs4Path $rini) {
        $dis = Get-IniValue $rini "DisabledAddons"
        if ($dis -and $dis -match "mgs4_dlss") {
            $rows += New-Row "bad" "ReShade: add-on disabled" "mgs4_dlss is in ReShade's DisabledAddons list" $dis $null
        }
        $up = Get-IniValue $rini "NREnableUpscaling"
        if ($null -ne $up) {
            if ($up -eq "0") { $rows += New-Row "ok" "RenoDX: NREnableUpscaling" "off, as it should be - this add-on already runs DLAA on the final image" "0" $null }
            else { $rows += New-Row "warn" "RenoDX: NREnableUpscaling" "on, on top of this add-on's own DLAA - two upscalers in a row" $up $null }
        }
    }

    $asi = Join-Mgs4Path $Game "scripts\MGS4_D3D12.ini"
    if (Test-Mgs4Path $asi) {
        $e = Get-IniValue $asi "Enabled"
        $api = $null
        if ($ss) { $api = Get-IniValue $ss "api" }
        if ($e -eq "1" -and $api -eq "dx12") {
            $rows += New-Row "warn" "D3D12 switch" "the ASI forces D3D12 while the game is already set to it - leave Enabled = 0" "Enabled = 1" $null
        } elseif ($e -eq "1") {
            $rows += New-Row "ok" "D3D12 switch" "forcing the D3D12 backend (the fallback path)" "Enabled = 1" $null
        } else {
            $rows += New-Row "info" "D3D12 switch" "off - the in-game DirectX 12 option is doing the job" "Enabled = 0" $null
        }
    }
    $sections += [pscustomobject]@{ Id = "settings"; Title = "Settings"; Blurb = "Read out of the game's own files - these decide whether the pieces above actually do anything."; Rows = $rows }

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
        if ($run.Nr) {
            if ($run.Nr -eq "loaded") { $rows += New-Row "ok" "Neural Rendering add-on" "renodx-dlss5 was loaded in the process" "loaded" $null }
            else { $rows += New-Row "info" "Neural Rendering add-on" "not present - DLAA runs before the HUD instead" "absent" $null }
        }
        if ($run.NrUnsupported) {
            $rows += New-Row "warn" "DLSS NR" "Streamline reported NR unsupported - check nvngx_dlssnr.dll and the driver" "unsupported" $null
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
    $req = $sections | Where-Object { $_.Id -eq "required" }
    $bad = @($sections.Rows | Where-Object { $_.Status -eq "bad" }).Count
    $warn = @($sections.Rows | Where-Object { $_.Status -eq "warn" }).Count
    $reqBad = 0
    if ($req) { $reqBad = @($req.Rows | Where-Object { $_.Status -ne "ok" }).Count }
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
        foreach ($r in $sec.Rows) {
            $mark = switch ($r.Status) { "ok" { "ok  " } "warn" { "warn" } "bad" { "MISS" } default { "    " } }
            [void]$sb.AppendLine(("  {0}  {1,-30} {2}" -f $mark, $r.Name, $r.Value))
            if ($r.Status -ne "ok" -and $r.Detail) { [void]$sb.AppendLine("        $($r.Detail)") }
            if ($r.Status -ne "ok" -and $r.Url) { [void]$sb.AppendLine("        $($r.Url)") }
        }
    }
    return $sb.ToString()
}

# ---------------------------------------------------------------------------------------------- text mode

$game = $GameDir
if (-not $game) { $game = (Get-Mgs4Paths).GameDir }
if (-not $game -or -not (Test-Mgs4Path (Join-Mgs4Path $game "mgs4.exe"))) {
    $msg = "mgs4.exe was not found. Set MGS4_DIR in config.ini or pass -GameDir ""<path to MGS4>""."
    if ($Report) { Write-Host $msg -ForegroundColor Red; exit 1 }
    Add-Type -AssemblyName PresentationFramework
    [void][System.Windows.MessageBox]::Show($msg, "MGS4 DLSS - install check")
    exit 1
}

if ($Report) {
    Write-Host (Format-TextReport $game (Invoke-InstallChecks -Game $game))
    exit 0
}

# ---------------------------------------------------------------------------------------------- window

Add-Type -AssemblyName PresentationFramework, PresentationCore, WindowsBase

$xaml = @'
<Window xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
        xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
        Title="MGS4 DLSS - install check" Height="820" Width="1000" MinHeight="480" MinWidth="720"
        Background="#0F1116" WindowStartupLocation="CenterScreen" TextOptions.TextFormattingMode="Ideal">
  <Window.Resources>
    <SolidColorBrush x:Key="Card" Color="#171A21"/>
    <SolidColorBrush x:Key="Line" Color="#242935"/>
    <SolidColorBrush x:Key="Text" Color="#E7EAF0"/>
    <SolidColorBrush x:Key="Muted" Color="#858D9E"/>
    <SolidColorBrush x:Key="Accent" Color="#7C9CFF"/>
    <Style TargetType="ScrollBar">
      <Setter Property="Background" Value="Transparent"/>
      <Setter Property="Width" Value="11"/>
      <Setter Property="Template">
        <Setter.Value>
          <ControlTemplate TargetType="ScrollBar">
            <Grid Background="Transparent">
              <Track x:Name="PART_Track" IsDirectionReversed="True">
                <Track.Thumb>
                  <Thumb>
                    <Thumb.Template>
                      <ControlTemplate TargetType="Thumb">
                        <Border x:Name="t" CornerRadius="4" Background="#333B4C" Margin="3,2"/>
                        <ControlTemplate.Triggers>
                          <Trigger Property="IsMouseOver" Value="True">
                            <Setter TargetName="t" Property="Background" Value="#46506681"/>
                          </Trigger>
                        </ControlTemplate.Triggers>
                      </ControlTemplate>
                    </Thumb.Template>
                  </Thumb>
                </Track.Thumb>
                <Track.IncreaseRepeatButton>
                  <RepeatButton Command="ScrollBar.PageDownCommand" Opacity="0" Focusable="False"/>
                </Track.IncreaseRepeatButton>
                <Track.DecreaseRepeatButton>
                  <RepeatButton Command="ScrollBar.PageUpCommand" Opacity="0" Focusable="False"/>
                </Track.DecreaseRepeatButton>
              </Track>
            </Grid>
          </ControlTemplate>
        </Setter.Value>
      </Setter>
    </Style>
    <Style x:Key="Flat" TargetType="Button">
      <Setter Property="Foreground" Value="#E7EAF0"/>
      <Setter Property="Background" Value="#222736"/>
      <Setter Property="BorderBrush" Value="#333A4D"/>
      <Setter Property="Padding" Value="16,8"/>
      <Setter Property="FontSize" Value="13"/>
      <Setter Property="Cursor" Value="Hand"/>
      <Setter Property="Template">
        <Setter.Value>
          <ControlTemplate TargetType="Button">
            <Border x:Name="b" CornerRadius="6" Background="{TemplateBinding Background}"
                    BorderBrush="{TemplateBinding BorderBrush}" BorderThickness="1" Padding="{TemplateBinding Padding}">
              <ContentPresenter HorizontalAlignment="Center" VerticalAlignment="Center"/>
            </Border>
            <ControlTemplate.Triggers>
              <Trigger Property="IsMouseOver" Value="True">
                <Setter TargetName="b" Property="Background" Value="#2C3346"/>
              </Trigger>
            </ControlTemplate.Triggers>
          </ControlTemplate>
        </Setter.Value>
      </Setter>
    </Style>
    <Style x:Key="Primary" TargetType="Button" BasedOn="{StaticResource Flat}">
      <Setter Property="Background" Value="#3A5BD9"/>
      <Setter Property="BorderBrush" Value="#4E6DE8"/>
      <Setter Property="FontWeight" Value="SemiBold"/>
    </Style>
    <Style x:Key="Link" TargetType="Button" BasedOn="{StaticResource Flat}">
      <Setter Property="Background" Value="#1D2432"/>
      <Setter Property="BorderBrush" Value="#31405E"/>
      <Setter Property="Foreground" Value="#9FB6FF"/>
      <Setter Property="Padding" Value="10,3"/>
      <Setter Property="FontSize" Value="11"/>
    </Style>
  </Window.Resources>

  <Grid>
    <Grid.RowDefinitions>
      <RowDefinition Height="Auto"/>
      <RowDefinition Height="*"/>
      <RowDefinition Height="Auto"/>
    </Grid.RowDefinitions>

    <Border Grid.Row="0" Background="#141821" BorderBrush="{StaticResource Line}" BorderThickness="0,0,0,1" Padding="22,18">
      <Grid>
        <Grid.ColumnDefinitions>
          <ColumnDefinition Width="*"/>
          <ColumnDefinition Width="Auto"/>
        </Grid.ColumnDefinitions>
        <StackPanel Grid.Column="0">
          <TextBlock Text="MGS4 DLSS" FontSize="21" FontWeight="SemiBold" Foreground="{StaticResource Text}"/>
          <TextBlock Text="installation check" FontSize="13" Foreground="{StaticResource Accent}" Margin="0,1,0,6"/>
          <TextBlock x:Name="GamePath" FontSize="11" Foreground="{StaticResource Muted}" FontFamily="Consolas" TextTrimming="CharacterEllipsis"/>
        </StackPanel>
        <Border x:Name="Pill" Grid.Column="1" CornerRadius="8" Padding="18,10" Background="#16281C" BorderBrush="#2E7D4F" BorderThickness="1" VerticalAlignment="Center">
          <StackPanel>
            <TextBlock x:Name="PillText" Text="checking" FontSize="17" FontWeight="SemiBold" Foreground="#7BE0A0" HorizontalAlignment="Center"/>
            <TextBlock x:Name="PillNote" Text="" FontSize="11" Foreground="{StaticResource Muted}" HorizontalAlignment="Center" Margin="0,2,0,0"/>
          </StackPanel>
        </Border>
      </Grid>
    </Border>

    <ScrollViewer Grid.Row="1" VerticalScrollBarVisibility="Auto" Padding="22,18,18,18">
      <StackPanel x:Name="Host"/>
    </ScrollViewer>

    <Border Grid.Row="2" Background="#141821" BorderBrush="{StaticResource Line}" BorderThickness="0,1,0,0" Padding="22,14">
      <Grid>
        <Grid.ColumnDefinitions>
          <ColumnDefinition Width="*"/>
          <ColumnDefinition Width="Auto"/>
        </Grid.ColumnDefinitions>
        <TextBlock x:Name="Stamp" Grid.Column="0" VerticalAlignment="Center" FontSize="11" Foreground="{StaticResource Muted}"/>
        <StackPanel Grid.Column="1" Orientation="Horizontal">
          <Button x:Name="CopyBtn" Content="Copy report" Style="{StaticResource Flat}" Margin="0,0,10,0"/>
          <Button x:Name="RefreshBtn" Content="Refresh  (F5)" Style="{StaticResource Primary}"/>
        </StackPanel>
      </Grid>
    </Border>
  </Grid>
</Window>
'@

$reader = New-Object System.Xml.XmlNodeReader ([xml]$xaml)
$win = [Windows.Markup.XamlReader]::Load($reader)

Add-Type -Namespace Mgs4 -Name Dwm -MemberDefinition @'
[DllImport("dwmapi.dll")] public static extern int DwmSetWindowAttribute(IntPtr h, int attr, ref int val, int size);
'@
$win.Add_SourceInitialized({
    $h = (New-Object System.Windows.Interop.WindowInteropHelper $win).Handle
    $on = 1
    # 20 = DWMWA_USE_IMMERSIVE_DARK_MODE; 19 on Windows 10 builds before 20H1
    if ([Mgs4.Dwm]::DwmSetWindowAttribute($h, 20, [ref]$on, 4) -ne 0) {
        [void][Mgs4.Dwm]::DwmSetWindowAttribute($h, 19, [ref]$on, 4)
    }
})

$Host_    = $win.FindName("Host")
$PillEl   = $win.FindName("Pill")
$PillText = $win.FindName("PillText")
$PillNote = $win.FindName("PillNote")
$GamePath = $win.FindName("GamePath")
$Stamp    = $win.FindName("Stamp")

function ConvertTo-Brush($hex) {
    return New-Object System.Windows.Media.SolidColorBrush ([System.Windows.Media.ColorConverter]::ConvertFromString($hex))
}

$script:Style = @{
    ok   = @{ Glyph = [char]0x2714; Fg = "#5FD38D"; Bg = "#152318"; Br = "#2C6B45" }
    warn = @{ Glyph = [char]0x25B2; Fg = "#F2C14E"; Bg = "#251E10"; Br = "#7A6027" }
    bad  = @{ Glyph = [char]0x2716; Fg = "#FF7B72"; Bg = "#2A1618"; Br = "#7E3B3B" }
    info = @{ Glyph = [char]0x25CF; Fg = "#7C9CFF"; Bg = "#161B2A"; Br = "#33436E" }
}

function New-TextBlock($text, $size, $color, $bold, $mono) {
    $t = New-Object System.Windows.Controls.TextBlock
    $t.Text = [string]$text
    $t.FontSize = $size
    $t.Foreground = ConvertTo-Brush $color
    if ($bold) { $t.FontWeight = [System.Windows.FontWeights]::SemiBold }
    if ($mono) { $t.FontFamily = New-Object System.Windows.Media.FontFamily "Consolas" }
    $t.TextWrapping = [System.Windows.TextWrapping]::Wrap
    return $t
}

function New-SectionCard($sec) {
    $card = New-Object System.Windows.Controls.Border
    $card.Background = ConvertTo-Brush "#171A21"
    $card.BorderBrush = ConvertTo-Brush "#242935"
    $card.BorderThickness = New-Object System.Windows.Thickness 1
    $card.CornerRadius = New-Object System.Windows.CornerRadius 10
    $card.Margin = New-Object System.Windows.Thickness 0, 0, 0, 14
    $stack = New-Object System.Windows.Controls.StackPanel

    # header
    $hdr = New-Object System.Windows.Controls.Border
    $hdr.Background = ConvertTo-Brush "#1B1F29"
    $hdr.BorderBrush = ConvertTo-Brush "#242935"
    $hdr.BorderThickness = New-Object System.Windows.Thickness 0, 0, 0, 1
    $hdr.CornerRadius = New-Object System.Windows.CornerRadius 10, 10, 0, 0
    $hdr.Padding = New-Object System.Windows.Thickness 18, 13, 18, 13
    $hg = New-Object System.Windows.Controls.Grid
    $c1 = New-Object System.Windows.Controls.ColumnDefinition; $c1.Width = "*"
    $c2 = New-Object System.Windows.Controls.ColumnDefinition; $c2.Width = "Auto"
    [void]$hg.ColumnDefinitions.Add($c1); [void]$hg.ColumnDefinitions.Add($c2)
    $hs = New-Object System.Windows.Controls.StackPanel
    [void]$hs.Children.Add((New-TextBlock $sec.Title 14 "#E7EAF0" $true $false))
    if ($sec.Blurb) {
        $b = New-TextBlock $sec.Blurb 11 "#858D9E" $false $false
        $b.Margin = New-Object System.Windows.Thickness 0, 2, 12, 0
        [void]$hs.Children.Add($b)
    }
    [void]$hg.Children.Add($hs)

    $bad = @($sec.Rows | Where-Object { $_.Status -eq "bad" }).Count
    $warn = @($sec.Rows | Where-Object { $_.Status -eq "warn" }).Count
    $kind = "ok"; $label = "all good"
    if ($warn -gt 0) { $kind = "warn"; $label = "$warn to look at" }
    if ($bad -gt 0) { $kind = "bad"; $label = "$bad missing" }
    if (@($sec.Rows).Count -eq 0) { $kind = "info"; $label = "nothing to check" }
    $st = $script:Style[$kind]
    $tag = New-Object System.Windows.Controls.Border
    $tag.Background = ConvertTo-Brush $st.Bg
    $tag.BorderBrush = ConvertTo-Brush $st.Br
    $tag.BorderThickness = New-Object System.Windows.Thickness 1
    $tag.CornerRadius = New-Object System.Windows.CornerRadius 20
    $tag.Padding = New-Object System.Windows.Thickness 12, 4, 12, 4
    $tag.VerticalAlignment = [System.Windows.VerticalAlignment]::Center
    $tag.Child = (New-TextBlock $label 11 $st.Fg $true $false)
    [System.Windows.Controls.Grid]::SetColumn($tag, 1)
    [void]$hg.Children.Add($tag)
    $hdr.Child = $hg
    [void]$stack.Children.Add($hdr)

    # rows
    $i = 0
    foreach ($row in $sec.Rows) {
        $st = $script:Style[$row.Status]
        $rb = New-Object System.Windows.Controls.Border
        $rb.Padding = New-Object System.Windows.Thickness 18, 11, 18, 11
        if ($i -gt 0) {
            $rb.BorderBrush = ConvertTo-Brush "#20242E"
            $rb.BorderThickness = New-Object System.Windows.Thickness 0, 1, 0, 0
        }
        $g = New-Object System.Windows.Controls.Grid
        foreach ($w in @("26", "*", "Auto")) {
            $cd = New-Object System.Windows.Controls.ColumnDefinition
            $cd.Width = $w
            [void]$g.ColumnDefinitions.Add($cd)
        }
        $glyph = New-TextBlock $st.Glyph 13 $st.Fg $true $false
        $glyph.VerticalAlignment = [System.Windows.VerticalAlignment]::Center
        [void]$g.Children.Add($glyph)

        $mid = New-Object System.Windows.Controls.StackPanel
        $mid.VerticalAlignment = [System.Windows.VerticalAlignment]::Center
        $mid.Margin = New-Object System.Windows.Thickness 0, 0, 16, 0
        [void]$mid.Children.Add((New-TextBlock $row.Name 13 "#E7EAF0" $false $false))
        if ($row.Detail) {
            $d = New-TextBlock $row.Detail 11 "#858D9E" $false $false
            $d.Margin = New-Object System.Windows.Thickness 0, 2, 0, 0
            [void]$mid.Children.Add($d)
        }
        [System.Windows.Controls.Grid]::SetColumn($mid, 1)
        [void]$g.Children.Add($mid)

        $right = New-Object System.Windows.Controls.StackPanel
        $right.Orientation = [System.Windows.Controls.Orientation]::Horizontal
        $right.HorizontalAlignment = [System.Windows.HorizontalAlignment]::Right
        $right.VerticalAlignment = [System.Windows.VerticalAlignment]::Center
        $val = New-TextBlock $row.Value 11 $st.Fg $false $true
        $val.VerticalAlignment = [System.Windows.VerticalAlignment]::Center
        $val.TextWrapping = [System.Windows.TextWrapping]::NoWrap
        [void]$right.Children.Add($val)
        if ($row.Url -and $row.Status -ne "ok") {
            $lb = New-Object System.Windows.Controls.Button
            $lb.Content = "Get it  " + [char]0x2192
            $lb.Style = $win.FindResource("Link")
            $lb.Tag = $row.Url
            $lb.Margin = New-Object System.Windows.Thickness 12, 0, 0, 0
            $lb.VerticalAlignment = [System.Windows.VerticalAlignment]::Center
            $lb.ToolTip = $row.Url
            $lb.Add_Click({ Start-Process $this.Tag })
            [void]$right.Children.Add($lb)
        }
        [System.Windows.Controls.Grid]::SetColumn($right, 2)
        [void]$g.Children.Add($right)

        $rb.Child = $g
        [void]$stack.Children.Add($rb)
        $i++
    }
    $card.Child = $stack
    return $card
}

function Update-View {
    $script:Sections = Invoke-InstallChecks -Game $game
    $Host_.Children.Clear()
    foreach ($sec in $script:Sections) { [void]$Host_.Children.Add((New-SectionCard $sec)) }
    $v = Get-Verdict $script:Sections
    $st = $script:Style[$v.Kind]
    $PillEl.Background = ConvertTo-Brush $st.Bg
    $PillEl.BorderBrush = ConvertTo-Brush $st.Br
    $PillText.Foreground = ConvertTo-Brush $st.Fg
    $PillText.Text = $v.Text
    $PillNote.Text = $v.Note
    $GamePath.Text = $game
    $Stamp.Text = "checked at " + (Get-Date -Format "HH:mm:ss") + "  -  manifest: tools\install_manifest.json"
}

$win.FindName("RefreshBtn").Add_Click({ Update-View })
$win.FindName("CopyBtn").Add_Click({
    Set-Clipboard -Value (Format-TextReport $game $script:Sections)
    $this.Content = "Copied"
    $t = New-Object System.Windows.Threading.DispatcherTimer
    $t.Interval = [TimeSpan]::FromSeconds(1.6)
    $b = $this
    $t.Add_Tick({ $b.Content = "Copy report"; $t.Stop() })
    $t.Start()
})
$win.Add_KeyDown({ if ($_.Key -eq [System.Windows.Input.Key]::F5) { Update-View } })

Update-View
[void]$win.ShowDialog()
