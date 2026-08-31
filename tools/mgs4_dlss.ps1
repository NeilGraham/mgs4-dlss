# MGS4 DLSS: the app for this add-on. Start the game or any single scene in it, set the add-on up, and check the
# install - one window with three tabs, and the same things as a command line.
#
#   mgs4-dlss.bat                                the window
#   mgs4-dlss.bat s02a50l_D1                     boot that scene, no window
#   mgs4-dlss.bat --main                         straight to MGS4's main menu (past the collection screen)
#   mgs4-dlss.bat --list naomi                   what can be launched
#   mgs4-dlss.bat --install                      the window, on the install check
#   mgs4-dlss.bat --report                       the install check as text, for pasting into an issue
#   mgs4-dlss.bat --shortcuts                    rebuild the desktop shortcut folder
#   mgs4-dlss.bat --set FrameGen=0 --set Mode=Quality
#
# launcher.bat and check-install.bat are aliases that open the right tab; both spellings keep working.
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
        DesktopDir    = ""
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
        elseif ($a -match '^--shortcuts$')                { $o.Action = "shortcuts"; $took = $false }
        elseif ($a -match '^--(show-settings|settings)$') { $o.Action = "settings"; $took = $false }
        elseif ($a -match '^--(install|check|check-install)$') { $o.Action = "install"; $took = $false }
        elseif ($a -match '^--report$')                   { $o.Action = "report"; $took = $false }
        elseif ($a -match '^--stop$')                     { $o.Action = "stop"; $took = $false }
        elseif ($a -match '^--main$')                     { $o.Stage = "@main"; $took = $false }
        elseif ($a -match '^--title$')                    { $o.Stage = "@title"; $took = $false }
        elseif ($a -match '^--collection$')               { $o.Stage = "@collection"; $took = $false }
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
            elseif ($a -match '^--desktop-dir$')   { $o.DesktopDir = $value }
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

  mgs4-dlss.bat                         open the window (Play / Settings / Install)
  mgs4-dlss.bat <stage id>              boot that scene and exit
  mgs4-dlss.bat --main                  MGS4's own main menu, past the Master Collection screen
  mgs4-dlss.bat --title                 the OTC intro (stage s00title_1)
  mgs4-dlss.bat --collection            the Master Collection launcher
  mgs4-dlss.bat --list [text]           every launchable scene (filtered by id / name / act)
  mgs4-dlss.bat --install               the window, opened on the install check
  mgs4-dlss.bat --report                the install check as text, for pasting into an issue
  mgs4-dlss.bat --shortcuts             rebuild "Desktop\MGS4 Shortcuts" against this checkout
  mgs4-dlss.bat --settings              print mgs4_dlss.ini the way the window shows it
  mgs4-dlss.bat --set Key=Value [...]   write those keys into mgs4_dlss.ini
  mgs4-dlss.bat --stop                  close a running game

launcher.bat and check-install.bat are aliases for the Play and Install tabs.

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
    $list = New-Object System.Collections.Generic.List[object]
    $list.Add([pscustomobject]@{ Id = "@main"; Kind = "start"; Act = "Start the game"; Name = "Main menu"
                                Note = "MGS4's own menu, past the Master Collection screen (--skip-to-main-menu)" })
    $list.Add([pscustomobject]@{ Id = "@title"; Kind = "start"; Act = "Start the game"; Name = "Title / OTC intro"
                                Note = "the attract sequence the game boots into (stage s00title_1)" })
    $list.Add([pscustomobject]@{ Id = "@collection"; Kind = "start"; Act = "Start the game"; Name = "Master Collection launcher"
                                Note = "the Unity front-end, where the display settings live" })
    if (Test-Mgs4Path $script:ScenesCsv) {
        foreach ($row in (Import-Csv -LiteralPath $script:ScenesCsv)) {
            $id = $row.stage_entry
            $note = switch ($row.kind) {
                "cutscene" { "in-engine cutscene" }
                "gameplay" { "playable section" }
                default    { "boots the stage at its start" }
            }
            $list.Add([pscustomobject]@{ Id = $id; Kind = $row.kind; Act = $row.act
                                         Name = (Format-SceneName $labels[$id]); Note = $note })
        }
    }
    $script:Catalogue = $list
    return $list
}

