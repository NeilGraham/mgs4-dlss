@echo off
:: Opens the install check for the MGS4 DLSS add-on: which required and optional files are in place, what the
:: settings say, and what the add-on reported on its last run. Double-click it, or run it from a terminal.
::
::   check-install.bat                     the window (Refresh / F5 re-checks without closing it)
::   check-install.bat --report            the same findings as text, for pasting into an issue
::   check-install.bat -GameDir "D:\..."   check an install the Steam library search does not find
::
:: -ExecutionPolicy Bypass is what lets this run straight out of an unzipped release, without the user having to
:: unblock the files first.
setlocal
set "PS=%~dp0tools\check_install.ps1"
if not exist "%PS%" (
    echo Could not find tools\check_install.ps1 next to this file.
    echo Run check-install.bat from the folder it was unzipped into.
    pause
    exit /b 1
)

set "ARGS=%*"
if /i "%~1"=="--report" set "ARGS=-Report"
if /i "%~1"=="/report" set "ARGS=-Report"
if /i "%~1"=="-report" set "ARGS=-Report"

powershell -NoProfile -ExecutionPolicy Bypass -File "%PS%" %ARGS%
set "RC=%ERRORLEVEL%"

:: On success the window did the talking and a double-clicked console can close with it. On a failure it must not
:: take the reason with it, so hold there instead.
if not "%RC%"=="0" pause
exit /b %RC%
