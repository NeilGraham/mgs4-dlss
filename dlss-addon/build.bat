@echo off
setlocal
cd /d "%~dp0"
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || (echo vcvars64 not found & exit /b 1)
if not exist ..\build mkdir ..\build
set INC=/I ..\third_party\reshade\include /I ..\third_party\DLSS\include
set LIBS=d3d12.lib dxgi.lib advapi32.lib user32.lib ..\third_party\DLSS\lib\Windows_x86_64\x64\nvsdk_ngx_s.lib
cl /nologo /std:c++17 /O2 /MT /EHsc /W3 /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS %INC% /LD src\mgs4_dlss.cpp /Fe:..\build\mgs4_dlss.addon64 /Fo:..\build\ /link %LIBS%
if errorlevel 1 exit /b 1
dir ..\build\mgs4_dlss.addon64 | findstr addon64
