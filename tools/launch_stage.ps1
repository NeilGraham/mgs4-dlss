# Boots MGS4 straight into a stage and optionally presses keys to set the scene up (e.g. skip the intro).
#   powershell -ExecutionPolicy Bypass -File tools\launch_stage.ps1 -Stage s00a00l -WaitSeconds 55 -Keys "5,ENTER,4,ENTER"
# Keys: comma-separated list of key names (E, SPACE, ENTER, ESC, F1..) and numbers (seconds to wait).
# Keys are sent with SendInput while the game window is in the foreground, so the game's raw-input path sees them.
param(
    [string]$Stage = "s00a00l",
    [int]$WaitSeconds = 55,
    [string]$Keys = "5,ENTER,4,ENTER",
    [string]$GameDir = "C:\Program Files (x86)\Steam\steamapps\common\METAL GEAR SOLID 4\MGS4",
    [switch]$NoRestart
)
$ErrorActionPreference = "Continue"
Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices;
public static class Inp {
  [StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT { public ushort wVk; public ushort wScan; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
  [StructLayout(LayoutKind.Explicit)] public struct INPUTUNION { [FieldOffset(0)] public KEYBDINPUT ki; [FieldOffset(0)] public long pad1; [FieldOffset(8)] public long pad2; [FieldOffset(16)] public long pad3; }
  [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public INPUTUNION u; }
  [DllImport("user32.dll", SetLastError=true)] public static extern uint SendInput(uint n, INPUT[] inputs, int size);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint mapType);
  public static void Key(ushort vk, bool down) {
    var i = new INPUT(); i.type = 1; i.u.ki.wVk = vk; i.u.ki.wScan = (ushort)MapVirtualKey(vk, 0); i.u.ki.dwFlags = (uint)((down ? 0 : 2) | 8); // KEYEVENTF_SCANCODE (8) so raw input sees a real scan code
    SendInput(1, new INPUT[] { i }, Marshal.SizeOf(typeof(INPUT)));
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
    Write-Host "launched --stage $Stage, waiting $WaitSeconds s"
    Start-Sleep $WaitSeconds
}
$pr = Get-Process mgs4 -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $pr) { Write-Host "no game window"; exit 1 }
[Inp]::SetForegroundWindow($pr.MainWindowHandle) | Out-Null; Start-Sleep -Milliseconds 500
foreach ($tok in $Keys.Split(',')) {
    $t = $tok.Trim(); if ($t -eq "") { continue }
    if ($t -match '^\d+(\.\d+)?$') { Write-Host "wait $t s"; Start-Sleep ([double]$t); continue }
    $vk = [uint16](VK $t)
    Write-Host "press $t"
    [Inp]::Key($vk, $true); Start-Sleep -Milliseconds 120; [Inp]::Key($vk, $false)
}
Write-Host "done"
