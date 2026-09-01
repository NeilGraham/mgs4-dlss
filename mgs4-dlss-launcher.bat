@echo off
:: MGS4 DLSS Launcher: start the game or any single scene in it, set the add-on up, and check the install.
:: This is the only entry point - Play, Settings and Setup are tabs of the one window.
:: Double-click it, or run it from a terminal.
::
::   mgs4-dlss-launcher.bat                the window (Play / Settings / Setup)
::   mgs4-dlss-launcher.bat s02a50l_D1     boot that scene, no window
::   mgs4-dlss-launcher.bat --main         MGS4's own main menu, past the Master Collection screen
::   mgs4-dlss-launcher.bat --list naomi   what can be launched
::   mgs4-dlss-launcher.bat --setup        the window, opened on Setup
::   mgs4-dlss-launcher.bat --report       the install check as text, for pasting into an issue
::   mgs4-dlss-launcher.bat --help         every option
::
:: The app itself is mgs4-dlss-launcher.exe, built from launcher\ by the C# compiler that ships with Windows. It is
:: not in the repo, because the icon inside it is read from the mgs4.exe on this machine and that artwork is
:: Konami's - so a fresh clone has no exe and this builds one, once, before running it. Nothing has to be
:: installed for that.
setlocal
set "EXE=%~dp0mgs4-dlss-launcher.exe"
set "CLI=%~dp0mgs4-dlss-launcher-cli.exe"
set "BUILD=%~dp0launcher\build.ps1"

if not exist "%CLI%" (
    if not exist "%BUILD%" (
        echo Could not find launcher\build.ps1 next to this file.
        echo Run mgs4-dlss-launcher.bat from the folder it was unzipped into.
        pause
        exit /b 1
    )
    echo Building mgs4-dlss-launcher.exe  ^(first run only^)...
    powershell -NoProfile -ExecutionPolicy Bypass -File "%BUILD%"
    if errorlevel 1 (
        echo.
        echo The build failed. See the message above.
        pause
        exit /b 1
    )
)

:: The console twin, not the windowed exe: cmd does not wait for a windowed program, and `start /b /wait`, which
:: does wait, hands the child its own handles - so a redirect here would catch nothing. Same code, same arguments,
:: same exit code; it just behaves like a command.
"%CLI%" %*
set "RC=%ERRORLEVEL%"

:: No pause on a failing run: the app has already printed why, or - when it was the window that failed - said so in
:: a message box of its own. cmd cannot tell a double-click from a script's `cmd /c`, so pausing here would hang
:: every script that hits a non-zero exit. The one pause left is the build failing above, which happens before
:: there is any window or any message to read.
exit /b %RC%
