# The detached worker of run_gold_overnight.ps1: record -> finalize -> check sheets -> restore FrameGen=4.
param([switch]$Redo)
$tools = Split-Path -Parent $MyInvocation.MyCommand.Path
. "$tools\paths.ps1"
$p = Get-Mgs4Paths
$gold = $p.Gold
$ini = Join-Path (Get-Mgs4GameDir) "mgs4_dlss.ini"
$out = "$gold\overnight.txt"
$py = $p.Python
New-Item -ItemType Directory -Force $gold | Out-Null
Add-Content $out ("=== worker started {0} (python {1}, redo {2})" -f (Get-Date), $py, $Redo)
(Get-Content $ini) -replace '^FrameGen=.*', 'FrameGen=0' | Set-Content $ini -Encoding ASCII
$recArgs = @("$tools\record_gold.py", "--cap-minutes", "20", "--capture", "game")
if ($Redo) { $recArgs += "--redo" }
& $py @recArgs *>> $out
& $py "$tools\finalize_gold.py" *>> $out
& $py "$tools\inspect_gold.py" *>> $out
(Get-Content $ini) -replace '^FrameGen=.*', 'FrameGen=4' | Set-Content $ini -Encoding ASCII
Add-Content $out ("=== worker finished {0}; FrameGen restored to 4" -f (Get-Date))
