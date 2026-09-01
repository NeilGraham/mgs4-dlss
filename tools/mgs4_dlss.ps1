# MGS4 DLSS: the app for this add-on. Start the game or any single scene in it, set the add-on up, and check the
# install - one window with three tabs, and the same things as a command line.
#
#   mgs4-dlss                                the window
#   mgs4-dlss s02a50l_D1                     boot that scene, no window
#   mgs4-dlss --main                         straight to MGS4's main menu (past the collection screen)
#   mgs4-dlss --list naomi                   what can be launched
#   mgs4-dlss --setup                        the window, on Setup (game folder + install check)
#   mgs4-dlss --report                       the install check as text, for pasting into an issue
#   mgs4-dlss s02a50l_D1 --shortcut "C:\...\scene.lnk"   save that scene, with its options, as a shortcut
#   mgs4-dlss --set FrameGen=0 --set Mode=Quality
#
# There is deliberately NO param() block: PowerShell then hands every argument through in $args verbatim, so the
# --flag spellings above survive `powershell -File`. Run options are parsed by Read-Options below.
#
# Everything the repo could already do to a scene lives here: the boot (`mgs4.exe --stage`), pressing through the
# auto-save / "press any button" prompts until the first 3D frame (was tools\launch_stage.ps1), tapping Cross for the
# in-cutscene flashback prompts and ending a scene when gameplay starts (was tools\record_cutscenes.py, without the
# recording). The install check itself is tools\install_checks.ps1; this renders it.
$ErrorActionPreference = "Continue"
. "$PSScriptRoot\paths.ps1"
. "$PSScriptRoot\install_checks.ps1"

$script:ScenesCsv = Join-Path $PSScriptRoot "scenes.csv"
$script:LabelsJson = Join-Path $PSScriptRoot "labels.json"
$script:SceneInfoJson = Join-Path $PSScriptRoot "scene_info.json"
$script:ShippedIni = Join-Path (Split-Path -Parent $PSScriptRoot) "dlss-addon\mgs4_dlss.ini"
$script:PrefsPath = Join-Path $env:LOCALAPPDATA "mgs4-dlss\launcher.json"

# ---------------------------------------------------------------------------------------------- options

function New-Options {
    return [ordered]@{
        Action        = ""          # "" = launch, or ui / install / report / list / settings / set /
                                    #      shortcuts / stop / help
        Stage         = ""          # a stage id, or one of the @-entries in the catalogue
        GameDir       = ""
        GameDirGiven  = $false      # true only when --game-dir was passed, so previews do not echo a detected path
        GameDirBad    = ""          # a folder that was named but holds no mgs4.exe, kept for the Install tab
        Advance       = $true       # press through the boot prompts until the first 3D frame
        MashX         = $false      # keep tapping Cross so the flashback prompts fire
        Keys          = ""          # an explicit key sequence instead of pressing through the prompts
        Settle        = 2.0         # seconds after the window appears before the key sequence starts
        PressKey      = ""          # force a keyboard key for the advance loop instead of the pad's Cross
        SceneDetect   = $true       # stop the advance loop at the first 3D frame
        EndOnGameplay = $false      # quit once the cutscene hands over to gameplay
        Hold          = 0.0         # stop after N seconds in the scene (0 = no limit)
        MaxMinutes    = 0.0         # ceiling on the whole run (0 = none)
        StartTimeout  = 100.0
        PressEvery    = 0.16
        PressHold     = 0.07
        MinSeconds    = 30.0        # never call a scene "over" before this much of it has played
        GameplayGrace = 9.0
        StaticGrace   = 25.0
        HudMin        = 40          # HUD draws in one heartbeat that mean the HUD is up
        Width         = 0
        Height        = 0
        Windowing     = ""
        Restart       = $true       # close a running game first
        KeepRunning   = $false      # leave the game up when the scene ends
        Quiet         = $false
        Filter        = ""
        Sets          = @()
        ShortcutPath  = ""          # --shortcut <file>: write a .lnk for this scene instead of launching it
    }
}

function Read-Options([string[]]$argv) {
    $o = New-Options
    $i = 0
    # -match rather than a -regex switch: switch runs *every* matching branch, so "--advance" would also fall into
    # the "^-" catch-all and be rejected as unknown.
    while ($i -lt $argv.Count) {
        $a = "$($argv[$i])"
        $value = $null
        if ($i + 1 -lt $argv.Count) { $value = "$($argv[$i + 1])" }
        $took = $true
        if     ($a -match '^(--help|-h|/\?)$')            { $o.Action = "help"; $took = $false }
        elseif ($a -match '^--(ui|window)$')              { $o.Action = "ui"; $took = $false }
        elseif ($a -match '^--list$')                     { $o.Action = "list"; $took = $false }
        elseif ($a -match '^--(show-settings|settings)$') { $o.Action = "settings"; $took = $false }
        elseif ($a -match '^--(setup|install|check|check-install)$') { $o.Action = "install"; $took = $false }
        elseif ($a -match '^--report$')                   { $o.Action = "report"; $took = $false }
        elseif ($a -match '^--stop$')                     { $o.Action = "stop"; $took = $false }
        elseif ($a -match '^--main$')                     { $o.Stage = "@main"; $took = $false }
        elseif ($a -match '^--collection$')               { $o.Stage = "@collection"; $took = $false }
        elseif ($a -match '^--title$') {
            throw "--title is gone: mgs4.exe --stage s00title_1 crashes the game on its own (verified with the add-on idle and frame generation off). Use --main."
        }
        elseif ($a -match '^--advance$')                  { $o.Advance = $true; $took = $false }
        elseif ($a -match '^--no-advance$')               { $o.Advance = $false; $took = $false }
        elseif ($a -match '^--(mash-x|mash|flashbacks)$') { $o.MashX = $true; $took = $false }
        elseif ($a -match '^--(end-on-gameplay|end-on-hud)$') { $o.EndOnGameplay = $true; $took = $false }
        elseif ($a -match '^--(no-restart|attach)$')      { $o.Restart = $false; $took = $false }
        elseif ($a -match '^--keep-running$')             { $o.KeepRunning = $true; $took = $false }
        elseif ($a -match '^--quiet$')                    { $o.Quiet = $true; $took = $false }
        elseif ($a -match '^--no-scene-detect$')          { $o.SceneDetect = $false; $took = $false }
        elseif ($a -match '^-') {
            if ($null -eq $value) { throw "$a needs a value  (--help lists the options)" }
            if     ($a -match '^--?-?stage$')      { $o.Stage = $value }
            elseif ($a -match '^--set$')           { $o.Action = "set"; $o.Sets += $value }
            elseif ($a -match '^--hold$')          { $o.Hold = [double]$value }
            elseif ($a -match '^--max-minutes$')   { $o.MaxMinutes = [double]$value }
            elseif ($a -match '^--start-timeout$') { $o.StartTimeout = [double]$value }
            elseif ($a -match '^--min-seconds$')   { $o.MinSeconds = [double]$value }
            elseif ($a -match '^--press-every$')   { $o.PressEvery = [double]$value }
            elseif ($a -match '^--hud-min$')       { $o.HudMin = [int]$value }
            elseif ($a -match '^--windowing$')     { $o.Windowing = $value }
            elseif ($a -match '^--keys$')          { $o.Keys = $value }
            elseif ($a -match '^--press-key$')     { $o.PressKey = $value }
            elseif ($a -match '^--settle$')        { $o.Settle = [double]$value }
            elseif ($a -match '^--?-?game-?dir$')  { $o.GameDir = $value; $o.GameDirGiven = $true }
            elseif ($a -match '^--shortcut$')      { $o.Action = "shortcut"; $o.ShortcutPath = $value }
            elseif ($a -match '^--res$') {
                if ($value -match '^(\d+)\s*[xX]\s*(\d+)$') { $o.Width = [int]$Matches[1]; $o.Height = [int]$Matches[2] }
                else { throw "--res wants WIDTHxHEIGHT, got $value" }
            }
            else { throw "unknown option $a  (--help lists them)" }
        }
        else {
            $took = $false
            if ($o.Action -eq "list") { $o.Filter = $a }
            elseif ($o.Stage -eq "") { $o.Stage = $a }
            else { throw "unexpected argument $a" }
        }
        $i += $(if ($took) { 2 } else { 1 })
    }
    return $o
}

$script:HelpText = @'
MGS4 DLSS - start the game or one scene of it, set the add-on up, check the install.

  mgs4-dlss                         open the window (Play / Settings / Install)
  mgs4-dlss <stage id>              boot that scene and exit
  mgs4-dlss --main                  MGS4's own main menu, past the Master Collection screen
  mgs4-dlss --collection            the Master Collection launcher
  mgs4-dlss --list [text]           every launchable scene (filtered by id / name / act)
  mgs4-dlss --setup                 the window, opened on Setup: the game folder and the install check
                                        (the first run opens there anyway; later ones open on Play)
  mgs4-dlss --report                the install check as text, for pasting into an issue
  mgs4-dlss <id> --shortcut <file>  save that scene, with the run options given, as a .lnk
  mgs4-dlss --settings              print mgs4_dlss.ini the way the window shows it
  mgs4-dlss --set Key=Value [...]   write those keys into mgs4_dlss.ini
  mgs4-dlss --stop                  close a running game

Run options (any of them keeps this attached until the scene is done):
  --advance / --no-advance   press through the auto-save notice and "press any button" until the
                             first 3D frame. On by default.
  --mash-x                   keep tapping Cross for the whole scene, so the flashback prompts in
                             cutscenes fire. Wants ViGEmBus + ViGEmClient.dll; falls back to Enter.
  --end-on-gameplay          close the game when the cutscene hands over to gameplay (HUD up).
  --hold <seconds>           close the game that many seconds into the scene.
  --max-minutes <n>          ceiling on the whole run.
  --min-seconds <n>          ignore any "it ended" signal before this (default 30).
  --hud-min <n>              HUD draws in one heartbeat that count as gameplay (default 40).
  --keys "5,ENTER,4,ENTER"   send that sequence to the game instead of pressing through the prompts:
                             key names (E, SPACE, ENTER, ESC, TAB, arrows, F1..) and numbers = seconds
                             to wait. For driving menus.
  --settle <seconds>         wait that long after the window appears before --keys (default 2).
  --press-key <name>         tap that keyboard key in the advance loop instead of the pad's Cross.
  --no-scene-detect          keep pressing for the whole --start-timeout instead of stopping at the
                             first 3D frame.
  --res <W>x<H>              render resolution on the command line (--res_width / --res_height).
  --windowing <mode>         windowed | full_borderless (the port often ignores this).
  --no-restart               drive the game that is already running instead of restarting it.
  --keep-running             leave the game up when the scene ends.
  --game-dir <path>          an install the Steam library search does not find.
  --quiet                    no console output.

Escape, held anywhere, stops an attached run.
'@

# ---------------------------------------------------------------------------------------------- catalogue

# "act2-naomi-in-the-lab" -> "Naomi in the Lab"; the act is already a column of its own.
function Format-SceneName([string]$slug) {
    if (-not $slug) { return "" }
    $parts = @($slug -split '-' | Where-Object { $_ -ne "" })
    if ($parts.Count -gt 1 -and $parts[0] -match '^(act\d|prologue|epilogue|interlude|briefing)$') {
        $parts = $parts[1..($parts.Count - 1)]
    }
    $small = @("the", "a", "an", "and", "of", "in", "at", "on", "with", "to", "by", "versus", "vs")
    $fixed = @{ "pmc" = "PMC"; "pmcs" = "PMCs"; "mk2" = "Mk. II"; "otc" = "OTC"; "rex" = "REX"; "ray" = "RAY"
                "hud" = "HUD"; "ai" = "AI"; "vs" = "vs" }
    $out = @()
    for ($i = 0; $i -lt $parts.Count; $i++) {
        $w = $parts[$i]
        if ($fixed.ContainsKey($w)) { $out += $fixed[$w] }
        elseif ($i -gt 0 -and $small -contains $w) { $out += $w }
        else { $out += ($w.Substring(0, 1).ToUpper() + $w.Substring(1)) }
    }
    return ($out -join " ")
}

function Get-SceneCatalogue {
    if ($script:Catalogue) { return $script:Catalogue }

    $labels = @{}
    if (Test-Mgs4Path $script:LabelsJson) {
        $raw = Get-Content -LiteralPath $script:LabelsJson -Raw -Encoding UTF8 | ConvertFrom-Json
        foreach ($p in $raw.PSObject.Properties) { if ($p.Value) { $labels[$p.Name] = $p.Value } }
    }

    # tools\scene_info.json: what a scene turns out to be once you boot it, which the stage table cannot say.
    $info = @{}
    $acts = [ordered]@{}
    if (Test-Mgs4Path $script:SceneInfoJson) {
        $raw = Get-Content -LiteralPath $script:SceneInfoJson -Raw -Encoding UTF8 | ConvertFrom-Json
        foreach ($a in $raw.acts) { $acts[$a.key] = $a.title }
        foreach ($p in $raw.scenes.PSObject.Properties) { $info[$p.Name] = $p.Value }
    }
    $script:ActTitles = $acts
    $script:ActOrder = @($acts.Keys)

    # Act 1..5 first, then the epilogue, is story order for someone picking a scene; the stage ids do not say that
    # (s00 is the Big Boss material at the very end, s10/s20/s30 are the briefings between acts).
    function ActKeyFor([string]$id) {
        switch -regex ($id) {
            '^s0[1-5]' { return "act" + $id.Substring(2, 1) }
            '^s00'     { return "epilogue" }
            default    { return "other" }
        }
    }

    $list = New-Object System.Collections.Generic.List[object]
    function Add-Entry($props) { $list.Add([pscustomobject]$props) }

    # No "@title" here on purpose: `mgs4.exe --stage s00title_1` access-violates within seconds every time, with
    # the add-on idle (Enabled=0) and frame generation off as well, so it is the port's own crash on a stage id that
    # is a string in the exe rather than something bootable. Verified 2026-08-31; do not put it back untested.
    # Hidden: --skip-to-main-menu lands on the menu selection with the pre-menu credits and PRESS START already
    # gone, which is not how the game starts, and it loses the device on most launches with frame generation on.
    # Kept as a fast way in for testing; s10a10l is the honest "start the game".
    Add-Entry @{ Id = "@main"; Kind = "start"; ActKey = "start"; Rank = 10; Name = "Main menu, skipping the intro"
                 Description = "Drops straight onto the menu selection, past the pre-menu credits and PRESS START. Quick for testing, but it crashes on most launches with frame generation on - use 'Start the game' to play."
                 Hidden = $true; Alts = @(); SortAs = "" }
    Add-Entry @{ Id = "@collection"; Kind = "start"; ActKey = "start"; Rank = 20; Name = "Master Collection launcher"
                 Description = "The Unity front-end, where the display settings live."
                 Hidden = $false; Alts = @(); SortAs = "" }

    # Ids that crash or come up black: every one whose id ends in a single digit - "_0", "_1", ... "_9". The
    # two-digit sections ("_00", "_11") are the ones that work, and "_D2" is a cutscene, not a section: the digit
    # has to be the whole suffix after the underscore for this to fire.
    $brokenRe = '_\d$'

    $aliasOf = @{}
    if (Test-Mgs4Path $script:ScenesCsv) {
        foreach ($row in (Import-Csv -LiteralPath $script:ScenesCsv)) {
            $id = $row.stage_entry
            $o = $info[$id]
            if ($o -and $o.sameAs) { $aliasOf[$id] = $o.sameAs; continue }

            $kind = $row.kind
            if ($o -and $o.kind) { $kind = $o.kind }
            $name = Format-SceneName $labels[$id]
            if ($o -and $o.name) { $name = $o.name }
            $act = ActKeyFor $id
            if ($o -and $o.act) { $act = $o.act }
            $rank = 0
            if ($o -and $null -ne $o.rank) { $rank = [int]$o.rank }
            $desc = ""
            if ($o -and $o.description) { $desc = $o.description }
            $hidden = ($id -match $brokenRe)
            if ($o -and $null -ne $o.hidden) { $hidden = [bool]$o.hidden }
            $sortAs = ""
            if ($o -and $o.sortAs) { $sortAs = $o.sortAs }

            Add-Entry @{ Id = $id; Kind = $kind; ActKey = $act; Rank = $rank; Name = $name
                         Description = $desc; Hidden = $hidden; Alts = @(); SortAs = $sortAs }
        }
    }

    # Fold the "same scene, other id" entries into the one they duplicate, so the list has one row and the panel
    # offers the choice. If the primary is missing, the alias stands on its own rather than disappearing.
    foreach ($id in $aliasOf.Keys) {
        $primary = $list | Where-Object { $_.Id -eq $aliasOf[$id] } | Select-Object -First 1
        if ($primary) { $primary.Alts = @($primary.Alts) + $id }
        else { Add-Entry @{ Id = $id; Kind = "cutscene"; ActKey = (ActKeyFor $id); Rank = 0
                            Name = ""; Description = ""; Hidden = $false; Alts = @(); SortAs = "" } }
    }

    foreach ($e in $list) {
        $e | Add-Member -NotePropertyName ActTitle -NotePropertyValue $(
            if ($acts[$e.ActKey]) { $acts[$e.ActKey] } else { "Uncategorised" }) -Force
        $e | Add-Member -NotePropertyName Note -NotePropertyValue $(
            switch ($e.Kind) {
                "cutscene"   { "in-engine cutscene" }
                "briefing"   { "mission briefing" }
                "gameplay"   { "playable section" }
                "start"      { "starts the game" }
                default      { "boots the stage at its start" }
            }) -Force
    }

    # Story order inside an act: the stage first, then its cutscenes, then its numbered sections. Sorting on the id
    # alone puts "_00" before "_D1" because a digit sorts before a letter, which is backwards - the demo of a stage
    # plays before the gameplay it introduces.
    foreach ($e in $list) {
        $prefix = $e.Id; $cat = 0; $num = 0
        if ($e.Id -match '^([^_]+)_(.*)$') {
            $prefix = $Matches[1]
            $suffix = $Matches[2]
            if ($suffix -match '^D(\d*)$') { $cat = 1; $num = $(if ($Matches[1]) { [int]$Matches[1] } else { -1 }) }
            else { $cat = 2 }
        }
        if ($e.SortAs) { $prefix = $e.SortAs; $cat = 0; $num = 0 }
        $e | Add-Member -NotePropertyName SortPrefix -NotePropertyValue $prefix -Force
        $e | Add-Member -NotePropertyName SortCat -NotePropertyValue $cat -Force
        $e | Add-Member -NotePropertyName SortNum -NotePropertyValue $num -Force
    }
    $order = @{}
    for ($i = 0; $i -lt $script:ActOrder.Count; $i++) { $order[$script:ActOrder[$i]] = $i }
    $script:Catalogue = @($list | Sort-Object @{ Expression = { $order[$_.ActKey] } },
                                              @{ Expression = { $_.Rank } },
                                              @{ Expression = { $_.SortPrefix } },
                                              @{ Expression = { $_.SortCat } },
                                              @{ Expression = { $_.SortNum } },
                                              @{ Expression = { $_.Id } })
    return $script:Catalogue
}

