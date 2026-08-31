@echo off
:: MGS4 DLSS: start the game or any single scene in it, set the add-on up, and check the install.
:: Double-click it, or run it from a terminal.
::
::   mgs4-dlss.bat                       the window (Play / Settings / Install)
::   mgs4-dlss.bat s02a50l_D1            boot that scene, no window
::   mgs4-dlss.bat --main                MGS4's own main menu, past the Master Collection screen
::   mgs4-dlss.bat --list naomi          what can be launched
::   mgs4-dlss.bat --install             the window, opened on the install check
::   mgs4-dlss.bat --report              the install check as text, for pasting into an issue
::   mgs4-dlss.bat --shortcuts           rebuild "Desktop\MGS4 Shortcuts" against this checkout
::   mgs4-dlss.bat --help                every option
::
:: launcher.bat and check-install.bat open the same program on the Play and Install tabs.
::
:: -ExecutionPolicy Bypass is what lets this run straight out of an unzipped release, without the user having to
:: unblock the files first. Arguments go through untouched: mgs4_dlss.ps1 has no param() block, so PowerShell hands
:: --flags to it verbatim.
setlocal
set "PS=%~dp0tools\mgs4_dlss.ps1"
if not exist "%PS%" (
    echo Could not find tools\mgs4_dlss.ps1 next to this file.
    echo Run mgs4-dlss.bat from the folder it was unzipped into.
    pause
    exit /b 1
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%PS%" %*
set "RC=%ERRORLEVEL%"

:: On success the window did the talking and a double-clicked console can close with it. On a failure it must not
:: take the reason with it, so hold there instead.
if not "%RC%"=="0" pause
exit /b %RC%
