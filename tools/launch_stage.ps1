# Boots MGS4 straight into a stage and presses keys to set the scene up. Default: once the window is up, tap Enter
# every 0.5 s until the add-on log reports the first 3D frame (auto-save notice and "press any button" are gone and
# the cutscene has started), for at most PressFor seconds.
#   powershell -ExecutionPolicy Bypass -File tools\launch_stage.ps1 -Stage s00a00l -WaitSeconds 40 -Keys "5,ENTER,4,ENTER"
# Keys: comma-separated key names (E, SPACE, ENTER, ESC, F1..) and numbers (seconds to wait).
# Keys are sent with SendInput, which only reaches the FOREGROUND window, so the game window is forced to the
# foreground first (AttachThreadInput + Alt trick to satisfy Windows' foreground lock) and verified before each key.
# Log: <GameDir>\logs\launch_stage.log
param(
    [string]$Stage = "s00a00l",
    [int]$WaitSeconds = 0,          # 0 = just wait for the game window to appear (+ SettleSeconds)
    [double]$SettleSeconds = 2,
    [string]$Keys = "",             # explicit sequence, e.g. "5,ENTER,4,ENTER"; empty = repeated presses below
    [string]$PressKey = "ENTER",
    [double]$PressEvery = 0.5,
    [double]$PressFor = 60,
    [switch]$NoSceneDetect,        # keep pressing for the full PressFor instead of stopping at the first 3D frame
    [string]$GameDir = "C:\Program Files (x86)\Steam\steamapps\common\METAL GEAR SOLID 4\MGS4",
    [switch]$NoRestart
)
$ErrorActionPreference = "Continue"
$log = Join-Path $GameDir "logs\launch_stage.log"
function Log($m) { $line = "[{0:HH:mm:ss.fff}] {1}" -f (Get-Date), $m; Write-Host $line; Add-Content -Path $log -Value $line -Encoding ASCII }
Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices;
public static class Inp {
  [StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT { public ushort wVk; public ushort wScan; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
  [StructLayout(LayoutKind.Explicit)] public struct INPUTUNION { [FieldOffset(0)] public KEYBDINPUT ki; [FieldOffset(0)] public long pad1; [FieldOffset(8)] public long pad2; [FieldOffset(16)] public long pad3; }
  [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public INPUTUNION u; }
  [DllImport("user32.dll", SetLastError=true)] public static extern uint SendInput(uint n, INPUT[] inputs, int size);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr pid);
  [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint mapType);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
  // keybd_event (virtual key + scan code, no KEYEVENTF_SCANCODE) is what this game's input path actually reacts to;
  // scan-code-only SendInput events were ignored by it.
  public static void Key(ushort vk, bool down) {
    keybd_event((byte)vk, (byte)MapVirtualKey(vk, 0), (uint)(down ? 0 : 2), UIntPtr.Zero);
  }
  public static bool Focus(IntPtr h) {
    if (GetForegroundWindow() == h) return true;
    keybd_event(0x12, 0, 0, UIntPtr.Zero); keybd_event(0x12, 0, 2, UIntPtr.Zero);   // Alt tap: makes the OS allow a foreground change
    uint fg = GetWindowThreadProcessId(GetForegroundWindow(), IntPtr.Zero), me = GetCurrentThreadId();
    if (fg != me) AttachThreadInput(me, fg, true);
    ShowWindow(h, 9); BringWindowToTop(h); SetForegroundWindow(h);
    if (fg != me) AttachThreadInput(me, fg, false);
    System.Threading.Thread.Sleep(300);
    return GetForegroundWindow() == h;
  }
}
"@
function VK([string]$name) {
    switch ($name.ToUpper()) { "SPACE" { 0x20 } "ENTER" { 0x0D } "ESC" { 0x1B } "TAB" { 0x09 } "UP" { 0x26 } "DOWN" { 0x28 } "LEFT" { 0x25 } "RIGHT" { 0x27 }
        default { if ($name -match '^F(\d+)$') { 0x6F + [int]$Matches[1] } elseif ($name.Length -eq 1) { [int][char]$name.ToUpper() } else { throw "unknown key $name" } } }
}
if (-not $NoRestart) {
    Stop-Process -Name mgs4,launcher -Force -ErrorAction SilentlyContinue; Start-Sleep 4
    Start-Process -FilePath "$GameDir\mgs4.exe" -ArgumentList "--stage $Stage" -WorkingDirectory $GameDir | Out-Null
    Log "launched --stage $Stage"
    if ($WaitSeconds -gt 0) { Start-Sleep $WaitSeconds }
}
$pr = $null
for ($i = 0; $i -lt 120 -and -not $pr; $i++) { $pr = Get-Process mgs4 -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1; if (-not $pr) { Start-Sleep 1 } }
if (-not $pr) { Log "no game window"; exit 1 }
$hw = $pr.MainWindowHandle
if ($WaitSeconds -le 0 -and $SettleSeconds -gt 0) { Log "window up, settling $SettleSeconds s"; Start-Sleep $SettleSeconds }
Log ("game window {0} '{1}', foreground now {2}" -f $hw, $pr.MainWindowTitle, [Inp]::GetForegroundWindow())
$addonLog = Join-Path $GameDir "logs\mgs4_dlss.log"
function SceneStarted { (Test-Path $addonLog) -and ((Select-String -Path $addonLog -SimpleMatch "tracing backbuffer draws" -Quiet) -eq $true) }
if ($Keys -eq "") {
    $vk = [uint16](VK $PressKey); $deadline = (Get-Date).AddSeconds($PressFor); $n = 0
    Log "pressing $PressKey every $PressEvery s until the first 3D frame (max $PressFor s)"
    while ((Get-Date) -lt $deadline) {
        if (-not $NoSceneDetect -and (SceneStarted)) { Log "3D scene detected after $n presses - stopping"; break }
        $ok = $false; for ($a = 0; $a -lt 3 -and -not $ok; $a++) { $ok = [Inp]::Focus($hw); if (-not $ok) { Start-Sleep -Milliseconds 300 } }
        [Inp]::Key($vk, $true); Start-Sleep -Milliseconds 120; [Inp]::Key($vk, $false); $n++
        Start-Sleep $PressEvery
    }
    Log "done ($n presses)"; exit 0
}
foreach ($tok in $Keys.Split(',')) {
    $t = $tok.Trim(); if ($t -eq "") { continue }
    if ($t -match '^\d+(\.\d+)?$') { Log "wait $t s"; Start-Sleep ([double]$t); continue }
    $vk = [uint16](VK $t)
    $ok = $false
    for ($a = 0; $a -lt 5 -and -not $ok; $a++) { $ok = [Inp]::Focus($hw); if (-not $ok) { Start-Sleep -Milliseconds 400 } }
    Log ("press {0} (focus {1}, foreground {2})" -f $t, $ok, [Inp]::GetForegroundWindow())
    [Inp]::Key($vk, $true); Start-Sleep -Milliseconds 300; [Inp]::Key($vk, $false)
}
Log "done"