# Entries that start the game rather than a scene: the run options do not apply to them.
function Test-StartEntry([string]$id) {
    if ($id -eq "" -or $id.StartsWith("@")) { return $true }
    $s = Find-Scene $id
    return ($s -and $s.Kind -eq "start")
}

# By id, including the alternate ids folded into an entry.
function Find-Scene([string]$id) {
    if (-not $id) { return $null }
    $all = Get-SceneCatalogue
    $hit = $all | Where-Object { $_.Id -eq $id } | Select-Object -First 1
    if ($hit) { return $hit }
    return ($all | Where-Object { $_.Alts -contains $id } | Select-Object -First 1)
}

# ---------------------------------------------------------------------------------------------- the ini

$script:IniSpec = @(
    @{ Group = "DLSS"; Key = "Enabled"; Type = "bool"; Label = "DLSS on"
       Help = "read again every second; 0 leaves the add-on loaded but idle" }
    @{ Group = "DLSS"; Key = "Mode"; Type = "choice"; Label = "Mode"
       Choices = @("DLAA", "Quality", "Balanced", "Performance", "UltraPerformance")
       Help = "DLAA renders at your resolution; the others render smaller and upscale. Restart the game to change." }
    @{ Group = "DLSS"; Key = "InternalRes"; Type = "readonly"; Label = "Internal resolution"
       Help = "what the game renders at - detected and written by the add-on itself" }
    @{ Group = "DLSS"; Key = "Preset"; Type = "choice"; Label = "Preset"; Choices = @("10", "11")
       ChoiceLabels = @("10 - preset J", "11 - preset K (transformer)")
       Help = "the DLSS model preset" }
    @{ Group = "DLSS"; Key = "Sharpness"; Type = "int"; Label = "Sharpness"; Help = "0..100, applied live" }
    @{ Group = "DLSS"; Key = "PrePost"; Type = "choice"; Label = "Insertion point"
       Choices = @("auto", "0", "1"); ChoiceLabels = @("auto", "before the post chain", "on the final image")
       Help = "auto = the final image when the DLSS 5 NR add-on is loaded, otherwise before post and the HUD" }

    @{ Group = "Frame generation"; Key = "FrameGen"; Type = "choice"; Label = "Frame generation"
       Choices = @("0", "1", "2", "3", "4"); ChoiceLabels = @("off", "2x", "3x", "4x", "dynamic to the target fps")
       Help = "needs the Streamline runtime next to mgs4.exe and a display faster than 60 Hz. Restart to change." }
    @{ Group = "Frame generation"; Key = "FGTargetFps"; Type = "int"; Label = "Target fps"
       Help = "dynamic mode aims here; match your refresh rate (0 = ask the monitor)" }
    @{ Group = "Frame generation"; Key = "Reflex"; Type = "bool"; Label = "Reflex"
       Help = "latency pacing; DLSS-G wants it on" }

    @{ Group = "Image"; Key = "PostDof"; Type = "bool"; Label = "Depth of field after DLSS"
       Help = "skip the game's DoF draws and re-apply the same DoF on the DLSS / NR output" }
    @{ Group = "Image"; Key = "ObjectMV"; Type = "bool"; Label = "Per-object motion vectors"
       Help = "stream-out of the game's vertex shaders; about 0.1 ms of GPU at 4K" }
    @{ Group = "Image"; Key = "Jitter"; Type = "bool"; Label = "Camera jitter"
       Help = "Halton jitter patched into the scene draw constants - what makes this real DLSS" }
    @{ Group = "Image"; Key = "MotionVectors"; Type = "bool"; Label = "Camera motion vectors"
       Help = "reconstructed from depth in a compute pass" }
    @{ Group = "Image"; Key = "DRS"; Type = "bool"; Label = "Dynamic resolution handling"
       Help = "the port shrinks its own scene under load; this brings it back to the full grid" }
    @{ Group = "Image"; Key = "UIMask"; Type = "bool"; Label = "HUD mask"
       Help = "no HUD ghosting under camera motion" }
    @{ Group = "Image"; Key = "FrozenBackground"; Type = "bool"; Label = "Pause / Codec background"
       Help = "keep the DLSS image behind the pause menu and the Codec" }
    @{ Group = "Image"; Key = "WindowScene"; Type = "bool"; Label = "3D windows"
       Help = "DLSS inside the Codec caller's own render target" }
    @{ Group = "Image"; Key = "PreWarm"; Type = "bool"; Label = "Pre-warm"
       Help = "build the DLSS / NR features on loading screens, so the stall is not in the first cutscene frames" }

    @{ Group = "Diagnostics"; Key = "DebugMode"; Type = "choice"; Label = "Debug view"
       Choices = @("0", "1", "2", "3", "4", "5", "9")
       ChoiceLabels = @("off", "magenta path test", "bypass DLSS (A/B)", "trace 3 frames", "draw constants",
                        "motion-vector field", "vectors over the image")
       Help = "costs frames; 0 for normal play" }
    @{ Group = "Diagnostics"; Key = "SceneLog"; Type = "bool"; Label = "Scene-state log"
       Help = "the SCENE-STATE lines this launcher reads to tell a cutscene from gameplay - leave it on" }
    @{ Group = "Diagnostics"; Key = "TraceFrames"; Type = "int"; Label = "Trace frames"
       Help = "N = log every full-frame draw for the next N frames" }
    @{ Group = "Diagnostics"; Key = "TraceFreeze"; Type = "bool"; Label = "Trace freezes"
       Help = "log the draw chain around the moment the world stops rendering" }
    @{ Group = "Diagnostics"; Key = "Probe"; Type = "bool"; Label = "Pipeline probe"
       Help = "sample the image before and after the insertion" }
    @{ Group = "Diagnostics"; Key = "DumpShaders"; Type = "bool"; Label = "Dump shaders"
       Help = "write every pipeline's bytecode to logs\shaders" }
)

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

function Test-GameRunning { return [bool](Get-Process mgs4 -ErrorAction SilentlyContinue) }

# Where the game folder came from, for the Setup tab to say so.
function Get-GameDirSource {
    if ([Environment]::GetEnvironmentVariable("MGS4_DIR")) { return "from the MGS4_DIR environment variable" }
    if ((Get-Mgs4ConfigValues)["MGS4_DIR"]) { return "from config.ini" }
    return "found in the Steam libraries"
}

# The one place the app writes it. config.ini is git-ignored and is what every script in the repo already asks.
function Set-ConfiguredGameDir([string]$dir) {
    $path = $Mgs4Config
    $lines = @()
    if (Test-Mgs4Path $path) { $lines = @(Get-Content -LiteralPath $path) }
    $done = $false
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match '^\s*;?\s*MGS4_DIR\s*=\s*(.*)$') {   # the shipped example is commented out; take it over
            $lines[$i] = $(if ($dir) { "MGS4_DIR=$dir" } else { ";MGS4_DIR=" + $Matches[1] })
            $done = $true
            break
        }
    }
    if (-not $done -and $dir) {
        if ($lines.Count -eq 0) {
            $lines = @("; Machine-local paths for this checkout (git-ignored). See config.example.ini for every key.")
        }
        $lines += "MGS4_DIR=$dir"
    }
    Set-Content -LiteralPath $path -Value $lines -Encoding UTF8
    return $path
}

# ---------------------------------------------------------------------------------------------- input

# keybd_event with a real scan code is what this port's input path reacts to; scan-code-only SendInput events were
# ignored by it. Keys only reach the foreground window, so the game is pushed there first (Alt tap + AttachThreadInput
# is the sequence Windows wants before it allows a foreground change).
Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices;
public static class Mgs4Win {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr pid);
  [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint mapType);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern short GetAsyncKeyState(int vk);
  [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
  public static void Key(ushort vk, bool down) {
    keybd_event((byte)vk, (byte)MapVirtualKey(vk, 0), (uint)(down ? 0 : 2), UIntPtr.Zero);
  }
  public static bool Focus(IntPtr h) {
    if (GetForegroundWindow() == h) return true;
    keybd_event(0x12, 0, 0, UIntPtr.Zero); keybd_event(0x12, 0, 2, UIntPtr.Zero);
    uint fg = GetWindowThreadProcessId(GetForegroundWindow(), IntPtr.Zero), me = GetCurrentThreadId();
    if (fg != me) AttachThreadInput(me, fg, true);
    ShowWindow(h, 9); BringWindowToTop(h); SetForegroundWindow(h);
    if (fg != me) AttachThreadInput(me, fg, false);
    System.Threading.Thread.Sleep(250);
    return GetForegroundWindow() == h;
  }
  public static bool EscapeDown() { return (GetAsyncKeyState(0x1B) & 0x8000) != 0; }
}
"@ -ErrorAction SilentlyContinue

# A virtual DualShock 4 through ViGEmBus. Cross is the button MGS4's flashback prompts want; a keyboard Enter gets
# past the boot prompts but does not fire them. The DLL is loaded by full path first, so the DllImport below binds to
# the module that is already in the process whatever folder it came from.
Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices;
public static class Mgs4Pad {
  [StructLayout(LayoutKind.Sequential)] public struct DS4Report {
    public byte lx, ly, rx, ry; public ushort buttons; public byte special, tl, tr;
  }
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode)] static extern IntPtr LoadLibraryW(string path);
  [DllImport("ViGEmClient.dll")] static extern IntPtr vigem_alloc();
  [DllImport("ViGEmClient.dll")] static extern int vigem_connect(IntPtr client);
  [DllImport("ViGEmClient.dll")] static extern void vigem_disconnect(IntPtr client);
  [DllImport("ViGEmClient.dll")] static extern void vigem_free(IntPtr client);
  [DllImport("ViGEmClient.dll")] static extern IntPtr vigem_target_ds4_alloc();
  [DllImport("ViGEmClient.dll")] static extern int vigem_target_add(IntPtr client, IntPtr target);
  [DllImport("ViGEmClient.dll")] static extern int vigem_target_remove(IntPtr client, IntPtr target);
  [DllImport("ViGEmClient.dll")] static extern void vigem_target_free(IntPtr target);
  [DllImport("ViGEmClient.dll")] static extern int vigem_target_ds4_update(IntPtr client, IntPtr target, DS4Report r);

  const int VIGEM_ERROR_NONE = 0x20000000;
  const ushort DPAD_NONE = 0x8, CROSS = 1 << 5;
  static IntPtr client = IntPtr.Zero, pad = IntPtr.Zero;
  public static string Error = "";

  public static bool Open(string dllPath) {
    if (pad != IntPtr.Zero) return true;
    try {
      if (!string.IsNullOrEmpty(dllPath) && LoadLibraryW(dllPath) == IntPtr.Zero) {
        Error = "ViGEmClient.dll could not be loaded from " + dllPath; return false;
      }
      client = vigem_alloc();
      int r = vigem_connect(client);
      if (r != VIGEM_ERROR_NONE) { Error = "ViGEmBus is not running (vigem_connect 0x" + r.ToString("X8") + ")"; Close(); return false; }
      pad = vigem_target_ds4_alloc();
      r = vigem_target_add(client, pad);
      if (r != VIGEM_ERROR_NONE) { Error = "vigem_target_add 0x" + r.ToString("X8"); Close(); return false; }
      Send(0);
      return true;
    } catch (Exception e) { Error = e.Message; Close(); return false; }
  }
  static void Send(ushort buttons) {
    DS4Report rep = new DS4Report();
    rep.lx = rep.ly = rep.rx = rep.ry = 128;
    rep.buttons = (ushort)(DPAD_NONE | buttons);
    vigem_target_ds4_update(client, pad, rep);
  }
  public static void TapCross(int holdMs) {
    if (pad == IntPtr.Zero) return;
    Send(CROSS); System.Threading.Thread.Sleep(holdMs); Send(0);
  }
  public static void Close() {
    try {
      if (pad != IntPtr.Zero) { vigem_target_remove(client, pad); vigem_target_free(pad); }
      if (client != IntPtr.Zero) { vigem_disconnect(client); vigem_free(client); }
    } catch {}
    pad = IntPtr.Zero; client = IntPtr.Zero;
  }
}
"@ -ErrorAction SilentlyContinue

# tools\ViGEmClient.dll, VIGEM_CLIENT_DLL, or the copy the vgamepad package installs.
function Find-VigemDll {
    $cands = @($env:VIGEM_CLIENT_DLL, (Join-Path $PSScriptRoot "ViGEmClient.dll"),
               (Join-Path (Split-Path -Parent $PSScriptRoot) "ViGEmClient.dll"))
    foreach ($c in $cands) { if ($c -and (Test-Mgs4Path $c)) { return $c } }
    return $null
}

function Open-Pad {
    $dll = Find-VigemDll
    if (-not $dll) { return @{ Ok = $false; Why = "ViGEmClient.dll not found (put it in tools\, or set VIGEM_CLIENT_DLL)" } }
    if ([Mgs4Pad]::Open($dll)) { return @{ Ok = $true; Why = "virtual DualShock 4 on $([IO.Path]::GetFileName($dll))" } }
    return @{ Ok = $false; Why = [Mgs4Pad]::Error }
}