function Find-Scene([string]$id) {
    if (-not $id) { return $null }
    return (Get-SceneCatalogue | Where-Object { $_.Id -eq $id } | Select-Object -First 1)
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
    elseif ($opt.Stage -eq "@title") { $cli += @("--stage", "s00title_1") }
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
        $rows = $rows | Where-Object { "$($_.Id) $($_.Name) $($_.Act) $($_.Kind)" -match [regex]::Escape($filter) }
    }
    $n = 0
    foreach ($r in $rows) {
        Write-Host ("{0,-16} {1,-12} {2,-24} {3}" -f $r.Id, $r.Kind, $r.Act, $(if ($r.Name) { $r.Name } else { $r.Note }))
        $n++
    }
    Write-Host ""
    Write-Host ("{0} {1}.  mgs4-dlss.bat <id>  boots one." -f $n, $(if ($n -eq 1) { "entry" } else { "entries" }))
}

function Write-SettingsReport($gameDir) {
    $ini = Join-Mgs4Path $gameDir "mgs4_dlss.ini"
    Write-Host "mgs4_dlss.ini: $ini"
    if (-not (Test-Mgs4Path $ini)) { Write-Host "  (not there - copy dlss-addon\mgs4_dlss.ini next to mgs4.exe)"; return }
    $group = ""
    foreach ($s in $script:IniSpec) {
        if ($s.Group -ne $group) { $group = $s.Group; Write-Host ""; Write-Host "[$group]" }
        $v = Get-IniValue $ini $s.Key
        if ($null -eq $v) { $v = "(unset)" }
        Write-Host ("  {0,-22} {1,-12} {2}" -f $s.Key, $v, $s.Label)
    }
    if (Test-GameRunning) {
        Write-Host ""
        Write-Host "The game is running: it owns this file, so leave the writing to the add-on until it exits."
    }
}

