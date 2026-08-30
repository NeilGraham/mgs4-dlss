# Overnight gold run, detached from the terminal: see run_gold_worker.ps1 for the steps.
#   powershell -ExecutionPolicy Bypass -File tools\run_gold_overnight.ps1 [-Redo]
param([switch]$Redo)
$tools = Split-Path -Parent $MyInvocation.MyCommand.Path
New-Item -ItemType Directory -Force "D:\mgs4-dlss5\gold\raw" | Out-Null
$worker = Join-Path $tools "run_gold_worker.ps1"
$argList = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", ('"' + $worker + '"'))
if ($Redo) { $argList += "-Redo" }
Start-Process -FilePath "powershell.exe" -ArgumentList $argList -WindowStyle Minimized
Write-Host "gold run started in the background; log: D:\mgs4-dlss5\gold\gold.log, output: D:\mgs4-dlss5\gold\overnight.txt"