# ---------------------------------------------------------------------------------------------- running a scene

function Get-GameWindow {
    $p = Get-Process mgs4 -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    if ($p) { return $p.MainWindowHandle }
    return [IntPtr]::Zero
}

function Stop-Game {
    Stop-Process -Name mgs4, launcher -Force -ErrorAction SilentlyContinue
    Start-Sleep 3
}

# --skip-to-main-menu is what a plain mgs4.exe used to do: without it the port stops on the Master Collection
# screen first. --stage <id> goes past both, straight into a scene (steam_appid.txt next to the exe keeps Steam
# from relaunching the game and losing the argument).
function Get-LaunchCommand($opt, $gameDir) {
    $exe = Join-Mgs4Path $gameDir "mgs4.exe"
    $cli = @()
    if ($opt.Stage -eq "@collection") {
        return @{ Exe = (Join-Mgs4Path (Split-Path -Parent $gameDir) "Launcher\launcher.exe"); Args = @() }
    }
    if ($opt.Stage -eq "@main" -or $opt.Stage -eq "") { $cli += "--skip-to-main-menu" }
    else { $cli += @("--stage", $opt.Stage) }
    if ($opt.Width -gt 0 -and $opt.Height -gt 0) { $cli += @("--res_width", "$($opt.Width)", "--res_height", "$($opt.Height)") }
    if ($opt.Windowing) { $cli += @("--windowing", $opt.Windowing) }
    return @{ Exe = $exe; Args = $cli }
}

# Only the lines the add-on wrote since we started reading. The log is replaced at every launch, so a shrink resets.
function New-LogTail([string]$path) {
    $pos = 0
    if (Test-Mgs4Path $path) { $pos = (Get-Item -LiteralPath $path).Length }
    return [pscustomobject]@{ Path = $path; Pos = $pos }
}

function Read-LogTail($tail) {
    if (-not (Test-Mgs4Path $tail.Path)) { return @() }
    $len = (Get-Item -LiteralPath $tail.Path).Length
    if ($len -lt $tail.Pos) { $tail.Pos = 0 }
    if ($len -eq $tail.Pos) { return @() }
    $fs = [IO.File]::Open($tail.Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    try {
        [void]$fs.Seek($tail.Pos, [IO.SeekOrigin]::Begin)
        $buf = New-Object byte[] ($len - $tail.Pos)
        $read = $fs.Read($buf, 0, $buf.Length)
        $tail.Pos += $read
        return ([Text.Encoding]::UTF8.GetString($buf, 0, $read) -split "`r?`n")
    } finally { $fs.Dispose() }
}

# The add-on's scene classifier, straight out of the log:
#   SCENE-STATE <state> (...)            on every change
#   SCENE-STATE-TICK <state> (frame N, scene draws X, HUD draws Y, ...)   heartbeat, about every 10 s
# "NGX EvaluateFeature ok" is the fallback for a build with SceneLog off: it only happens on a 3D frame.
function Read-SceneSignals($tail, [ref]$state, [ref]$hudDraws) {
    $hudDraws.Value = -1        # only a heartbeat seen in *this* read counts: a HUD flash at the start of a
                                # cutscene must not still be sitting there when the minimum time runs out
    foreach ($line in (Read-LogTail $tail)) {
        if ($line -match 'SCENE-STATE-TICK (cutscene|gameplay|no-3d) \(frame \d+, scene draws \d+, HUD draws (\d+)') {
            $state.Value = $Matches[1]; $hudDraws.Value = [int]$Matches[2]
        } elseif ($line -match 'SCENE-STATE (cutscene|gameplay|no-3d) ') {
            $state.Value = $Matches[1]
        } elseif ($line -match 'NGX EvaluateFeature ok') {
            # a build with SceneLog off still says this, and it only happens on a 3D frame
            if ($state.Value -eq "unknown") { $state.Value = "cutscene" }
        }
    }
}

# Key names as tools\launch_stage.ps1 spelled them, so its sequences still work.
function Get-VirtualKey([string]$name) {
    switch ($name.ToUpper()) {
        "SPACE" { return 0x20 } "ENTER" { return 0x0D } "ESC" { return 0x1B } "TAB" { return 0x09 }
        "UP" { return 0x26 } "DOWN" { return 0x28 } "LEFT" { return 0x25 } "RIGHT" { return 0x27 }
        "BACK" { return 0x08 } "DELETE" { return 0x2E }
    }
    if ($name -match '^F(\d+)$') { return 0x6F + [int]$Matches[1] }
    if ($name.Length -eq 1) { return [int][char]$name.ToUpper() }
    throw "unknown key $name"
}

# "5,ENTER,4,ENTER": a number is a wait in seconds, anything else a key press.
function Send-KeySequence([IntPtr]$hwnd, [string]$keys, [scriptblock]$Say) {
    foreach ($tok in $keys.Split(',')) {
        $t = $tok.Trim()
        if ($t -eq "") { continue }
        if ($t -match '^\d+(\.\d+)?$') { & $Say "wait $t s"; Start-Sleep ([double]$t); continue }
        $vk = [uint16](Get-VirtualKey $t)
        $ok = $false
        for ($a = 0; $a -lt 5 -and -not $ok; $a++) { $ok = [Mgs4Win]::Focus($hwnd); if (-not $ok) { Start-Sleep -Milliseconds 400 } }
        & $Say ("press {0} (foreground {1})" -f $t, $ok)
        [Mgs4Win]::Key($vk, $true); Start-Sleep -Milliseconds 300; [Mgs4Win]::Key($vk, $false)
    }
}

function Invoke-SceneRun($opt, [scriptblock]$Say) {
    $gameDir = $opt.GameDir
    if (-not $gameDir) { $gameDir = Get-Mgs4GameDir }
    $log = Join-Mgs4Path $gameDir "logs\launcher.log"
    New-Item -ItemType Directory -Force (Split-Path -Parent $log) | Out-Null
    if (-not $Say) {
        $Say = {
            param($m)
            $line = "[{0:HH:mm:ss.fff}] {1}" -f (Get-Date), $m
            if (-not $opt.Quiet) { Write-Host $line }
            Add-Content -LiteralPath $log -Value $line -Encoding ASCII
        }.GetNewClosure()
    }

    $addonLog = Join-Mgs4Path $gameDir "logs\mgs4_dlss.log"
    $tail = New-LogTail $addonLog
    $cmd = Get-LaunchCommand $opt $gameDir
    if (-not (Test-Mgs4Path $cmd.Exe)) { & $Say "not found: $($cmd.Exe)"; return 1 }

    if ($opt.Restart) {
        Stop-Game
        $tail = New-LogTail $addonLog          # the add-on truncates its log at startup; read from the new one
        $wd = Split-Path -Parent $cmd.Exe
        if ($cmd.Args.Count) { Start-Process -FilePath $cmd.Exe -ArgumentList $cmd.Args -WorkingDirectory $wd | Out-Null }
        else { Start-Process -FilePath $cmd.Exe -WorkingDirectory $wd | Out-Null }
        & $Say ("launched " + [IO.Path]::GetFileName($cmd.Exe) + " " + ($cmd.Args -join " "))
    } else {
        & $Say "attaching to the running game"
    }
    if ($opt.Stage -eq "@collection") { return 0 }

    # A menu is not a scene: there are no boot prompts to press through, no first 3D frame to wait for, and tapping
    # Cross on the main menu just starts a new game. Only an explicit key sequence makes sense here.
    if ((Test-StartEntry $opt.Stage) -and -not $opt.Keys) {
        if ($opt.Advance -or $opt.MashX -or $opt.EndOnGameplay) { & $Say "menu entry: leaving the game alone (the run options are for scenes)" }
        return 0
    }

    $attached = $opt.Advance -or $opt.MashX -or $opt.EndOnGameplay -or $opt.Keys -or $opt.Hold -gt 0 -or $opt.MaxMinutes -gt 0
    if (-not $attached) { return 0 }

    $hwnd = [IntPtr]::Zero
    for ($i = 0; $i -lt 120 -and $hwnd -eq [IntPtr]::Zero; $i++) {
        $hwnd = Get-GameWindow
        if ($hwnd -eq [IntPtr]::Zero) { Start-Sleep 1 }
    }
    if ($hwnd -eq [IntPtr]::Zero) { & $Say "no game window appeared"; return 1 }

    $pad = @{ Ok = $false; Why = "not needed" }
    if ((($opt.Advance -and -not $opt.Keys) -or $opt.MashX) -and -not $opt.PressKey) {
        $pad = Open-Pad
        if ($pad.Ok) { & $Say $pad.Why }
        elseif ($opt.MashX) { & $Say "no controller: $($pad.Why) - falling back to Enter, flashback prompts will not fire" }
    }
    $vk = [uint16](Get-VirtualKey $(if ($opt.PressKey) { $opt.PressKey } else { "ENTER" }))
    $usePad = $pad.Ok -and -not $opt.PressKey
    $press = {
        if (-not [Mgs4Win]::Focus($hwnd)) { [void][Mgs4Win]::Focus($hwnd) }
        if ($usePad) {
            [Mgs4Pad]::TapCross([int]($opt.PressHold * 1000))
        } else {
            [Mgs4Win]::Key($vk, $true); Start-Sleep -Milliseconds ([int]($opt.PressHold * 1000)); [Mgs4Win]::Key($vk, $false)
        }
    }.GetNewClosure()

    if ($opt.Keys) {
        if ($opt.Settle -gt 0) { & $Say "window up, settling $($opt.Settle)s"; Start-Sleep $opt.Settle }
        Send-KeySequence $hwnd $opt.Keys $Say
    }

    $state = "unknown"; $hud = -1
    $t0 = Get-Date
    $deadline = { param($secs) (Get-Date) -gt $t0.AddSeconds($secs) }

    # Phase 1 - through the auto-save notice, the "press any button" screen and the load, to the first 3D frame.
    if ($opt.Advance -and -not $opt.Keys) {
        & $Say "pressing through the boot prompts (up to $($opt.StartTimeout)s)"
        $n = 0
        while (-not (& $deadline $opt.StartTimeout)) {
            if ([Mgs4Win]::EscapeDown()) { & $Say "Escape - stopping"; [Mgs4Pad]::Close(); return 130 }
            Read-SceneSignals $tail ([ref]$state) ([ref]$hud)
            if ($opt.SceneDetect -and ($state -eq "cutscene" -or $state -eq "gameplay")) { break }
            if ((Get-GameWindow) -eq [IntPtr]::Zero) { & $Say "the game exited"; [Mgs4Pad]::Close(); return 1 }
            & $press; $n++
            Start-Sleep -Milliseconds ([int]($opt.PressEvery * 1000))
        }
        if ($state -eq "cutscene" -or $state -eq "gameplay") { & $Say "scene running ($state) after $n presses" }
        else { & $Say "no 3D frame within $($opt.StartTimeout)s (state $state)" }
    }

    if (-not ($opt.MashX -or $opt.EndOnGameplay -or $opt.Hold -gt 0 -or $opt.MaxMinutes -gt 0)) {
        [Mgs4Pad]::Close(); return 0
    }

    # Phase 2 - stay with the scene: keep Cross going for the flashbacks, and watch for the hand-over to gameplay.
    $tScene = Get-Date
    $gameplaySince = $null; $staticSince = $null
    $reason = "still running"
    while ($true) {
        if ([Mgs4Win]::EscapeDown()) { $reason = "Escape"; break }
        if ((Get-GameWindow) -eq [IntPtr]::Zero) { $reason = "the game exited"; break }
        $inScene = ((Get-Date) - $tScene).TotalSeconds
        if ($opt.Hold -gt 0 -and $inScene -ge $opt.Hold) { $reason = "held $($opt.Hold)s"; break }
        if ($opt.MaxMinutes -gt 0 -and ((Get-Date) - $t0).TotalMinutes -ge $opt.MaxMinutes) { $reason = "max-minutes"; break }

        if ($opt.MashX) { & $press }
        Start-Sleep -Milliseconds ([int]($opt.PressEvery * 1000))

        $prev = $state
        Read-SceneSignals $tail ([ref]$state) ([ref]$hud)
        if ($state -ne $prev) { & $Say ("state -> {0} at {1:n0}s" -f $state, $inScene) }

        if ($opt.EndOnGameplay -and $inScene -gt $opt.MinSeconds) {
            if ($hud -ge $opt.HudMin) { $reason = "HUD up ($hud draws)"; break }
            if ($state -eq "gameplay") {
                if (-not $gameplaySince) { $gameplaySince = Get-Date }
                elseif (((Get-Date) - $gameplaySince).TotalSeconds -ge $opt.GameplayGrace) { $reason = "gameplay"; break }
            } else { $gameplaySince = $null }
            if ($state -eq "no-3d") {
                if (-not $staticSince) { $staticSince = Get-Date }
                elseif (((Get-Date) - $staticSince).TotalSeconds -ge $opt.StaticGrace) { $reason = "the scene ended (loading / continue screen)"; break }
            } else { $staticSince = $null }
        }
    }
    [Mgs4Pad]::Close()
    & $Say ("done after {0:n0}s: {1}" -f ((Get-Date) - $tScene).TotalSeconds, $reason)
    if (-not $opt.KeepRunning -and $reason -ne "still running" -and $reason -ne "the game exited") {
        if ($opt.EndOnGameplay -or $opt.Hold -gt 0 -or $opt.MaxMinutes -gt 0) { & $Say "closing the game"; Stop-Game }
    }
    return 0
}

# ---------------------------------------------------------------------------------------------- text actions

function Write-SceneList($filter) {
    $rows = Get-SceneCatalogue
    if ($filter) {
        $rows = $rows | Where-Object {
            "$($_.Id) $($_.Alts) $($_.Name) $($_.ActTitle) $($_.Kind) $($_.Description)" -match [regex]::Escape($filter)
        }
    } else {
        $rows = $rows | Where-Object { -not $_.Hidden }     # the ones that crash or come up black
    }
    $n = 0
    $act = ""
    foreach ($r in $rows) {
        if ($r.ActTitle -ne $act) { $act = $r.ActTitle; Write-Host ""; Write-Host "[$act]" }
        $ids = $r.Id
        if ($r.Alts.Count) { $ids += " (= " + ($r.Alts -join ", ") + ")" }
        $what = $(if ($r.Description) { $r.Description } else { $r.Note })
        Write-Host ("  {0,-30} {1,-10} {2}" -f $ids, $r.Kind, $(if ($r.Name) { "$($r.Name) - $what" } else { $what }))
        $n++
    }
    Write-Host ""
    Write-Host ("{0} {1}.  mgs4-dlss <id>  boots one." -f $n, $(if ($n -eq 1) { "entry" } else { "entries" }))
}

function Write-SettingsReport($gameDir) {
    Write-Host "mgs4_dlss.ini: $ini"
    if (-not (Test-Mgs4Path $ini)) { Write-Host "  (not there - copy dlss-addon\mgs4_dlss.ini next to mgs4.exe)"; return }
    $group = ""
    foreach ($s in $script:IniSpec) {
        if ($s.Group -ne $group) { $group = $s.Group; Write-Host ""; Write-Host "[$group]" }
        $v = Get-IniValue (Join-Mgs4Path $state.GameDir "mgs4_dlss.ini") $s.Key
        if ($null -eq $v) { $v = "(unset)" }
        Write-Host ("  {0,-22} {1,-12} {2}" -f $s.Key, $v, $s.Label)
    }
    if (Test-GameRunning) {
        Write-Host ""
        Write-Host "The game is running: it owns this file, so leave the writing to the add-on until it exits."
    }
}

function Set-SettingsFromCli($gameDir, $sets) {
    if (Test-GameRunning) {
        Write-Host "mgs4.exe is running - it rewrites mgs4_dlss.ini through the profile API and would undo this. Close it first." -ForegroundColor Yellow
        return 1
    }
    $vals = @{}
    foreach ($s in $sets) {
        if ($s -notmatch '^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*)$') { Write-Host "not a Key=Value: $s" -ForegroundColor Red; return 1 }
        $vals[$Matches[1]] = $Matches[2].Trim()
    }
    Set-Ini $ini $vals
    foreach ($k in $vals.Keys) { Write-Host ("{0}={1}" -f $k, $vals[$k]) }
    Write-Host "written to $ini"
    return 0
}

# ---------------------------------------------------------------------------------------------- shortcuts

# A sensible default file name for a scene's shortcut: "<id> - <act> - <name>", minus what Windows will not take.
function Get-ShortcutName($scene) {
    $name = $scene.Id
    if ($scene.ActTitle -and $scene.Kind -ne "start") { $name += " - " + $scene.ActTitle }
    if ($scene.Name) { $name += " - " + $scene.Name }
    foreach ($c in [IO.Path]::GetInvalidFileNameChars()) { $name = $name.Replace($c, '-') }
    return $name
}

# One .lnk for one scene, wherever the caller wants it, carrying the run options it was made with. The shortcut
# holds this script's absolute path, so moving the checkout breaks it - make a new one rather than editing it.
function New-SceneShortcut($opt, [string]$path) {
    if (-not $opt.Stage) { throw "no scene to make a shortcut for" }
    if (-not $path) { throw "no file name for the shortcut" }
    if (-not $path.ToLower().EndsWith(".lnk")) { $path += ".lnk" }
    $dir = Split-Path -Parent $path
    if ($dir -and -not (Test-Mgs4Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }

    $scriptPath = Join-Path $PSScriptRoot "mgs4_dlss.ps1"
    $scene = Find-Scene $opt.Stage
    $cli = @(Get-CliArgs $opt | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } })

    $shell = New-Object -ComObject WScript.Shell
    $lnk = $shell.CreateShortcut($path)
    $lnk.TargetPath = Join-Path $env:SystemRoot "System32\WindowsPowerShell\v1.0\powershell.exe"
    $lnk.Arguments = '-NoProfile -ExecutionPolicy Bypass -WindowStyle Minimized -File "' + $scriptPath + '" ' + ($cli -join " ")
    if ($opt.GameDir) {
        $lnk.WorkingDirectory = $opt.GameDir
        $lnk.IconLocation = (Join-Mgs4Path $opt.GameDir "mgs4.exe") + ",0"
    }
    $lnk.Description = $opt.Stage + $(if ($scene -and $scene.Description) { " - " + $scene.Description }
                                     elseif ($scene -and $scene.Note) { " - " + $scene.Note } else { "" })
    $lnk.Save()
    return $path
}