function Set-SettingsFromCli($gameDir, $sets) {
    $ini = Join-Mgs4Path $gameDir "mgs4_dlss.ini"
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

function Get-ShortcutName($scene) {
    $name = $scene.Id
    if ($scene.Act -and $scene.Kind -ne "start") { $name += " - " + $scene.Act }
    if ($scene.Name) { $name += " - " + $scene.Name }
    foreach ($c in [IO.Path]::GetInvalidFileNameChars()) { $name = $name.Replace($c, '-') }
    return $name
}

$script:ShortcutReadme = @'
MGS4 scene shortcuts
====================

Every launchable scene in Metal Gear Solid 4 (Master Collection Vol. 2), one shortcut each, plus the two ways of
starting the game itself. Each shortcut runs tools\launcher.ps1 out of the mgs4-dlss checkout named below: it starts
the game with --stage <id>, brings the window to the foreground and presses through the auto-save / "press any
button" prompts until the first 3D frame. The DLSS add-on is active as usual, so the scene boots with DLAA /
DLSS 5 Neural Rendering / frame generation exactly as mgs4_dlss.ini has it.

Folders
-------
  cutscene       the in-engine cutscenes (stage ids ending in _D<n>)
  gameplay       the playable segments
  stage entry    the bare stage ids: each act's stage from its own beginning
  notable        duplicates of the scenes that have a curated name

Names are "<stage id> - <act> - <scene name>", so each folder sorts in story order. Scene names come from
tools\labels.json; scenes without one show just the id and the act.

Notes
-----
- The game is restarted by the shortcut, so anything already running is closed first.
- A scene may need a few seconds of black screen while the stage loads.
- These shortcuts are generated: re-run "mgs4-dlss.bat --shortcuts" after moving the checkout, and they will point at
  the new place. That is the whole reason a shortcut can stop working - it holds an absolute path.
- More options (keep tapping X for the flashback prompts, close the game when gameplay starts) are in the launcher
  window: launcher.bat
'@

function New-Shortcuts($opt) {
    $root = $opt.DesktopDir
    if (-not $root) { $root = Join-Path ([Environment]::GetFolderPath("Desktop")) "MGS4 Shortcuts" }
    $scriptPath = Join-Path $PSScriptRoot "mgs4_dlss.ps1"
    $gameDir = $opt.GameDir
    if (-not $gameDir) { $gameDir = Get-Mgs4GameDir }

    $shell = New-Object -ComObject WScript.Shell
    $folders = @{ "cutscene" = "cutscene"; "gameplay" = "gameplay"; "stage-entry" = "stage entry"; "start" = "" }
    $removed = 0
    # "greatest" is what the folder used to be called; its shortcuts point at a path that no longer exists.
    foreach ($f in (@($folders.Values | Where-Object { $_ }) + @("notable", "greatest", ""))) {
        $dir = if ($f) { Join-Path $root $f } else { $root }
        if (Test-Mgs4Path $dir) {
            $old = @(Get-ChildItem -LiteralPath $dir -Filter *.lnk -File -ErrorAction SilentlyContinue)
            $removed += $old.Count
            $old | Remove-Item -Force -ErrorAction SilentlyContinue
        }
        if ($f -eq "greatest") {
            if ((Test-Mgs4Path $dir) -and -not (Get-ChildItem -LiteralPath $dir -Force)) { Remove-Item -LiteralPath $dir -Force }
            continue
        }
        New-Item -ItemType Directory -Force $dir | Out-Null
    }

    $made = 0
    foreach ($scene in (Get-SceneCatalogue)) {
        $sub = $folders[$scene.Kind]
        $dirs = @($(if ($sub) { Join-Path $root $sub } else { $root }))
        if ($scene.Name -and $scene.Kind -ne "start") { $dirs += (Join-Path $root "notable") }
        foreach ($dir in $dirs) {
            $lnk = $shell.CreateShortcut((Join-Path $dir ((Get-ShortcutName $scene) + ".lnk")))
            $lnk.TargetPath = Join-Path $env:SystemRoot "System32\WindowsPowerShell\v1.0\powershell.exe"
            $lnk.Arguments = '-NoProfile -ExecutionPolicy Bypass -WindowStyle Minimized -File "' + $scriptPath + '" ' + $scene.Id
            $lnk.WorkingDirectory = $gameDir
            $lnk.IconLocation = (Join-Mgs4Path $gameDir "mgs4.exe") + ",0"
            $lnk.Description = "$($scene.Id) - $($scene.Note)"
            $lnk.Save()
            $made++
        }
    }
    Set-Content -LiteralPath (Join-Path $root "README.txt") -Encoding UTF8 `
        -Value ($script:ShortcutReadme + "`r`nGenerated " + (Get-Date -Format "yyyy-MM-dd HH:mm") + " from " + $scriptPath + "`r`n")
    return @{ Root = $root; Made = $made; Removed = $removed }
}

# ---------------------------------------------------------------------------------------------- the window

# The window never runs a scene on its own thread - it starts this same script again with the equivalent command
# line, so the UI stays responsive and every route into the game (window, shortcut, test script) is one code path.
function Get-CliArgs($opt) {
    $a = @()
    if ($opt.Stage -eq "@main") { $a += "--main" }
    elseif ($opt.Stage -eq "@title") { $a += "--title" }
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
    return "mgs4-dlss.bat " + (($cliArgs | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join " ")
}

function Read-Prefs {
    if (Test-Mgs4Path $script:PrefsPath) {
        try { return (Get-Content -LiteralPath $script:PrefsPath -Raw -Encoding UTF8 | ConvertFrom-Json) } catch {}
    }
    return $null
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
    <Style TargetType="TextBox">
      <Setter Property="Foreground" Value="#E7EAF0"/>
      <Setter Property="CaretBrush" Value="#E7EAF0"/>
      <Setter Property="Background" Value="#12151D"/>
      <Setter Property="BorderBrush" Value="#333A4D"/>
      <Setter Property="FontSize" Value="12"/>
      <Setter Property="Padding" Value="8,5"/>
      <Setter Property="Template">
        <Setter.Value>
          <ControlTemplate TargetType="TextBox">
            <Border CornerRadius="6" Background="{TemplateBinding Background}" BorderBrush="{TemplateBinding BorderBrush}"
                    BorderThickness="1" Padding="{TemplateBinding Padding}">
              <ScrollViewer x:Name="PART_ContentHost" VerticalAlignment="Center"/>
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
          <RadioButton x:Name="NavInstall" Style="{StaticResource Nav}" Content="Install" GroupName="nav"/>
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
            <Grid>
              <Grid.ColumnDefinitions>
                <ColumnDefinition Width="*"/>
                <ColumnDefinition Width="Auto"/>
              </Grid.ColumnDefinitions>
              <Grid Grid.Column="0" Margin="0,0,10,0">
                <TextBox x:Name="Search" Height="30"/>
                <TextBlock x:Name="SearchHint" Text="Search by scene name, stage id or act" FontSize="12"
                           Foreground="#5C6478" IsHitTestVisible="False" VerticalAlignment="Center" Margin="10,0,0,0"/>
              </Grid>
              <ComboBox x:Name="KindFilter" Grid.Column="1" Width="170" Height="30"/>
            </Grid>
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
            </StackPanel>
          </Border>
        </Grid>
      </Border>
    </Grid>

    <ScrollViewer x:Name="InstallView" Grid.Row="1" Margin="22,16,10,0" VerticalScrollBarVisibility="Auto"
                  Padding="0,0,12,0" Visibility="Collapsed">
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
          <Button x:Name="ShortcutBtn" Content="Desktop shortcuts" Style="{StaticResource Flat}" Margin="0,0,10,0"/>
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
    <Grid.ColumnDefinitions>
      <ColumnDefinition Width="135"/>
      <ColumnDefinition Width="*"/>
    </Grid.ColumnDefinitions>
    <TextBlock Grid.Column="0" Text="{Binding Id}" FontFamily="Consolas" FontSize="12" Foreground="#7C9CFF"
               VerticalAlignment="Center"/>
    <StackPanel Grid.Column="1">
      <TextBlock Text="{Binding Title}" FontSize="13" Foreground="#E7EAF0" TextTrimming="CharacterEllipsis"/>
      <TextBlock Text="{Binding Sub}" FontSize="11" Foreground="#858D9E" Margin="0,1,0,0" TextTrimming="CharacterEllipsis"/>
    </StackPanel>
  </Grid>
</DataTemplate>
'@

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
        $tag.CornerRadius = New-Object System.Windows.CornerRadius 20
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

# One row of the install check: glyph, name + detail, the value found, and a link to where a missing one comes from.
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

function Show-AppWindow($opt, $gameDir, $startTab) {
    Add-Type -AssemblyName PresentationFramework, PresentationCore, WindowsBase

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
                     "Search", "SearchHint", "KindFilter", "SceneList", "PickTitle", "PickSub", "OptAdvance", "OptMashX", "MashNote",
                     "OptEnd", "OptHold", "HoldSecs", "OptRes", "ResW", "ResH", "CmdPreview", "LaunchBtn", "StopBtn",
                     "Status", "ShortcutBtn", "ReloadBtn", "SaveBtn", "SettingsHost", "LockBanner", "LockText")) {
        $ui[$n] = $win.FindName($n)
    }
    $script:LinkStyle = $win.FindResource("Link")
    $ui.GamePath.Text = $(if ($gameDir) { $gameDir }
                          elseif ($opt.GameDirBad) { "no mgs4.exe in $($opt.GameDirBad) - see the Install tab" }
                          else { "no MGS4 install found - see the Install tab" })
    $ui.GamePath.ToolTip = $ui.GamePath.Text
    $ui.SceneList.ItemTemplate = [Windows.Markup.XamlReader]::Parse($script:ItemTemplateXaml)

    $ini = Join-Mgs4Path $gameDir "mgs4_dlss.ini"
    $pad = Open-Pad
    [Mgs4Pad]::Close()
    $ui.MashNote.Text = if ($pad.Ok) {
        "A virtual DualShock 4 taps Cross about six times a second, which is what MGS4's in-cutscene flashback prompts want. The game must stay in the foreground."
    } else {
        "Needs ViGEmBus and ViGEmClient.dll ($($pad.Why)). Without them the launcher can only press Enter, which gets past the prompts but does not fire the flashbacks."
    }

    # ------------------------------------------------------------------ the scene list
    $all = @(Get-SceneCatalogue | ForEach-Object {
        [pscustomobject]@{
            Id = $_.Id; Kind = $_.Kind; Act = $_.Act; Name = $_.Name; Note = $_.Note
            Title = $(if ($_.Name) { $_.Name } else { $_.Id })
            Sub = $(if ($_.Kind -eq "start") { $_.Note } else { "$($_.Act)  -  $($_.Note)" })
            Hay = "$($_.Id) $($_.Name) $($_.Act) $($_.Kind)".ToLower()
        }
    })
    $kinds = [ordered]@{
        "Everything"      = { $true }
        "Start the game"  = { $_.Kind -eq "start" }
        "Cutscenes"       = { $_.Kind -eq "cutscene" }
        "Named scenes"    = { $_.Name -ne "" }
        "Gameplay"        = { $_.Kind -eq "gameplay" }
        "Stage entries"   = { $_.Kind -eq "stage-entry" }
    }
    foreach ($k in $kinds.Keys) { [void]$ui.KindFilter.Items.Add($k) }
    $ui.KindFilter.SelectedIndex = 0

    $applyFilter = {
        $q = $ui.Search.Text.Trim().ToLower()
        $ui.SearchHint.Visibility = $(if ($ui.Search.Text) { "Collapsed" } else { "Visible" })
        $pick = "$($ui.KindFilter.SelectedItem)"
        $test = $kinds[$pick]
        $rows = @($all | Where-Object $test)
        if ($q) { $rows = @($rows | Where-Object { $_.Hay.Contains($q) }) }
        $keep = $ui.SceneList.SelectedItem
        $ui.SceneList.ItemsSource = $rows
        if ($keep -and $rows -contains $keep) { $ui.SceneList.SelectedItem = $keep }
        $ui.Status.Text = "$($rows.Count) of $($all.Count) entries"
    }.GetNewClosure()

    # ------------------------------------------------------------------ options <-> command line
    $collect = {
        $o = New-Options
        $o.GameDir = $opt.GameDir
        $o.GameDirGiven = $opt.GameDirGiven
        $sel = $ui.SceneList.SelectedItem
        $o.Stage = $(if ($sel) { $sel.Id } else { "" })
        $o.Advance = [bool]$ui.OptAdvance.IsChecked
        $o.MashX = [bool]$ui.OptMashX.IsChecked
        $o.EndOnGameplay = [bool]$ui.OptEnd.IsChecked
        if ($ui.OptHold.IsChecked) { $o.Hold = [double]($ui.HoldSecs.Text -replace '[^\d.]', '') }
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
            $ui.LaunchBtn.IsEnabled = [bool]$gameDir
            $ui.CmdPreview.Text = Format-CliPreview (Get-CliArgs (& $collect))
        } else {
            $ui.PickTitle.Text = "Nothing picked"
            $ui.PickSub.Text = "Choose a scene on the left."
            $ui.LaunchBtn.IsEnabled = $false
            $ui.CmdPreview.Text = "launcher.bat --list"
        }
        Save-Prefs ([pscustomobject]@{
            Stage = $(if ($sel) { $sel.Id } else { "" }); Kind = "$($ui.KindFilter.SelectedItem)"
            Advance = [bool]$ui.OptAdvance.IsChecked; MashX = [bool]$ui.OptMashX.IsChecked
            EndOnGameplay = [bool]$ui.OptEnd.IsChecked; Hold = [bool]$ui.OptHold.IsChecked
            HoldSecs = $ui.HoldSecs.Text; Res = [bool]$ui.OptRes.IsChecked
            ResW = $ui.ResW.Text; ResH = $ui.ResH.Text
        })
    }.GetNewClosure()

    # ------------------------------------------------------------------ settings
    # Every scriptblock below is closed with GetNewClosure(), which binds it to its own dynamic module - so
    # $script:... inside one of them is NOT this script's scope. Anything shared goes through these locals.
    $spec = $script:IniSpec
    $selfPath = $PSCommandPath          # $PSCommandPath is per-scope too, and would be empty inside a closure
    $state = @{ Controls = @(); RunProc = $null; Sections = $null }

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
                $cur = Get-IniValue $ini $s.Key
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
            Set-Ini $ini $vals
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
        $ui.SaveBtn.IsEnabled = (-not $running) -and [bool]$gameDir
            $ui.LaunchBtn.IsEnabled = [bool]$gameDir -and [bool]$ui.SceneList.SelectedItem
        if ($ui.SettingsView.Visibility -eq [System.Windows.Visibility]::Visible) {
            if (-not $gameDir) {
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
    $ui.KindFilter.Add_SelectionChanged($applyFilter)
    $ui.SceneList.Add_SelectionChanged($refreshPreview)
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
        $root = Join-Path ([Environment]::GetFolderPath("Desktop")) "MGS4 Shortcuts"
        $answer = [System.Windows.MessageBox]::Show(
            "Rebuild the shortcut folder?`n`n$root`n`nEvery .lnk in it is replaced by one per scene, pointing at this checkout. Nothing else in the folder is touched.",
            "MGS4 DLSS", [System.Windows.MessageBoxButton]::OKCancel, [System.Windows.MessageBoxImage]::Question)
        if ($answer -ne [System.Windows.MessageBoxResult]::OK) { return }
        $this.IsEnabled = $false
        $ui.Status.Text = "writing shortcuts..."
        try {
            $r = New-Shortcuts $opt
            $ui.Status.Text = "$($r.Made) shortcuts in $($r.Root) (replaced $($r.Removed))"
            Start-Process $r.Root
        } catch {
            $ui.Status.Text = "shortcuts failed: $($_.Exception.Message)"
        }
        $this.IsEnabled = $true
    }.GetNewClosure())

    $ui.SaveBtn.Add_Click($saveSettings)
    $ui.ReloadBtn.Add_Click({ & $buildSettings; $ui.Status.Text = "reloaded mgs4_dlss.ini" }.GetNewClosure())

    # ------------------------------------------------------------------ install check
    $buildInstall = {
        $ui.InstallHost.Children.Clear()
        if (-not $gameDir) {
            $why = "The Steam libraries were searched for app 2492670 and no mgs4.exe turned up."
            if ($opt.GameDirBad) { $why = "There is no mgs4.exe in $($opt.GameDirBad)." }
            $c = New-Card "No MGS4 install found" ($why + " Set MGS4_DIR in config.ini (copy config.example.ini), " +
                 "or start this with --game-dir ""<path to the MGS4 folder>"".") "bad" "nothing to check"
            [void]$ui.InstallHost.Children.Add($c.Card)
            return
        }
        $sections = Invoke-InstallChecks -Game $gameDir
        $state.Sections = $sections
        $v = Get-Verdict $sections
        $vc = New-Card ("Install: " + $v.Text) $v.Note $v.Kind $v.Text
        [void]$ui.InstallHost.Children.Add($vc.Card)
        foreach ($sec in $sections) {
            $bad = @($sec.Rows | Where-Object { $_.Status -eq "bad" }).Count
            $warn = @($sec.Rows | Where-Object { $_.Status -eq "warn" }).Count
            $kind = "ok"; $label = "all good"
            if ($warn -gt 0) { $kind = "warn"; $label = "$warn to look at" }
            if ($bad -gt 0) { $kind = "bad"; $label = "$bad missing" }
            if (@($sec.Rows).Count -eq 0) { $kind = "info"; $label = "nothing to check" }
            $card = New-Card $sec.Title $sec.Blurb $kind $label
            $first = $true
            foreach ($row in $sec.Rows) {
                [void]$card.Body.Children.Add((New-CheckRow $row $first))
                $first = $false
            }
            [void]$ui.InstallHost.Children.Add($card.Card)
        }
        $ui.Status.Text = "checked at " + (Get-Date -Format "HH:mm:ss") + "  -  file list: tools\install_manifest.json"
    }.GetNewClosure()

    $showView = {
        $tab = "play"
        if ($ui.NavSettings.IsChecked) { $tab = "settings" }
        elseif ($ui.NavInstall.IsChecked) { $tab = "install" }
        $vis = { param($on) if ($on) { "Visible" } else { "Collapsed" } }
        $ui.PlayView.Visibility = & $vis ($tab -eq "play")
        $ui.SettingsView.Visibility = & $vis ($tab -eq "settings")
        $ui.InstallView.Visibility = & $vis ($tab -eq "install")
        $ui.ShortcutBtn.Visibility = & $vis ($tab -eq "play")
        $ui.SaveBtn.Visibility = & $vis ($tab -eq "settings")
        $ui.ReloadBtn.Visibility = & $vis ($tab -eq "settings")
        $ui.CopyBtn.Visibility = & $vis ($tab -eq "install")
        $ui.RecheckBtn.Visibility = & $vis ($tab -eq "install")
        $ui.Caption.Text = switch ($tab) { "settings" { "add-on settings" } "install" { "install check" } default { "start a scene" } }
        if ($tab -eq "settings") { & $buildSettings }
        if ($tab -eq "install") { & $buildInstall }
        & $refreshState
    }.GetNewClosure()
    $ui.NavPlay.Add_Checked($showView)
    $ui.NavSettings.Add_Checked($showView)
    $ui.NavInstall.Add_Checked($showView)

    $ui.RecheckBtn.Add_Click($buildInstall)
    $ui.CopyBtn.Add_Click({
        if (-not $state.Sections) { return }
        Set-Clipboard -Value (Format-TextReport $gameDir $state.Sections)
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
        if ($prefs.Kind -and $ui.KindFilter.Items.Contains($prefs.Kind)) { $ui.KindFilter.SelectedItem = $prefs.Kind }
        $ui.OptAdvance.IsChecked = [bool]$prefs.Advance
        $ui.OptMashX.IsChecked = [bool]$prefs.MashX
        $ui.OptEnd.IsChecked = [bool]$prefs.EndOnGameplay
        $ui.OptHold.IsChecked = [bool]$prefs.Hold
        $ui.OptRes.IsChecked = [bool]$prefs.Res
        if ($prefs.HoldSecs) { $ui.HoldSecs.Text = $prefs.HoldSecs }
        if ($prefs.ResW) { $ui.ResW.Text = $prefs.ResW }
        if ($prefs.ResH) { $ui.ResH.Text = $prefs.ResH }
    }
    & $applyFilter
    $want = $opt.Stage
    if (-not $want -and $prefs) { $want = $prefs.Stage }
    if ($want) {
        $hit = @($ui.SceneList.ItemsSource | Where-Object { $_.Id -eq $want }) | Select-Object -First 1
        if (-not $hit) {
            $ui.KindFilter.SelectedIndex = 0
            & $applyFilter
            $hit = @($ui.SceneList.ItemsSource | Where-Object { $_.Id -eq $want }) | Select-Object -First 1
        }
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
            elseif ($ui.NavInstall.IsChecked) { & $buildInstall }
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
$wantsWindow = ($opt.Action -in @("", "ui", "install")) -and -not $opt.Stage
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
    "shortcuts" {
        $r = New-Shortcuts $opt
        Write-Host "$($r.Made) shortcuts written to $($r.Root) (replaced $($r.Removed))"
        exit 0
    }
}

if ($wantsWindow) { Show-AppWindow $opt $gameDir $startTab; exit 0 }
if (-not $opt.Stage) { Show-AppWindow $opt $gameDir $startTab; exit 0 }
if (-not (Find-Scene $opt.Stage)) {
    Write-Host "unknown scene '$($opt.Stage)' - it is not in tools\scenes.csv. Launching it anyway; --list shows the known ones." -ForegroundColor Yellow
}
exit (Invoke-SceneRun $opt)
