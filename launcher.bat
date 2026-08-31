@echo off
:: Opens the MGS4 DLSS launcher: start the game, or any single scene in it, and set the add-on up.
:: Double-click it, or run it from a terminal.
::
::   launcher.bat                        the window
::   launcher.bat s02a50l_D1             boot that scene, no window
::   launcher.bat --main                 MGS4's own main menu, past the Master Collection screen
::   launcher.bat --list naomi           what can be launched
::   launcher.bat --shortcuts            rebuild "Desktop\MGS4 Shortcuts" against this checkout
::   launcher.bat --help                 every option
::
:: -ExecutionPolicy Bypass is what lets this run straight out of an unzipped release, without the user having to
:: unblock the files first. Arguments go through untouched: launcher.ps1 has no param() block, so PowerShell hands
:: --flags to it verbatim.
setlocal
set "PS=%~dp0tools\launcher.ps1"
if not exist "%PS%" (
    echo Could not find tools\launcher.ps1 next to this file.
    echo Run launcher.bat from the folder it was unzipped into.
    pause
    exit /b 1
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%PS%" %*
set "RC=%ERRORLEVEL%"

:: On success the window did the talking and a double-clicked console can close with it. On a failure it must not
:: take the reason with it, so hold there instead.
if not "%RC%"=="0" pause
exit /b %RC%