# ---------------------------------------------------------------------------------------------- the window

# The window never runs a scene on its own thread - it starts this same script again with the equivalent command
# line, so the UI stays responsive and every route into the game (window, shortcut, test script) is one code path.
function Get-CliArgs($opt) {
    $a = @()
    if ($opt.Stage -eq "@main") { $a += "--main" }
    elseif ($opt.Stage -eq "@collection") { $a += "--collection" }
    else { $a += $opt.Stage }
    if (-not $opt.Advance) { $a += "--no-advance" }
    if ($opt.MashX) { $a += "--mash-x" }
    if ($opt.EndOnGameplay) { $a += "--end-on-gameplay" }
    if ($opt.Hold -gt 0) { $a += @("--hold", ("{0:0}" -f $opt.Hold)) }
    if ($opt.KeepRunning) { $a += "--keep-running" }
    if ($opt.Width -gt 0 -and $opt.Height -gt 0) { $a += @("--res", "$($opt.Width)x$($opt.Height)") }
    if ($opt.GameDirGiven -and $opt.GameDir) { $a += @("--game-dir", $opt.GameDir) }
    return $a
}

# The same argument list as a line someone can paste into a terminal.
function Format-CliPreview($cliArgs) {
    return "mgs4-dlss " + (($cliArgs | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join " ")
}

function Read-Prefs {
    if (Test-Mgs4Path $script:PrefsPath) {
        try { return (Get-Content -LiteralPath $script:PrefsPath -Raw -Encoding UTF8 | ConvertFrom-Json) } catch {}
    }
    return $null
}

# No preferences file yet, or one from before this marker existed, means nobody has opened the window here: the
# install check is the first thing worth seeing. Every run after that opens on Play.
function Test-FirstRun {
    $p = Read-Prefs
    return -not ($p -and $p.Seen)
}

function Save-Prefs($o) {
    try {
        New-Item -ItemType Directory -Force (Split-Path -Parent $script:PrefsPath) | Out-Null
        $o | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $script:PrefsPath -Encoding UTF8
    } catch {}
}

$script:Xaml = @'
<Window xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
        xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
        Title="MGS4 DLSS" Height="880" Width="1180" MinHeight="560" MinWidth="920"
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
                            <Setter TargetName="t" Property="Background" Value="#465066"/>
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
              <Trigger Property="IsEnabled" Value="False">
                <Setter Property="Opacity" Value="0.45"/>
                <Setter Property="Cursor" Value="Arrow"/>
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
    <Style x:Key="Chip" TargetType="ToggleButton">
      <Setter Property="Foreground" Value="#858D9E"/>
      <Setter Property="FontSize" Value="11"/>
      <Setter Property="Cursor" Value="Hand"/>
      <Setter Property="Margin" Value="0,0,6,6"/>
      <Setter Property="Template">
        <Setter.Value>
          <ControlTemplate TargetType="ToggleButton">
            <Border x:Name="b" CornerRadius="4" Background="#12151D" BorderBrush="#2A3040" BorderThickness="1"
                    Padding="10,4">
              <ContentPresenter VerticalAlignment="Center"/>
            </Border>
            <ControlTemplate.Triggers>
              <Trigger Property="IsMouseOver" Value="True">
                <Setter TargetName="b" Property="BorderBrush" Value="#3E4A66"/>
              </Trigger>
              <Trigger Property="IsChecked" Value="True">
                <Setter TargetName="b" Property="Background" Value="#25335C"/>
                <Setter TargetName="b" Property="BorderBrush" Value="#4E6DE8"/>
                <Setter Property="Foreground" Value="#CBD8FF"/>
              </Trigger>
            </ControlTemplate.Triggers>
          </ControlTemplate>
        </Setter.Value>
      </Setter>
    </Style>
    <Style x:Key="Nav" TargetType="RadioButton">
      <Setter Property="Foreground" Value="#858D9E"/>
      <Setter Property="Cursor" Value="Hand"/>
      <Setter Property="FontSize" Value="13"/>
      <Setter Property="Margin" Value="0,0,6,0"/>
      <Setter Property="Template">
        <Setter.Value>
          <ControlTemplate TargetType="RadioButton">
            <Border x:Name="b" CornerRadius="6" Background="Transparent" Padding="18,7">
              <ContentPresenter VerticalAlignment="Center"/>
            </Border>
            <ControlTemplate.Triggers>
              <Trigger Property="IsMouseOver" Value="True">
                <Setter TargetName="b" Property="Background" Value="#1B2030"/>
              </Trigger>
              <Trigger Property="IsChecked" Value="True">
                <Setter TargetName="b" Property="Background" Value="#222B45"/>
                <Setter Property="Foreground" Value="#E7EAF0"/>
                <Setter Property="FontWeight" Value="SemiBold"/>
              </Trigger>
            </ControlTemplate.Triggers>
          </ControlTemplate>
        </Setter.Value>
      </Setter>
    </Style>
    <!-- No padding on the border: it ate the height the text needed, so a box with a set height clipped whatever was
         typed into it. The content host is inset horizontally and centred vertically instead, which cannot clip. -->
    <Style TargetType="TextBox">
      <Setter Property="Foreground" Value="#E7EAF0"/>
      <Setter Property="CaretBrush" Value="#E7EAF0"/>
      <Setter Property="Background" Value="#12151D"/>
      <Setter Property="BorderBrush" Value="#333A4D"/>
      <Setter Property="FontSize" Value="12"/>
      <Setter Property="MinHeight" Value="30"/>
      <Setter Property="Template">
        <Setter.Value>
          <ControlTemplate TargetType="TextBox">
            <Border CornerRadius="6" Background="{TemplateBinding Background}" BorderBrush="{TemplateBinding BorderBrush}"
                    BorderThickness="1">
              <ScrollViewer x:Name="PART_ContentHost" Margin="9,0" VerticalAlignment="Center"/>
            </Border>
          </ControlTemplate>
        </Setter.Value>
      </Setter>
    </Style>
    <Style TargetType="CheckBox">
      <Setter Property="Foreground" Value="#E7EAF0"/>
      <Setter Property="FontSize" Value="12"/>
      <Setter Property="Cursor" Value="Hand"/>
      <Setter Property="Template">
        <Setter.Value>
          <ControlTemplate TargetType="CheckBox">
            <Grid Background="Transparent">
              <Grid.ColumnDefinitions>
                <ColumnDefinition Width="Auto"/>
                <ColumnDefinition Width="*"/>
              </Grid.ColumnDefinitions>
              <Border x:Name="box" Width="16" Height="16" CornerRadius="4" Background="#12151D" BorderBrush="#3A445C"
                      BorderThickness="1" VerticalAlignment="Center">
                <TextBlock x:Name="tick" Text="&#x2714;" FontSize="10" Foreground="#0F1116" FontWeight="Bold"
                           HorizontalAlignment="Center" VerticalAlignment="Center" Visibility="Collapsed"/>
              </Border>
              <ContentPresenter Grid.Column="1" Margin="9,0,0,0" VerticalAlignment="Center"/>
            </Grid>
            <ControlTemplate.Triggers>
              <Trigger Property="IsChecked" Value="True">
                <Setter TargetName="box" Property="Background" Value="#7C9CFF"/>
                <Setter TargetName="box" Property="BorderBrush" Value="#7C9CFF"/>
                <Setter TargetName="tick" Property="Visibility" Value="Visible"/>
              </Trigger>
              <Trigger Property="IsMouseOver" Value="True">
                <Setter TargetName="box" Property="BorderBrush" Value="#5A6480"/>
              </Trigger>
              <Trigger Property="IsEnabled" Value="False">
                <Setter Property="Opacity" Value="0.45"/>
              </Trigger>
            </ControlTemplate.Triggers>
          </ControlTemplate>
        </Setter.Value>
      </Setter>
    </Style>
    <Style TargetType="ComboBoxItem">
      <Setter Property="Foreground" Value="#E7EAF0"/>
      <Setter Property="FontSize" Value="12"/>
      <Setter Property="Padding" Value="10,6"/>
      <Setter Property="Cursor" Value="Hand"/>
      <Setter Property="Template">
        <Setter.Value>
          <ControlTemplate TargetType="ComboBoxItem">
            <Border x:Name="b" Background="Transparent" Padding="{TemplateBinding Padding}">
              <ContentPresenter/>
            </Border>
            <ControlTemplate.Triggers>
              <Trigger Property="IsHighlighted" Value="True">
                <Setter TargetName="b" Property="Background" Value="#2C3346"/>
              </Trigger>
            </ControlTemplate.Triggers>
          </ControlTemplate>
        </Setter.Value>
      </Setter>
    </Style>
    <Style TargetType="ComboBox">
      <Setter Property="Foreground" Value="#E7EAF0"/>
      <Setter Property="FontSize" Value="12"/>
      <Setter Property="Cursor" Value="Hand"/>
      <Setter Property="Template">
        <Setter.Value>
          <ControlTemplate TargetType="ComboBox">
            <Grid TextBlock.Foreground="#E7EAF0">
              <ToggleButton Focusable="False" ClickMode="Press"
                            IsChecked="{Binding IsDropDownOpen, Mode=TwoWay, RelativeSource={RelativeSource TemplatedParent}}">
                <ToggleButton.Template>
                  <ControlTemplate TargetType="ToggleButton">
                    <Border x:Name="tb" CornerRadius="6" Background="#12151D" BorderBrush="#333A4D" BorderThickness="1">
                      <Path Data="M0,0 L7,0 L3.5,4.5 Z" Fill="#858D9E" HorizontalAlignment="Right"
                            VerticalAlignment="Center" Margin="0,0,10,0"/>
                    </Border>
                    <ControlTemplate.Triggers>
                      <Trigger Property="IsMouseOver" Value="True">
                        <Setter TargetName="tb" Property="BorderBrush" Value="#4A5573"/>
                      </Trigger>
                    </ControlTemplate.Triggers>
                  </ControlTemplate>
                </ToggleButton.Template>
              </ToggleButton>
              <ContentPresenter IsHitTestVisible="False" Content="{TemplateBinding SelectionBoxItem}"
                                ContentTemplate="{TemplateBinding SelectionBoxItemTemplate}"
                                Margin="10,5,26,5" VerticalAlignment="Center"/>
              <Popup IsOpen="{TemplateBinding IsDropDownOpen}" Placement="Bottom" AllowsTransparency="True" Focusable="False">
                <Border Background="#1B1F29" BorderBrush="#333A4D" BorderThickness="1" CornerRadius="6"
                        MinWidth="{Binding ActualWidth, RelativeSource={RelativeSource TemplatedParent}}" Margin="0,2,0,0">
                  <ScrollViewer MaxHeight="280"><ItemsPresenter/></ScrollViewer>
                </Border>
              </Popup>
            </Grid>
          </ControlTemplate>
        </Setter.Value>
      </Setter>
    </Style>
    <Style TargetType="ListBox">
      <Setter Property="Background" Value="Transparent"/>
      <Setter Property="BorderThickness" Value="0"/>
      <Setter Property="ScrollViewer.HorizontalScrollBarVisibility" Value="Disabled"/>
    </Style>
    <Style TargetType="ListBoxItem">
      <Setter Property="Cursor" Value="Hand"/>
      <Setter Property="Template">
        <Setter.Value>
          <ControlTemplate TargetType="ListBoxItem">
            <Border x:Name="b" Background="Transparent" BorderBrush="#1C202B" BorderThickness="0,0,0,1" Padding="14,8">
              <ContentPresenter/>
            </Border>
            <ControlTemplate.Triggers>
              <Trigger Property="IsMouseOver" Value="True">
                <Setter TargetName="b" Property="Background" Value="#1A1E28"/>
              </Trigger>
              <Trigger Property="IsSelected" Value="True">
                <Setter TargetName="b" Property="Background" Value="#232C48"/>
                <Setter TargetName="b" Property="BorderBrush" Value="#3A5BD9"/>
              </Trigger>
            </ControlTemplate.Triggers>
          </ControlTemplate>
        </Setter.Value>
      </Setter>
    </Style>
  </Window.Resources>

  <Grid>
    <Grid.RowDefinitions>
      <RowDefinition Height="Auto"/>
      <RowDefinition Height="*"/>
      <RowDefinition Height="Auto"/>
    </Grid.RowDefinitions>

    <Border Grid.Row="0" Background="#141821" BorderBrush="{StaticResource Line}" BorderThickness="0,0,0,1" Padding="22,16">
      <Grid>
        <Grid.ColumnDefinitions>
          <ColumnDefinition Width="Auto"/>
          <ColumnDefinition Width="*"/>
          <ColumnDefinition Width="Auto"/>
        </Grid.ColumnDefinitions>
        <StackPanel Grid.Column="0">
          <TextBlock Text="MGS4 DLSS" FontSize="21" FontWeight="SemiBold" Foreground="{StaticResource Text}"/>
          <TextBlock x:Name="Caption" Text="start a scene" FontSize="13" Foreground="{StaticResource Accent}" Margin="0,1,0,6"/>
          <TextBlock x:Name="GamePath" FontSize="11" Foreground="{StaticResource Muted}" FontFamily="Consolas"
                     TextTrimming="CharacterEllipsis" MaxWidth="360"/>
        </StackPanel>
        <StackPanel Grid.Column="1" Orientation="Horizontal" HorizontalAlignment="Center" VerticalAlignment="Center">
          <RadioButton x:Name="NavPlay" Style="{StaticResource Nav}" Content="Play" IsChecked="True" GroupName="nav"/>
          <RadioButton x:Name="NavSettings" Style="{StaticResource Nav}" Content="Settings" GroupName="nav"/>
          <RadioButton x:Name="NavInstall" Style="{StaticResource Nav}" Content="Setup" GroupName="nav"/>
        </StackPanel>
        <Border x:Name="Pill" Grid.Column="2" CornerRadius="8" Padding="16,9" Background="#161B2A" BorderBrush="#33436E"
                BorderThickness="1" VerticalAlignment="Center" MinWidth="150">
          <StackPanel>
            <TextBlock x:Name="PillText" Text="idle" FontSize="15" FontWeight="SemiBold" Foreground="#7C9CFF"
                       HorizontalAlignment="Center"/>
            <TextBlock x:Name="PillNote" Text="" FontSize="11" Foreground="{StaticResource Muted}"
                       HorizontalAlignment="Center" Margin="0,2,0,0"/>
          </StackPanel>
        </Border>
      </Grid>
    </Border>

    <Grid x:Name="PlayView" Grid.Row="1" Margin="22,16,22,0">
      <Grid.ColumnDefinitions>
        <ColumnDefinition Width="*"/>
        <ColumnDefinition Width="370"/>
      </Grid.ColumnDefinitions>

      <Border Grid.Column="0" Background="{StaticResource Card}" BorderBrush="{StaticResource Line}"
              BorderThickness="1" CornerRadius="10">
        <Grid>
          <Grid.RowDefinitions>
            <RowDefinition Height="Auto"/>
            <RowDefinition Height="*"/>
          </Grid.RowDefinitions>
          <Border Grid.Row="0" Background="#1B1F29" BorderBrush="{StaticResource Line}" BorderThickness="0,0,0,1"
                  CornerRadius="10,10,0,0" Padding="14,12">
            <StackPanel>
              <Grid>
                <TextBox x:Name="Search"/>
                <TextBlock x:Name="SearchHint" Text="Search by scene name, stage id or act" FontSize="12"
                           Foreground="#5C6478" IsHitTestVisible="False" VerticalAlignment="Center" Margin="10,0,0,0"/>
              </Grid>
              <WrapPanel x:Name="Filters" Margin="0,10,0,-6"/>
            </StackPanel>
          </Border>
          <ListBox x:Name="SceneList" Grid.Row="1" Margin="0,4,0,6"/>
        </Grid>
      </Border>

      <Border Grid.Column="1" Background="{StaticResource Card}" BorderBrush="{StaticResource Line}" BorderThickness="1"
              CornerRadius="10" Margin="14,0,0,0">
        <Grid>
          <Grid.RowDefinitions>
            <RowDefinition Height="Auto"/>
            <RowDefinition Height="*"/>
            <RowDefinition Height="Auto"/>
          </Grid.RowDefinitions>
          <Border Grid.Row="0" Background="#1B1F29" BorderBrush="{StaticResource Line}" BorderThickness="0,0,0,1"
                  CornerRadius="10,10,0,0" Padding="18,13">
            <StackPanel>
              <TextBlock x:Name="PickTitle" Text="Nothing picked" FontSize="14" FontWeight="SemiBold"
                         Foreground="{StaticResource Text}" TextTrimming="CharacterEllipsis"/>
              <TextBlock x:Name="PickSub" Text="Choose a scene on the left." FontSize="11"
                         Foreground="{StaticResource Muted}" Margin="0,2,0,0" TextWrapping="Wrap"/>
              <TextBlock x:Name="PickWarn" FontSize="11" Foreground="#F2C14E" Margin="0,7,0,0"
                         TextWrapping="Wrap" Visibility="Collapsed"/>
              <StackPanel x:Name="AltRow" Orientation="Horizontal" Margin="0,9,0,0" Visibility="Collapsed">
                <TextBlock Text="Same scene, two ids:" FontSize="11" Foreground="{StaticResource Muted}"
                           VerticalAlignment="Center"/>
                <ComboBox x:Name="AltPick" Width="140" Height="26" Margin="8,0,0,0"/>
              </StackPanel>
            </StackPanel>
          </Border>
          <ScrollViewer Grid.Row="1" VerticalScrollBarVisibility="Auto" Padding="18,14,14,10">
            <StackPanel>
              <CheckBox x:Name="OptAdvance" Content="Skip the boot prompts"/>
              <TextBlock Text="Presses through the auto-save notice and the &quot;press any button&quot; screen until the first 3D frame."
                         FontSize="11" Foreground="{StaticResource Muted}" TextWrapping="Wrap" Margin="25,3,0,13"/>
              <CheckBox x:Name="OptMashX" Content="Keep pressing X"/>
              <TextBlock x:Name="MashNote" FontSize="11" Foreground="{StaticResource Muted}" TextWrapping="Wrap" Margin="25,3,0,13"/>
              <CheckBox x:Name="OptEnd" Content="Close the game when gameplay starts"/>
              <TextBlock Text="For cutscenes: the HUD coming up means the scene has handed over."
                         FontSize="11" Foreground="{StaticResource Muted}" TextWrapping="Wrap" Margin="25,3,0,13"/>
              <CheckBox x:Name="OptHold" Content="Close it after a fixed time"/>
              <StackPanel Orientation="Horizontal" Margin="25,5,0,13">
                <TextBox x:Name="HoldSecs" Width="64" Text="60"/>
                <TextBlock Text="seconds into the scene" FontSize="11" Foreground="{StaticResource Muted}"
                           VerticalAlignment="Center" Margin="8,0,0,0"/>
              </StackPanel>
              <Border Height="1" Background="#20242E" Margin="0,2,0,13"/>
              <CheckBox x:Name="OptRes" Content="Set the render resolution"/>
              <StackPanel Orientation="Horizontal" Margin="25,5,0,13">
                <TextBox x:Name="ResW" Width="64" Text="3840"/>
                <TextBlock Text="x" FontSize="12" Foreground="{StaticResource Muted}" VerticalAlignment="Center" Margin="7,0,7,0"/>
                <TextBox x:Name="ResH" Width="64" Text="2160"/>
              </StackPanel>
              <TextBlock Text="The same thing from a terminal" FontSize="11" Foreground="{StaticResource Muted}" Margin="0,4,0,5"/>
              <Border Background="#12151D" BorderBrush="#242935" BorderThickness="1" CornerRadius="6" Padding="9,7">
                <TextBlock x:Name="CmdPreview" FontFamily="Consolas" FontSize="11" Foreground="#9FB6FF" TextWrapping="Wrap"/>
              </Border>
            </StackPanel>
          </ScrollViewer>
          <Border Grid.Row="2" Background="#1B1F29" BorderBrush="{StaticResource Line}" BorderThickness="0,1,0,0"
                  CornerRadius="0,0,10,10" Padding="18,14">
            <StackPanel>
              <Button x:Name="LaunchBtn" Content="Launch" Style="{StaticResource Primary}" Padding="16,11"/>
              <Button x:Name="StopBtn" Content="Close the game" Style="{StaticResource Flat}" Margin="0,8,0,0"/>
              <Button x:Name="ShortcutBtn" Content="Create shortcut" Style="{StaticResource Flat}" Margin="0,8,0,0"/>
            </StackPanel>
          </Border>
        </Grid>
      </Border>
    </Grid>

    <ScrollViewer x:Name="InstallView" Grid.Row="1" Margin="22,16,10,0" VerticalScrollBarVisibility="Auto"
                  Padding="0,0,12,0" Visibility="Collapsed" AllowDrop="True" Background="Transparent">
      <StackPanel x:Name="InstallHost"/>
    </ScrollViewer>

    <Grid x:Name="SettingsView" Grid.Row="1" Margin="22,16,22,0" Visibility="Collapsed">
      <Grid.RowDefinitions>
        <RowDefinition Height="Auto"/>
        <RowDefinition Height="*"/>
      </Grid.RowDefinitions>
      <Border x:Name="LockBanner" Grid.Row="0" Background="#251E10" BorderBrush="#7A6027" BorderThickness="1"
              CornerRadius="8" Padding="16,11" Margin="0,0,0,14" Visibility="Collapsed">
        <TextBlock x:Name="LockText" FontSize="12" Foreground="#F2C14E" TextWrapping="Wrap"/>
      </Border>
      <ScrollViewer Grid.Row="1" VerticalScrollBarVisibility="Auto" Padding="0,0,12,0">
        <StackPanel x:Name="SettingsHost"/>
      </ScrollViewer>
    </Grid>

    <Border Grid.Row="2" Background="#141821" BorderBrush="{StaticResource Line}" BorderThickness="0,1,0,0" Padding="22,13">
      <Grid>
        <Grid.ColumnDefinitions>
          <ColumnDefinition Width="*"/>
          <ColumnDefinition Width="Auto"/>
        </Grid.ColumnDefinitions>
        <TextBlock x:Name="Status" Grid.Column="0" VerticalAlignment="Center" FontSize="11"
                   Foreground="{StaticResource Muted}" TextTrimming="CharacterEllipsis"/>
        <StackPanel Grid.Column="1" Orientation="Horizontal">
          <Button x:Name="CopyBtn" Content="Copy report" Style="{StaticResource Flat}" Margin="0,0,10,0" Visibility="Collapsed"/>
          <Button x:Name="RecheckBtn" Content="Re-check  (F5)" Style="{StaticResource Primary}" Visibility="Collapsed"/>
          <Button x:Name="ReloadBtn" Content="Reload" Style="{StaticResource Flat}" Margin="0,0,10,0" Visibility="Collapsed"/>
          <Button x:Name="SaveBtn" Content="Save settings" Style="{StaticResource Primary}" Visibility="Collapsed"/>
        </StackPanel>
      </Grid>
    </Border>
  </Grid>
</Window>
'@

$script:ItemTemplateXaml = @'
<DataTemplate xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation">
  <Grid>
    <Grid Visibility="{Binding HeaderVis}">
      <Grid.ColumnDefinitions>
        <ColumnDefinition Width="18"/>
        <ColumnDefinition Width="*"/>
        <ColumnDefinition Width="Auto"/>
      </Grid.ColumnDefinitions>
      <TextBlock Grid.Column="0" Text="{Binding Chevron}" FontSize="11" Foreground="#858D9E" VerticalAlignment="Center"/>
      <TextBlock Grid.Column="1" Text="{Binding HeadText}" FontSize="13" FontWeight="SemiBold" Foreground="#E7EAF0"
                 VerticalAlignment="Center"/>
      <TextBlock Grid.Column="2" Text="{Binding HeadCount}" FontSize="11" Foreground="#5C6478" VerticalAlignment="Center"/>
    </Grid>
    <Grid Visibility="{Binding SceneVis}" Margin="18,0,0,0">
      <Grid.ColumnDefinitions>
        <ColumnDefinition Width="130"/>
        <ColumnDefinition Width="*"/>
      </Grid.ColumnDefinitions>
      <TextBlock Grid.Column="0" Text="{Binding Id}" FontFamily="Consolas" FontSize="12" Foreground="#7C9CFF"
                 VerticalAlignment="Center"/>
      <StackPanel Grid.Column="1">
        <TextBlock Text="{Binding Title}" FontSize="13" Foreground="#E7EAF0" TextTrimming="CharacterEllipsis"/>
        <TextBlock Text="{Binding Sub}" FontSize="11" Foreground="#858D9E" Margin="0,1,0,0" TextTrimming="CharacterEllipsis"/>
      </StackPanel>
    </Grid>
  </Grid>
</DataTemplate>
'@

# The two row shapes the scene list holds. Script scope on purpose: the list is built inside a closure, and a
# function defined in the enclosing function would not be visible from there.
function New-HeaderRow($actKey, $title, $count, $collapsed) {
    return [pscustomobject]@{
        IsHeader = $true; ActKey = $actKey
        HeaderVis = "Visible"; SceneVis = "Collapsed"
        HeadText = $title; HeadCount = "$count"
        Chevron = $(if ($collapsed) { [char]0x25B8 } else { [char]0x25BE })   # > and v
        Id = ""; Title = ""; Sub = ""; Entry = $null
    }
}

function New-SceneRow($r) {
    return [pscustomobject]@{
        IsHeader = $false; ActKey = $r.ActKey
        HeaderVis = "Collapsed"; SceneVis = "Visible"
        HeadText = ""; HeadCount = ""; Chevron = ""
        Id = $r.Id; Title = $r.Title; Sub = $r.Sub; Entry = $r.Entry
    }
}

# The game's own icon on the window, taken from the mgs4.exe on this machine rather than shipped - PrivateExtractIcons
# gives the best size it holds (256 in this port), which ExtractAssociatedIcon would flatten to 32.
function Set-WindowIcon($win, [string]$gameDir) {
    if (-not $gameDir) { return }
    $exe = Join-Mgs4Path $gameDir "mgs4.exe"
    if (-not (Test-Mgs4Path $exe)) { return }
    try {
        Add-Type -AssemblyName System.Drawing
        Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices;
public static class Mgs4WinIcon {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)]
  public static extern int PrivateExtractIcons(string file, int index, int cx, int cy, IntPtr[] icons, int[] ids, int count, int flags);
  [DllImport("user32.dll")] public static extern bool DestroyIcon(IntPtr h);
}
"@ -ErrorAction SilentlyContinue
        foreach ($size in @(256, 64, 32)) {
            $handles = New-Object IntPtr[] 1
            $ids = New-Object int[] 1
            if ([Mgs4WinIcon]::PrivateExtractIcons($exe, 0, $size, $size, $handles, $ids, 1, 0) -le 0) { continue }
            if ($handles[0] -eq [IntPtr]::Zero) { continue }
            try {
                $src = [System.Windows.Interop.Imaging]::CreateBitmapSourceFromHIcon(
                    $handles[0], [System.Windows.Int32Rect]::Empty,
                    [System.Windows.Media.Imaging.BitmapSizeOptions]::FromEmptyOptions())
                $src.Freeze()
                $win.Icon = $src
                return
            } finally { [void][Mgs4WinIcon]::DestroyIcon($handles[0]) }
        }
    } catch { }        # an icon is decoration; never let it stop the window opening
}

