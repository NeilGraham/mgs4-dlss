@echo off
:: Alias for mgs4-dlss.bat, which opens on the Play tab anyway. Kept because it is what the desktop shortcuts and the
:: older notes say, and because "launcher" is the obvious thing to look for when you want to start a scene.
::
::   launcher.bat                        the window
::   launcher.bat s02a50l_D1             boot that scene, no window
::   launcher.bat --help                 every option
setlocal
if not exist "%~dp0mgs4-dlss.bat" (
    echo Could not find mgs4-dlss.bat next to this file.
    pause
    exit /b 1
)
call "%~dp0mgs4-dlss.bat" %*
exit /b %ERRORLEVEL%
