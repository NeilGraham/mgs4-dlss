# Overnight gold run, detached from the terminal: see run_gold_worker.ps1 for the steps.
#   powershell -ExecutionPolicy Bypass -File tools\run_gold_overnight.ps1 [-Redo]
param([switch]$Redo)
$tools = Split-Path -Parent $MyInvocation.MyCommand.Path
. "$tools\paths.ps1"
$gold = (Get-Mgs4Paths).Gold
New-Item -ItemType Directory -Force (Join-Path $gold "raw") | Out-Null
$worker = Join-Path $tools "run_gold_worker.ps1"
$argList = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", ('"' + $worker + '"'))
if ($Redo) { $argList += "-Redo" }
Start-Process -FilePath "powershell.exe" -ArgumentList $argList -WindowStyle Minimized
Write-Host "gold run started in the background; log: $gold\gold.log, output: $gold\overnight.txt"