function ConvertTo-Brush($hex) {
    return New-Object System.Windows.Media.SolidColorBrush ([System.Windows.Media.ColorConverter]::ConvertFromString($hex))
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

# How a check result looks: glyph, text colour, and the pill colours behind it.
$script:StatusStyle = @{
    ok   = @{ Glyph = [char]0x2714; Fg = "#5FD38D"; Bg = "#152318"; Br = "#2C6B45" }
    warn = @{ Glyph = [char]0x25B2; Fg = "#F2C14E"; Bg = "#251E10"; Br = "#7A6027" }
    bad  = @{ Glyph = [char]0x2716; Fg = "#FF7B72"; Bg = "#2A1618"; Br = "#7E3B3B" }
    info = @{ Glyph = [char]0x25CF; Fg = "#7C9CFF"; Bg = "#161B2A"; Br = "#33436E" }
}

# Says the window takes files, because nothing else would.
function New-DropCard {
    $c = New-Card "Drop the downloads here" "" "info" "drag and drop"
    $b = New-Object System.Windows.Controls.Border
    $b.Padding = New-Object System.Windows.Thickness 18, 13, 18, 13
    $t = New-TextBlock ("Drag streamline.zip, renodx-dlss5.addon64, mgs4_dlss.addon64 or the ReShade setup onto " +
                        "this tab and each one goes where it belongs - the zip is unpacked into the game folder, " +
                        "the ReShade setup is started for you. Anything that is not part of the install is left " +
                        "alone and reported.") 11 "#9AA3B4" $false $false
    $b.Child = $t
    [void]$c.Body.Children.Add($b)
    return $c.Card
}

# The Setup tab's first card: which folder everything else is checked against, and how to change it.
function New-GameDirCard($state) {
    $ok = [bool]$state.GameDir
    $c = New-Card "Game folder" $(if ($ok) { Get-GameDirSource } else { "not set" }) `
                  $(if ($ok) { "ok" } else { "bad" }) $(if ($ok) { "found" } else { "not set" })

    $row = New-Object System.Windows.Controls.Border
    $row.Padding = New-Object System.Windows.Thickness 18, 13, 18, 13
    $g = New-Object System.Windows.Controls.Grid
    foreach ($w in @("*", "Auto")) {
        $cd = New-Object System.Windows.Controls.ColumnDefinition
        $cd.Width = $w
        [void]$g.ColumnDefinitions.Add($cd)
    }
    $path = New-TextBlock $(if ($ok) { $state.GameDir } else { "no mgs4.exe found - pick the folder that holds it" }) `
                          12 $(if ($ok) { "#7C9CFF" } else { "#FF7B72" }) $false $true
    $path.VerticalAlignment = [System.Windows.VerticalAlignment]::Center
    $path.Margin = New-Object System.Windows.Thickness 0, 0, 16, 0
    [void]$g.Children.Add($path)

    $buttons = New-Object System.Windows.Controls.StackPanel
    $buttons.Orientation = [System.Windows.Controls.Orientation]::Horizontal
    $buttons.VerticalAlignment = [System.Windows.VerticalAlignment]::Center

    $browse = New-Object System.Windows.Controls.Button
    $browse.Content = "Browse..."
    $browse.Style = $script:FlatStyle
    $browse.Tag = $state
    $browse.Add_Click({
        $st = $this.Tag
        $dlg = New-Object Microsoft.Win32.OpenFileDialog
        $dlg.Title = "Pick mgs4.exe"
        $dlg.Filter = "mgs4.exe|mgs4.exe|Any program (*.exe)|*.exe"
        $dlg.CheckFileExists = $true
        if ($st.GameDir) { $dlg.InitialDirectory = $st.GameDir }
        if ($dlg.ShowDialog() -eq $true) { & $st.ApplyGameDir ([IO.Path]::GetDirectoryName($dlg.FileName)) }
    })
    [void]$buttons.Children.Add($browse)

    $auto = New-Object System.Windows.Controls.Button
    $auto.Content = "Detect"
    $auto.Style = $script:FlatStyle
    $auto.Margin = New-Object System.Windows.Thickness 8, 0, 0, 0
    $auto.Tag = $state
    $auto.ToolTip = "Forget the configured folder and search the Steam libraries again"
    $auto.Add_Click({
        $st = $this.Tag
        [void](Set-ConfiguredGameDir "")          # stop pinning it, then look again - and leave it unpinned
        $found = $null
        try { $found = Get-Mgs4GameDir } catch { }
        & $st.ApplyGameDir $found $false
    })
    [void]$buttons.Children.Add($auto)

    [System.Windows.Controls.Grid]::SetColumn($buttons, 1)
    [void]$g.Children.Add($buttons)
    $row.Child = $g
    [void]$c.Body.Children.Add($row)
    return $c.Card
}

function New-Card($title, $blurb, $tagKind, $tagLabel) {
    $card = New-Object System.Windows.Controls.Border
    $card.Background = ConvertTo-Brush "#171A21"
    $card.BorderBrush = ConvertTo-Brush "#242935"
    $card.BorderThickness = New-Object System.Windows.Thickness 1
    $card.CornerRadius = New-Object System.Windows.CornerRadius 10
    $card.Margin = New-Object System.Windows.Thickness 0, 0, 0, 14
    $stack = New-Object System.Windows.Controls.StackPanel

    $hdr = New-Object System.Windows.Controls.Border
    $hdr.Background = ConvertTo-Brush "#1B1F29"
    $hdr.BorderBrush = ConvertTo-Brush "#242935"
    $hdr.BorderThickness = New-Object System.Windows.Thickness 0, 0, 0, 1
    $hdr.CornerRadius = New-Object System.Windows.CornerRadius 10, 10, 0, 0
    $hdr.Padding = New-Object System.Windows.Thickness 18, 13, 18, 13
    $hg = New-Object System.Windows.Controls.Grid
    foreach ($w in @("*", "Auto")) {
        $cd = New-Object System.Windows.Controls.ColumnDefinition
        $cd.Width = $w
        [void]$hg.ColumnDefinitions.Add($cd)
    }
    $hs = New-Object System.Windows.Controls.StackPanel
    [void]$hs.Children.Add((New-TextBlock $title 14 "#E7EAF0" $true $false))
    if ($blurb) {
        $b = New-TextBlock $blurb 11 "#858D9E" $false $false
        $b.Margin = New-Object System.Windows.Thickness 0, 2, 12, 0
        [void]$hs.Children.Add($b)
    }
    [void]$hg.Children.Add($hs)
    if ($tagLabel) {
        $st = $script:StatusStyle[$tagKind]
        $tag = New-Object System.Windows.Controls.Border
        $tag.Background = ConvertTo-Brush $st.Bg
        $tag.BorderBrush = ConvertTo-Brush $st.Br
        $tag.BorderThickness = New-Object System.Windows.Thickness 1
        $tag.CornerRadius = New-Object System.Windows.CornerRadius 4
        $tag.Padding = New-Object System.Windows.Thickness 12, 4, 12, 4
        $tag.VerticalAlignment = [System.Windows.VerticalAlignment]::Center
        $tag.Child = (New-TextBlock $tagLabel 11 $st.Fg $true $false)
        [System.Windows.Controls.Grid]::SetColumn($tag, 1)
        [void]$hg.Children.Add($tag)
    }
    $hdr.Child = $hg
    [void]$stack.Children.Add($hdr)
    $card.Child = $stack
    return @{ Card = $card; Body = $stack }
}

# What a group's files are and where they come from, above the files themselves. This is the part that turns a list
# of filenames into something you can act on: one link, one instruction, then the paths it produces.
function New-GuideRow($sec) {
    $b = New-Object System.Windows.Controls.Border
    $b.Background = ConvertTo-Brush "#12151D"
    $b.BorderBrush = ConvertTo-Brush "#20242E"
    $b.BorderThickness = New-Object System.Windows.Thickness 0, 0, 0, 1
    $b.Padding = New-Object System.Windows.Thickness 18, 12, 18, 12
    $g = New-Object System.Windows.Controls.Grid
    foreach ($w in @("*", "Auto")) {
        $cd = New-Object System.Windows.Controls.ColumnDefinition
        $cd.Width = $w
        [void]$g.ColumnDefinitions.Add($cd)
    }
    $t = New-TextBlock $sec.Guide 11 "#9AA3B4" $false $false
    $t.VerticalAlignment = [System.Windows.VerticalAlignment]::Center
    $t.Margin = New-Object System.Windows.Thickness 0, 0, 16, 0
    [void]$g.Children.Add($t)
    if ($sec.Url) {
        $link = New-Object System.Windows.Controls.Button
        $link.Content = $(if ($sec.UrlLabel) { $sec.UrlLabel } else { "Get the files" }) + "  " + [char]0x2192
        $link.Style = $script:LinkStyle
        $link.Tag = $sec.Url
        $link.ToolTip = $sec.Url
        $link.VerticalAlignment = [System.Windows.VerticalAlignment]::Center
        $link.Add_Click({ Start-Process $this.Tag })
        [System.Windows.Controls.Grid]::SetColumn($link, 1)
        [void]$g.Children.Add($link)
    }
    $b.Child = $g
    return $b
}

# One row of the install check: the path, what it is, the value found, and a link when that one file comes from
# somewhere other than its group.
function New-CheckRow($row, $first) {
    $st = $script:StatusStyle[$row.Status]
    $rb = New-Object System.Windows.Controls.Border
    $rb.Padding = New-Object System.Windows.Thickness 18, 11, 18, 11
    if (-not $first) {
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
    [void]$mid.Children.Add((New-TextBlock $row.Name 13 "#E7EAF0" $false $true))
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
        $lb.Style = $script:LinkStyle
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
    return $rb
}

# Windows groups taskbar buttons by AppUserModelID, and a process that never sets one inherits its host's - which
# is why the window sat under "Windows PowerShell". Claiming an id of our own, before any window exists, makes it a
# separate taskbar entry named after the window instead.
function Set-AppUserModelId([string]$id) {
    try {
        Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices;
public static class Mgs4AppId {
  [DllImport("shell32.dll", CharSet=CharSet.Unicode, PreserveSig=false)]
  public static extern void SetCurrentProcessExplicitAppUserModelID(string appId);
}
"@ -ErrorAction SilentlyContinue
        [Mgs4AppId]::SetCurrentProcessExplicitAppUserModelID($id)
    } catch { }
}

function Show-AppWindow($opt, $gameDir, $startTab) {
    Add-Type -AssemblyName PresentationFramework, PresentationCore, WindowsBase
    Set-AppUserModelId "NeilGraham.Mgs4Dlss"

    # Every scriptblock below is closed with GetNewClosure(), which binds it to its own dynamic module - so
    # $script:... and the automatic variables inside one are NOT this scope's. Anything shared is captured here,
    # above every closure that uses it: a local declared later would be captured as $null.
    $spec = $script:IniSpec
    $selfPath = $PSCommandPath
    # GameDir lives in $state because the Setup tab can repoint it while the window is open; every closure below
    # reads $state.GameDir rather than closing over the value it had at startup.
    $state = @{ Controls = @(); RunProc = $null; Sections = $null; Collapsed = @{}; PickedId = ""
                GameDir = $gameDir }


    $win = [Windows.Markup.XamlReader]::Load((New-Object System.Xml.XmlNodeReader ([xml]$script:Xaml)))
    Add-Type -Namespace Mgs4 -Name Dwm -MemberDefinition @'
[DllImport("dwmapi.dll")] public static extern int DwmSetWindowAttribute(IntPtr h, int attr, ref int val, int size);
'@ -ErrorAction SilentlyContinue
    $win.Add_SourceInitialized({
        $h = (New-Object System.Windows.Interop.WindowInteropHelper $win).Handle
        $on = 1
        # 20 = DWMWA_USE_IMMERSIVE_DARK_MODE; 19 on Windows 10 builds before 20H1
        if ([Mgs4.Dwm]::DwmSetWindowAttribute($h, 20, [ref]$on, 4) -ne 0) {
            [void][Mgs4.Dwm]::DwmSetWindowAttribute($h, 19, [ref]$on, 4)
        }
    })

    $ui = @{}
    foreach ($n in @("GamePath", "Caption", "NavPlay", "NavSettings", "NavInstall", "Pill", "PillText", "PillNote", "PlayView",
                     "SettingsView", "InstallView", "InstallHost", "CopyBtn", "RecheckBtn",
                     "Search", "SearchHint", "Filters", "SceneList", "PickTitle", "PickSub", "PickWarn", "AltRow", "AltPick", "OptAdvance", "OptMashX", "MashNote",
                     "OptEnd", "OptHold", "HoldSecs", "OptRes", "ResW", "ResH", "CmdPreview", "LaunchBtn", "StopBtn",
                     "Status", "ShortcutBtn", "ReloadBtn", "SaveBtn", "SettingsHost", "LockBanner", "LockText")) {
        $ui[$n] = $win.FindName($n)
    }
    $script:LinkStyle = $win.FindResource("Link")
    $script:FlatStyle = $win.FindResource("Flat")
    Set-WindowIcon $win $state.GameDir

    $refreshGamePath = {
        $ui.GamePath.Text = $(if ($state.GameDir) { $state.GameDir }
                              elseif ($opt.GameDirBad) { "no mgs4.exe in $($opt.GameDirBad) - see Setup" }
                              else { "no MGS4 install found - see Setup" })
        $ui.GamePath.ToolTip = $ui.GamePath.Text
    }.GetNewClosure()
    & $refreshGamePath
    $ui.SceneList.ItemTemplate = [Windows.Markup.XamlReader]::Parse($script:ItemTemplateXaml)

    $pad = Open-Pad
    [Mgs4Pad]::Close()
    $ui.MashNote.Text = if ($pad.Ok) {
        "A virtual DualShock 4 taps Cross about six times a second, which is what MGS4's in-cutscene flashback prompts want. The game must stay in the foreground."
    } else {
        "Needs ViGEmBus and ViGEmClient.dll ($($pad.Why)). Without them the launcher can only press Enter, which gets past the prompts but does not fire the flashbacks."
    }

    # ------------------------------------------------------------------ the scene list
    # Rows are act headers and scenes in one list: the ListBox item template shows whichever half the row says,
    # which keeps the grouping without a DataTemplateSelector.
    # Which of the filter chips a scene answers to. Worked out once here, so filtering is a set test rather than a
    # predicate run over four hundred rows on every keystroke.
    $catsOf = {
        param($e)
        $c = @()
        if ($e.Kind -eq "start") { $c += "Start the game" }
        if ($e.Kind -eq "cutscene" -or $e.Kind -eq "briefing") { $c += "Cutscenes" }
        if ($e.Kind -eq "briefing") { $c += "Mission briefings" }
        if ($e.Name) { $c += "Named scenes" }
        if ($e.Kind -eq "gameplay") { $c += "Gameplay" }
        if ($e.Kind -eq "stage-entry") { $c += "Stage entries" }
        if ($e.Hidden) { $c += "Known broken" }
        return $c
    }
    $all = @(Get-SceneCatalogue | ForEach-Object {
        [pscustomobject]@{
            Entry = $_
            Id = $_.Id; Kind = $_.Kind; ActKey = $_.ActKey; Name = $_.Name; Hidden = $_.Hidden
            Title = $(if ($_.Name) { $_.Name } else { $_.Id })
            Sub = $(if ($_.Description) { $_.Description } else { $_.Note })
            Cats = @(& $catsOf $_)
            Hay = "$($_.Id) $($_.Alts -join ' ') $($_.Name) $($_.ActTitle) $($_.Kind) $($_.Description)".ToLower()
        }
    })
    $actOrder = $script:ActOrder        # populated by Get-SceneCatalogue, just above
    $actTitles = $script:ActTitles
    $catNames = @("Start the game", "Cutscenes", "Mission briefings", "Named scenes", "Gameplay", "Stage entries",
                  "Known broken")

    $applyFilter = {
        $q = $ui.Search.Text.Trim().ToLower()
        $ui.SearchHint.Visibility = $(if ($ui.Search.Text) { "Collapsed" } else { "Visible" })

        # No chip ticked means everything (bar the broken ids); ticking chips shows the union of what they cover.
        $picked = @($ui.Filters.Children | Where-Object { $_.IsChecked } | ForEach-Object { "$($_.Tag)" })
        $rows = $all
        if ($picked.Count) {
            $rows = @($rows | Where-Object { @($_.Cats | Where-Object { $picked -contains $_ }).Count -gt 0 })
        }
        if ($picked -notcontains "Known broken") { $rows = @($rows | Where-Object { -not $_.Hidden }) }
        if ($q) { $rows = @($rows | Where-Object { $_.Hay.Contains($q) }) }

        # A search is a request to see what matched, so it overrides the collapsed groups.
        $searching = [bool]$q
        $out = New-Object System.Collections.Generic.List[object]
        foreach ($key in $actOrder) {
            $inAct = @($rows | Where-Object { $_.ActKey -eq $key })
            if ($inAct.Count -eq 0) { continue }
            $collapsed = (-not $searching) -and $state.Collapsed[$key]
            [void]$out.Add((New-HeaderRow $key $actTitles[$key] $inAct.Count $collapsed))
            if (-not $collapsed) { foreach ($r in $inAct) { [void]$out.Add((New-SceneRow $r)) } }
        }
        $keepId = $state.PickedId
        $ui.SceneList.ItemsSource = $out
        if ($keepId) {
            $hit = @($out | Where-Object { -not $_.IsHeader -and $_.Id -eq $keepId }) | Select-Object -First 1
            if ($hit) { $ui.SceneList.SelectedItem = $hit }
        }
        $shown = @($rows).Count
        $ui.Status.Text = "$shown of $($all.Count) entries" +
                          $(if ($picked.Count) { " - " + ($picked -join ", ") } else { "" })
    }.GetNewClosure()

    # Clicking an act header expands or collapses it rather than picking anything.
    $toggleGroup = {
        param($row)
        if (-not $row -or -not $row.IsHeader) { return }
        $state.Collapsed[$row.ActKey] = -not $state.Collapsed[$row.ActKey]
        & $applyFilter
    }.GetNewClosure()

    # The row the mouse is over, or $null. OriginalSource is whatever bit of the template was hit, so walk up the
    # visual tree to the container; anything that is not a Visual (a text Run, say) ends the walk.
    $rowUnderMouse = {
        param($src)
        while ($src -and -not ($src -is [System.Windows.Controls.ListBoxItem])) {
            if ($src -is [System.Windows.Media.Visual]) { $src = [System.Windows.Media.VisualTreeHelper]::GetParent($src) }
            else { $src = $null }
        }
        if ($src) { return $src.DataContext }
        return $null
    }.GetNewClosure()

    # ------------------------------------------------------------------ options <-> command line
    $collect = {
        $o = New-Options
        $o.GameDir = $opt.GameDir
        $o.GameDirGiven = $opt.GameDirGiven
        $sel = $ui.SceneList.SelectedItem
        if ($sel -and $sel.IsHeader) { $sel = $null }
        $o.Stage = $(if ($sel) { $sel.Id } else { "" })
        if ($sel -and $ui.AltRow.Visibility -eq [System.Windows.Visibility]::Visible -and $ui.AltPick.SelectedItem) {
            $o.Stage = "$($ui.AltPick.SelectedItem)"
        }
        if (Test-StartEntry $o.Stage) {
            $o.Advance = $false            # a menu has no boot prompts and no first 3D frame
        } else {
            $o.Advance = [bool]$ui.OptAdvance.IsChecked
            $o.MashX = [bool]$ui.OptMashX.IsChecked
            $o.EndOnGameplay = [bool]$ui.OptEnd.IsChecked
            if ($ui.OptHold.IsChecked) { $o.Hold = [double]($ui.HoldSecs.Text -replace '[^\d.]', '') }
        }
        if ($ui.OptRes.IsChecked) {
            $o.Width = [int]($ui.ResW.Text -replace '\D', '')
            $o.Height = [int]($ui.ResH.Text -replace '\D', '')
        }
        return $o
    }.GetNewClosure()

    $refreshPreview = {
        $sel = $ui.SceneList.SelectedItem
        if ($sel) {
            $ui.PickTitle.Text = $sel.Title
            $ui.PickSub.Text = $sel.Sub
            $ui.LaunchBtn.IsEnabled = [bool]$state.GameDir
            $ui.ShortcutBtn.IsEnabled = $true
            $ui.CmdPreview.Text = Format-CliPreview (Get-CliArgs (& $collect))

            # The run options drive a scene. On a menu there is nothing to press through, and tapping Cross would
            # just start a new game, so they are switched off and greyed rather than quietly ignored.
            $isStart = ($sel.Kind -eq "start")
            foreach ($c in @($ui.OptAdvance, $ui.OptMashX, $ui.OptEnd, $ui.OptHold)) { $c.IsEnabled = -not $isStart }
            $warn = ""
            if ($isStart) { $warn = "The run options below are for scenes; the game is started and left alone here." }
            # Frame generation on MGS4's own menu loses the device on most launches (not all - it is a race).
            if ($sel.Id -eq "@main" -and $state.GameDir) {
                $fgm = Get-IniValue (Join-Mgs4Path $state.GameDir "mgs4_dlss.ini") "FrameGen"
                if ($fgm -and $fgm -ne "0") {
                    $warn = "Frame generation (FrameGen=$fgm) crashes the game on this menu most of the time: " +
                            "sl.dlss_g stops evaluating and the device is lost within a minute. Set it to 0 on the " +
                            "Settings tab to sit on the menu - scenes are unaffected."
                }
            }

            # Some scenes are reachable under two stage ids. One row, and the id to boot picked here.
            $alts = @()
            if ($sel.Entry) { $alts = @($sel.Entry.Alts) }
            if ($alts.Count) {
                $wanted = "$($ui.AltPick.SelectedItem)"
                $ids = @($sel.Id) + $alts
                if ($ui.AltPick.Tag -ne $sel.Id) {
                    $ui.AltPick.Items.Clear()
                    foreach ($i in $ids) { [void]$ui.AltPick.Items.Add($i) }
                    $ui.AltPick.Tag = $sel.Id
                    $ui.AltPick.SelectedIndex = 0
                } elseif ($ids -notcontains $wanted) { $ui.AltPick.SelectedIndex = 0 }
                $ui.AltRow.Visibility = "Visible"
            } else {
                $ui.AltRow.Visibility = "Collapsed"
                $ui.AltPick.Tag = $null
            }
            $ui.PickWarn.Text = $warn
            $ui.PickWarn.Visibility = $(if ($warn) { "Visible" } else { "Collapsed" })
        } else {
            $ui.PickTitle.Text = "Nothing picked"
            $ui.PickSub.Text = "Choose a scene on the left."
            $ui.LaunchBtn.IsEnabled = $false
            $ui.ShortcutBtn.IsEnabled = $false
            $ui.CmdPreview.Text = "mgs4-dlss --list"
            $ui.PickWarn.Visibility = "Collapsed"
            $ui.AltRow.Visibility = "Collapsed"
            foreach ($c in @($ui.OptAdvance, $ui.OptMashX, $ui.OptEnd, $ui.OptHold)) { $c.IsEnabled = $true }
        }
        Save-Prefs ([pscustomobject]@{
            Stage = $state.PickedId
            Filters = @($ui.Filters.Children | Where-Object { $_.IsChecked } | ForEach-Object { "$($_.Tag)" })
            Advance = [bool]$ui.OptAdvance.IsChecked; MashX = [bool]$ui.OptMashX.IsChecked
            EndOnGameplay = [bool]$ui.OptEnd.IsChecked; Hold = [bool]$ui.OptHold.IsChecked
            HoldSecs = $ui.HoldSecs.Text; Res = [bool]$ui.OptRes.IsChecked
            ResW = $ui.ResW.Text; ResH = $ui.ResH.Text; Seen = $true
        })
    }.GetNewClosure()

    # ------------------------------------------------------------------ settings

    $buildSettings = {
        $ui.SettingsHost.Children.Clear()
        $state.Controls = @()
        $groups = @()
        foreach ($s in $spec) { if ($groups -notcontains $s.Group) { $groups += $s.Group } }
        foreach ($g in $groups) {
            $card = New-Object System.Windows.Controls.Border
            $card.Background = ConvertTo-Brush "#171A21"
            $card.BorderBrush = ConvertTo-Brush "#242935"
            $card.BorderThickness = New-Object System.Windows.Thickness 1
            $card.CornerRadius = New-Object System.Windows.CornerRadius 10
            $card.Margin = New-Object System.Windows.Thickness 0, 0, 0, 14
            $stack = New-Object System.Windows.Controls.StackPanel

            $hdr = New-Object System.Windows.Controls.Border
            $hdr.Background = ConvertTo-Brush "#1B1F29"
            $hdr.BorderBrush = ConvertTo-Brush "#242935"
            $hdr.BorderThickness = New-Object System.Windows.Thickness 0, 0, 0, 1
            $hdr.CornerRadius = New-Object System.Windows.CornerRadius 10, 10, 0, 0
            $hdr.Padding = New-Object System.Windows.Thickness 18, 12, 18, 12
            $hdr.Child = (New-TextBlock $g 14 "#E7EAF0" $true)
            [void]$stack.Children.Add($hdr)

            $first = $true
            foreach ($s in ($spec | Where-Object { $_.Group -eq $g })) {
                $cur = Get-IniValue (Join-Mgs4Path $state.GameDir "mgs4_dlss.ini") $s.Key
                $row = New-Object System.Windows.Controls.Border
                $row.Padding = New-Object System.Windows.Thickness 18, 11, 18, 11
                if (-not $first) {
                    $row.BorderBrush = ConvertTo-Brush "#20242E"
                    $row.BorderThickness = New-Object System.Windows.Thickness 0, 1, 0, 0
                }
                $first = $false
                $grid = New-Object System.Windows.Controls.Grid
                foreach ($w in @("*", "230")) {
                    $cd = New-Object System.Windows.Controls.ColumnDefinition
                    $cd.Width = $w
                    [void]$grid.ColumnDefinitions.Add($cd)
                }
                $left = New-Object System.Windows.Controls.StackPanel
                $left.VerticalAlignment = [System.Windows.VerticalAlignment]::Center
                $left.Margin = New-Object System.Windows.Thickness 0, 0, 18, 0
                [void]$left.Children.Add((New-TextBlock $s.Label 13 "#E7EAF0" $false))
                $help = New-TextBlock ($s.Key + " - " + $s.Help) 11 "#858D9E" $false
                $help.Margin = New-Object System.Windows.Thickness 0, 2, 0, 0
                [void]$left.Children.Add($help)
                [void]$grid.Children.Add($left)

                $ctl = $null
                switch ($s.Type) {
                    "bool" {
                        $ctl = New-Object System.Windows.Controls.CheckBox
                        $ctl.IsChecked = ($cur -eq "1")
                        $ctl.HorizontalAlignment = [System.Windows.HorizontalAlignment]::Right
                    }
                    "choice" {
                        $ctl = New-Object System.Windows.Controls.ComboBox
                        $ctl.Height = 30
                        $labels = $s.Choices
                        if ($s.ChoiceLabels) { $labels = $s.ChoiceLabels }
                        foreach ($l in $labels) { [void]$ctl.Items.Add($l) }
                        $idx = [array]::IndexOf([string[]]$s.Choices, [string]$cur)
                        $ctl.SelectedIndex = $(if ($idx -ge 0) { $idx } else { 0 })
                    }
                    "int" {
                        $ctl = New-Object System.Windows.Controls.TextBox
                        $ctl.Text = "$cur"
                        $ctl.HorizontalAlignment = [System.Windows.HorizontalAlignment]::Right
                        $ctl.Width = 100
                    }
                    default {
                        $ctl = New-TextBlock $(if ($cur) { $cur } else { "(unset)" }) 12 "#7C9CFF" $false
                        $ctl.HorizontalAlignment = [System.Windows.HorizontalAlignment]::Right
                        $ctl.FontFamily = New-Object System.Windows.Media.FontFamily "Consolas"
                    }
                }
                $ctl.VerticalAlignment = [System.Windows.VerticalAlignment]::Center
                [System.Windows.Controls.Grid]::SetColumn($ctl, 1)
                [void]$grid.Children.Add($ctl)
                $row.Child = $grid
                [void]$stack.Children.Add($row)
                if ($s.Type -ne "readonly") { $state.Controls += @{ Spec = $s; Control = $ctl; Was = $cur } }
            }
            $card.Child = $stack
            [void]$ui.SettingsHost.Children.Add($card)
        }
    }.GetNewClosure()

    $saveSettings = {
        if (Test-GameRunning) { return }
        $vals = @{}
        foreach ($e in $state.Controls) {
            $s = $e.Spec
            $v = switch ($s.Type) {
                "bool"   { if ($e.Control.IsChecked) { "1" } else { "0" } }
                "choice" { $s.Choices[[Math]::Max(0, $e.Control.SelectedIndex)] }
                default  { ($e.Control.Text).Trim() }
            }
            if ("$v" -ne "$($e.Was)") { $vals[$s.Key] = $v }
        }
        if ($vals.Count -eq 0) { $ui.Status.Text = "settings unchanged"; return }
        try {
            Set-Ini (Join-Mgs4Path $state.GameDir "mgs4_dlss.ini") $vals
            $ui.Status.Text = "wrote " + (($vals.Keys | Sort-Object) -join ", ") + " to mgs4_dlss.ini"
            & $buildSettings
        } catch {
            $ui.Status.Text = "could not write mgs4_dlss.ini: $($_.Exception.Message)"
        }
    }.GetNewClosure()

    # ------------------------------------------------------------------ live state
    $refreshState = {
        $running = Test-GameRunning
        $busy = $state.RunProc -and -not $state.RunProc.HasExited
        if ($busy) {
            $ui.PillText.Text = "driving"; $ui.PillNote.Text = "the launcher is attached"
            $ui.Pill.Background = ConvertTo-Brush "#251E10"; $ui.Pill.BorderBrush = ConvertTo-Brush "#7A6027"
            $ui.PillText.Foreground = ConvertTo-Brush "#F2C14E"
        } elseif ($running) {
            $ui.PillText.Text = "running"; $ui.PillNote.Text = "mgs4.exe is up"
            $ui.Pill.Background = ConvertTo-Brush "#152318"; $ui.Pill.BorderBrush = ConvertTo-Brush "#2C6B45"
            $ui.PillText.Foreground = ConvertTo-Brush "#5FD38D"
        } else {
            $ui.PillText.Text = "idle"; $ui.PillNote.Text = "nothing is running"
            $ui.Pill.Background = ConvertTo-Brush "#161B2A"; $ui.Pill.BorderBrush = ConvertTo-Brush "#33436E"
            $ui.PillText.Foreground = ConvertTo-Brush "#7C9CFF"
        }
        $ui.StopBtn.IsEnabled = $running -or $busy
        $ui.SaveBtn.IsEnabled = (-not $running) -and [bool]$state.GameDir
            if ($ui.SettingsView.Visibility -eq [System.Windows.Visibility]::Visible) {
            if (-not $state.GameDir) {
                $ui.LockText.Text = "No MGS4 install found, so there is no mgs4_dlss.ini to read or write. The Install tab says what was looked for."
                $ui.LockBanner.Visibility = [System.Windows.Visibility]::Visible
            } elseif ($running) {
                $ui.LockText.Text = "The game is running. It rewrites mgs4_dlss.ini through the Windows profile API, whose cache would undo anything written from here - close the game to save. Most of these keys are read again every second by the add-on, and its own overlay (ReShade, Add-ons tab) can change them live."
                $ui.LockBanner.Visibility = [System.Windows.Visibility]::Visible
            } else {
                $ui.LockBanner.Visibility = [System.Windows.Visibility]::Collapsed
            }
        }
    }.GetNewClosure()

    # ------------------------------------------------------------------ wiring
    $ui.Search.Add_TextChanged($applyFilter)
    foreach ($name in $catNames) {
        $chip = New-Object System.Windows.Controls.Primitives.ToggleButton
        $chip.Content = $name
        $chip.Tag = $name
        $chip.Style = $win.FindResource("Chip")
        $chip.Add_Checked($applyFilter)
        $chip.Add_Unchecked($applyFilter)
        [void]$ui.Filters.Children.Add($chip)
    }
    # Act headers are toggled here, before the ListBox gets the click, and the event is marked handled so a header
    # is never selected. Driving this from SelectionChanged instead used to fire twice on some clicks: rebuilding
    # the list inside the handler left the ListBox to finish its click against the rebuilt row, which selected the
    # header again and toggled it straight back.
    $ui.SceneList.Add_PreviewMouseLeftButtonDown({
        param($sender, $e)
        $row = & $rowUnderMouse $e.OriginalSource
        if ($row -and $row.IsHeader) {
            $e.Handled = $true
            & $toggleGroup $row
        }
    }.GetNewClosure())

    # Keyboard: headers can still be reached with the arrow keys, where Enter or Space opens and closes them.
    $ui.SceneList.Add_KeyDown({
        param($sender, $e)
        if ($e.Key -ne [System.Windows.Input.Key]::Return -and $e.Key -ne [System.Windows.Input.Key]::Space) { return }
        $sel = $ui.SceneList.SelectedItem
        if ($sel -and $sel.IsHeader) { $e.Handled = $true; & $toggleGroup $sel }
    }.GetNewClosure())

    $ui.SceneList.Add_SelectionChanged({
        $sel = $ui.SceneList.SelectedItem
        if ($sel -and $sel.IsHeader) { return }      # arrow-keyed onto a header; nothing to preview
        if ($sel) { $state.PickedId = $sel.Id }
        & $refreshPreview
    }.GetNewClosure())
    $ui.AltPick.Add_SelectionChanged($refreshPreview)
    foreach ($c in @($ui.OptAdvance, $ui.OptMashX, $ui.OptEnd, $ui.OptHold, $ui.OptRes)) {
        $c.Add_Checked($refreshPreview); $c.Add_Unchecked($refreshPreview)
    }
    foreach ($t in @($ui.HoldSecs, $ui.ResW, $ui.ResH)) { $t.Add_TextChanged($refreshPreview) }

    # A WPF event handler that throws does it silently, so anything that goes wrong here has to be caught and said.
    $ui.LaunchBtn.Add_Click({
        try {
            $o = & $collect
            if (-not $o.Stage) { $ui.Status.Text = "pick a scene first"; return }
            # Start-Process joins -ArgumentList with spaces and quotes nothing, so anything holding a space (this
            # script's own path, under "Neil Graham" or "Program Files") has to be quoted here or powershell.exe
            # sees it as two arguments and silently starts nothing.
            $quote = { param($s) if ($s -match '\s') { '"' + $s + '"' } else { $s } }
            $psArgs = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-WindowStyle", "Hidden", "-File", (& $quote $selfPath))
            $psArgs += @(Get-CliArgs $o | ForEach-Object { & $quote $_ })
            $state.RunProc = Start-Process -FilePath "powershell.exe" -ArgumentList $psArgs -PassThru
            $ui.Status.Text = "launched $($o.Stage) - hold Escape to stop an attached run"
            & $refreshState
        } catch {
            $ui.Status.Text = "could not launch: $($_.Exception.Message)"
        }
    }.GetNewClosure())

    $ui.StopBtn.Add_Click({
        if ($state.RunProc -and -not $state.RunProc.HasExited) {
            try { $state.RunProc.Kill() } catch {}
        }
        Stop-Game
        $ui.Status.Text = "closed the game"
        & $refreshState
    }.GetNewClosure())

    $ui.ShortcutBtn.Add_Click({
        try {
            $o = & $collect
            if (-not $o.Stage) { $ui.Status.Text = "pick a scene first"; return }
            $scene = Find-Scene $o.Stage
            $dlg = New-Object Microsoft.Win32.SaveFileDialog
            $dlg.Title = "Save a shortcut for $($o.Stage)"
            $dlg.Filter = "Shortcut (*.lnk)|*.lnk"
            $dlg.DefaultExt = ".lnk"
            $dlg.AddExtension = $true
            $dlg.FileName = $(if ($scene) { Get-ShortcutName $scene } else { $o.Stage })
            $dlg.InitialDirectory = [Environment]::GetFolderPath("Desktop")
            if ($dlg.ShowDialog() -ne $true) { return }
            $written = New-SceneShortcut $o $dlg.FileName
            $ui.Status.Text = "shortcut written: $written"
        } catch {
            $ui.Status.Text = "could not write the shortcut: $($_.Exception.Message)"
        }
    }.GetNewClosure())

    $ui.SaveBtn.Add_Click($saveSettings)
    $ui.ReloadBtn.Add_Click({ & $buildSettings; $ui.Status.Text = "reloaded mgs4_dlss.ini" }.GetNewClosure())

    # ------------------------------------------------------------------ install check
    $buildInstall = {
        # Queued at Background priority, so the tab may have been left before this runs.
        if ($ui.InstallView.Visibility -ne [System.Windows.Visibility]::Visible) { return }
        $ui.InstallHost.Children.Clear()
        if (-not $state.GameDir) {
            [void]$ui.InstallHost.Children.Add((New-GameDirCard $state))
            $why = "The Steam libraries were searched for app 2492670 and no mgs4.exe turned up."
            if ($opt.GameDirBad) { $why = "There is no mgs4.exe in $($opt.GameDirBad)." }
            $c = New-Card "Nothing to check yet" ($why + " Point the app at the folder holding mgs4.exe with " +
                 "Browse above, and everything below fills in.") "bad" "no game folder"
            [void]$ui.InstallHost.Children.Add($c.Card)
            $ui.Status.Text = "no game folder set"
            return
        }
        $sections = Invoke-InstallChecks -Game $state.GameDir
        $state.Sections = $sections
        $v = Get-Verdict $sections
        $vc = New-Card ("Install check: " + $v.Text) $v.Note $v.Kind $v.Text
        [void]$ui.InstallHost.Children.Add($vc.Card)
        [void]$ui.InstallHost.Children.Add((New-GameDirCard $state))
        [void]$ui.InstallHost.Children.Add((New-DropCard))
        foreach ($sec in $sections) {
            $bad = @($sec.Rows | Where-Object { $_.Status -eq "bad" }).Count
            $warn = @($sec.Rows | Where-Object { $_.Status -eq "warn" }).Count
            $kind = "ok"; $label = "all good"
            if ($warn -gt 0) { $kind = "warn"; $label = "$warn to look at" }
            if ($bad -gt 0) { $kind = "bad"; $label = "$bad missing" }
            if (@($sec.Rows).Count -eq 0) { $kind = "info"; $label = "nothing to check" }
            $card = New-Card $sec.Title $sec.Blurb $kind $label
            if ($sec.Guide -or $sec.Url) { [void]$card.Body.Children.Add((New-GuideRow $sec)) }
            $first = $true
            foreach ($row in $sec.Rows) {
                [void]$card.Body.Children.Add((New-CheckRow $row $first))
                $first = $false
            }

            [void]$ui.InstallHost.Children.Add($card.Card)
        }
        $ui.Status.Text = "checked at " + (Get-Date -Format "HH:mm:ss") + "  -  file list: tools\install_manifest.json"
    }.GetNewClosure()

    # Invoke-InstallChecks is about a third of a second of file reads and log parsing, and the cards on top of that.
    # Run synchronously it holds the click, so the tab looks like it is refusing to open. Put a placeholder up, let
    # WPF paint, and do the work at Background priority once the frame is on screen.
    $showInstall = {
        $ui.InstallHost.Children.Clear()
        $c = New-Card "Setup" "Reading the files, the settings and the last run..." "info" "checking"
        [void]$ui.InstallHost.Children.Add($c.Card)
        $ui.Status.Text = "checking..."
        [void]$win.Dispatcher.BeginInvoke([System.Windows.Threading.DispatcherPriority]::Background, [action]$buildInstall)
    }.GetNewClosure()

    $showView = {
        $tab = "play"
        if ($ui.NavSettings.IsChecked) { $tab = "settings" }
        elseif ($ui.NavInstall.IsChecked) { $tab = "install" }
        $vis = { param($on) if ($on) { "Visible" } else { "Collapsed" } }
        $ui.PlayView.Visibility = & $vis ($tab -eq "play")
        $ui.SettingsView.Visibility = & $vis ($tab -eq "settings")
        $ui.InstallView.Visibility = & $vis ($tab -eq "install")
        $ui.SaveBtn.Visibility = & $vis ($tab -eq "settings")
        $ui.ReloadBtn.Visibility = & $vis ($tab -eq "settings")
        $ui.CopyBtn.Visibility = & $vis ($tab -eq "install")
        $ui.RecheckBtn.Visibility = & $vis ($tab -eq "install")
        $ui.Caption.Text = switch ($tab) { "settings" { "add-on settings" } "install" { "setup and install check" } default { "start a scene" } }
        if ($tab -eq "settings") { & $buildSettings }
        if ($tab -eq "install") { & $showInstall }
        & $refreshState
    }.GetNewClosure()
    $ui.NavPlay.Add_Checked($showView)
    $ui.NavSettings.Add_Checked($showView)
    $ui.NavInstall.Add_Checked($showView)

    # Repointing the game folder from the Setup tab. Reached through $state so New-GameDirCard's button can call it
    # without this having to exist before the card builder does.
    $state.ApplyGameDir = {
        param($dir, $persist = $true)
        if ($dir) {
            $dir = $dir.TrimEnd('\')
            if (Test-Mgs4Path (Join-Mgs4Path $dir "MGS4\mgs4.exe")) { $dir = Join-Mgs4Path $dir "MGS4" }
            if (-not (Test-Mgs4Path (Join-Mgs4Path $dir "mgs4.exe"))) {
                $ui.Status.Text = "no mgs4.exe in $dir"
                return
            }
        }
        $state.GameDir = $dir
        $opt.GameDir = $dir
        $opt.GameDirBad = ""
        if ($persist) {
            try {
                $written = Set-ConfiguredGameDir $dir
                $ui.Status.Text = "game folder saved to $written"
            } catch {
                $ui.Status.Text = "could not write config.ini: $($_.Exception.Message)"
            }
        } else {
            $ui.Status.Text = $(if ($dir) { "found $dir - left to auto-detection" } else { "no MGS4 install found" })
        }
        Set-WindowIcon $win $dir
        & $refreshGamePath
        & $applyFilter
        & $showInstall
    }.GetNewClosure()

    # Files dropped on the Setup tab are put where the manifest says they go, then everything is checked again.
    $ui.InstallView.Add_DragOver({
        param($sender, $e)
        $e.Effects = $(if ($e.Data.GetDataPresent([System.Windows.DataFormats]::FileDrop)) {
            [System.Windows.DragDropEffects]::Copy } else { [System.Windows.DragDropEffects]::None })
        $e.Handled = $true
    })
    $ui.InstallView.Add_Drop({
        param($sender, $e)
        $e.Handled = $true
        if (-not $e.Data.GetDataPresent([System.Windows.DataFormats]::FileDrop)) { return }
        $paths = @($e.Data.GetData([System.Windows.DataFormats]::FileDrop))
        if (-not $paths.Count) { return }
        try {
            $lines = Copy-DroppedFiles $state.Sections $state.GameDir $paths
            $ui.Status.Text = ($lines -join "   |   ")
        } catch {
            $ui.Status.Text = "drop failed: $($_.Exception.Message)"
        }
        & $showInstall
    }.GetNewClosure())

    $ui.RecheckBtn.Add_Click($showInstall)
    $ui.CopyBtn.Add_Click({
        if (-not $state.Sections) { return }
        Set-Clipboard -Value (Format-TextReport $state.GameDir $state.Sections)
        $this.Content = "Copied"
        $t = New-Object System.Windows.Threading.DispatcherTimer
        $t.Interval = [TimeSpan]::FromSeconds(1.6)
        $b = $this
        $t.Add_Tick({ $b.Content = "Copy report"; $t.Stop() })
        $t.Start()
    }.GetNewClosure())

    # ------------------------------------------------------------------ restore and go
    $prefs = Read-Prefs
    $ui.OptAdvance.IsChecked = $true
    if ($prefs) {
        if ($prefs.Filters) {
            foreach ($chip in $ui.Filters.Children) { $chip.IsChecked = (@($prefs.Filters) -contains "$($chip.Tag)") }
        }
        $ui.OptAdvance.IsChecked = [bool]$prefs.Advance
        $ui.OptMashX.IsChecked = [bool]$prefs.MashX
        $ui.OptEnd.IsChecked = [bool]$prefs.EndOnGameplay
        $ui.OptHold.IsChecked = [bool]$prefs.Hold
        $ui.OptRes.IsChecked = [bool]$prefs.Res
        if ($prefs.HoldSecs) { $ui.HoldSecs.Text = $prefs.HoldSecs }
        if ($prefs.ResW) { $ui.ResW.Text = $prefs.ResW }
        if ($prefs.ResH) { $ui.ResH.Text = $prefs.ResH }
    }
    # Every act starts collapsed, so the window opens as a short list of acts rather than 400 rows.
    foreach ($k in $actOrder) { $state.Collapsed[$k] = $true }
    $want = $opt.Stage
    if (-not $want -and $prefs) { $want = $prefs.Stage }
    if ($want) {
        # A scene asked for on the command line, or the last one used, opens its act and is selected in it.
        $entry = Find-Scene $want
        if ($entry) {
            $state.Collapsed[$entry.ActKey] = $false
            $state.PickedId = $entry.Id
            foreach ($chip in $ui.Filters.Children) { $chip.IsChecked = $false }   # so the scene is in view
        }
    }
    & $applyFilter
    if ($state.PickedId) {
        $hit = @($ui.SceneList.ItemsSource | Where-Object { -not $_.IsHeader -and $_.Id -eq $state.PickedId }) | Select-Object -First 1
        if ($hit) { $ui.SceneList.SelectedItem = $hit; $ui.SceneList.ScrollIntoView($hit) }
    }
    & $refreshPreview
    & $refreshState

    if ($startTab -eq "install") { $ui.NavInstall.IsChecked = $true }
    elseif ($startTab -eq "settings") { $ui.NavSettings.IsChecked = $true }

    $timer = New-Object System.Windows.Threading.DispatcherTimer
    $timer.Interval = [TimeSpan]::FromSeconds(1.5)
    $timer.Add_Tick($refreshState)
    $timer.Start()
    $win.Add_Closed({ $timer.Stop() }.GetNewClosure())
    $win.Add_KeyDown({
        if ($_.Key -eq [System.Windows.Input.Key]::F5) {
            if ($ui.NavSettings.IsChecked) { & $buildSettings }
            elseif ($ui.NavInstall.IsChecked) { & $showInstall }
            else { & $applyFilter }
        }
    }.GetNewClosure())

    [void]$win.ShowDialog()
}

# ---------------------------------------------------------------------------------------------- dispatch

try { $opt = Read-Options $args }
catch { Write-Host $_.Exception.Message -ForegroundColor Red; exit 2 }

if ($opt.Action -eq "help") { Write-Host $script:HelpText; exit 0 }

$gameDir = $opt.GameDir
if (-not $gameDir) {
    try { $gameDir = Get-Mgs4GameDir } catch { $gameDir = $null }
}
# --game-dir can name a folder that holds no mgs4.exe; a non-empty string is not an install.
if ($gameDir -and -not (Test-Mgs4Path (Join-Mgs4Path $gameDir "mgs4.exe"))) {
    $opt.GameDirBad = $gameDir
    $gameDir = $null
}
$opt.GameDir = $gameDir

# The window opens whether or not there is an install: with no game folder the Install tab is the one thing that can
# still say something useful, so that is where it starts. Everything else needs the folder and says so.
$startTab = switch ($opt.Action) { "install" { "install" } "settings" { "settings" } default { "play" } }
if ($opt.Action -eq "" -and (Test-FirstRun)) { $startTab = "install" }
# --ui / --install always mean the window, with any scene named alongside them preselected in it. Only the
# argument-less form is "window because nothing else was asked for".
$wantsWindow = ($opt.Action -in @("ui", "install")) -or ($opt.Action -eq "" -and -not $opt.Stage)
if ($opt.Action -eq "settings" -and -not $gameDir) { $wantsWindow = $false }
if (-not $gameDir -and $wantsWindow) { $startTab = "install" }

if (-not $gameDir -and -not $wantsWindow -and $opt.Action -ne "list") {
    Write-Host "mgs4.exe was not found. Set MGS4_DIR in config.ini or pass --game-dir ""<path to MGS4>""." -ForegroundColor Red
    exit 1
}

switch ($opt.Action) {
    "list"      { Write-SceneList $opt.Filter; exit 0 }
    "report"    { Write-Host (Format-TextReport $gameDir (Invoke-InstallChecks -Game $gameDir)); exit 0 }
    "settings"  { Write-SettingsReport $gameDir; exit 0 }
    "set"       { exit (Set-SettingsFromCli $gameDir $opt.Sets) }
    "stop"      { Stop-Game; Write-Host "closed mgs4.exe"; exit 0 }
    "shortcut"  {
        if (-not $opt.Stage) { Write-Host "--shortcut needs a scene: mgs4-dlss <id> --shortcut <file>" -ForegroundColor Red; exit 2 }
        $written = New-SceneShortcut $opt $opt.ShortcutPath
        Write-Host "shortcut written: $written"
        exit 0
    }
}

if ($wantsWindow) { Show-AppWindow $opt $gameDir $startTab; exit 0 }
if (-not $opt.Stage) { Show-AppWindow $opt $gameDir $startTab; exit 0 }
if (-not (Find-Scene $opt.Stage)) {
    Write-Host "unknown scene '$($opt.Stage)' - it is not in tools\scenes.csv. Launching it anyway; --list shows the known ones." -ForegroundColor Yellow
}
exit (Invoke-SceneRun $opt)
