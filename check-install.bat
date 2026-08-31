@echo off
:: Opens the install check for the MGS4 DLSS add-on: which required and optional files are in place, what the
:: settings say, and what the add-on reported on its last run. It is the Install tab of mgs4-dlss.bat; this name is
:: kept because it is what the README, the releases and every issue reply say.
::
::   check-install.bat                     the window, on the install check (Re-check / F5 re-runs it)
::   check-install.bat --report            the same findings as text, for pasting into an issue
::   check-install.bat -GameDir "D:\..."   check an install the Steam library search does not find
::
:: With no arguments it opens on the Install tab; the other two tabs (Play, Settings) are right there.
setlocal
if not exist "%~dp0mgs4-dlss.bat" (
    echo Could not find mgs4-dlss.bat next to this file.
    echo Run check-install.bat from the folder it was unzipped into.
    pause
    exit /b 1
)

:: --report / -report / /report all mean the text form; anything else is passed through, and no arguments at all
:: means "open the window on the install check".
set "REPORT="
if /i "%~1"=="--report" set "REPORT=1"
if /i "%~1"=="-report" set "REPORT=1"
if /i "%~1"=="/report" set "REPORT=1"

if defined REPORT (
    call "%~dp0mgs4-dlss.bat" --report %2 %3 %4 %5 %6 %7 %8 %9
) else (
    call "%~dp0mgs4-dlss.bat" --install %*
)
exit /b %ERRORLEVEL%
