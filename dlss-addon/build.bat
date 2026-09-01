@echo off
:: Builds mgs4_dlss.addon64 into ..\build. The MSVC environment and fxc.exe are located automatically (vswhere and
:: the newest Windows 10 SDK); MGS4_VCVARS / MGS4_FXC, from the environment or ..\config.ini, override that.
setlocal enabledelayedexpansion
cd /d "%~dp0"

set "CFG=%~dp0..\config.ini"
if exist "%CFG%" for /f "usebackq tokens=1,* delims==" %%a in (`findstr /b /i "MGS4_VCVARS MGS4_FXC" "%CFG%"`) do (
    if /i "%%a"=="MGS4_VCVARS" if not defined MGS4_VCVARS set "MGS4_VCVARS=%%~b"
    if /i "%%a"=="MGS4_FXC" if not defined MGS4_FXC set "MGS4_FXC=%%~b"
)

if not defined MGS4_VCVARS (
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if exist "!VSWHERE!" for /f "usebackq delims=" %%i in (`""!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath" 2^>nul`) do (
        if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" set "MGS4_VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
    )
)
if not defined MGS4_VCVARS (echo vcvars64.bat not found - set MGS4_VCVARS in "%CFG%" or in the environment & exit /b 1)
call "%MGS4_VCVARS%" >nul 2>&1 || (echo "%MGS4_VCVARS%" failed & exit /b 1)
if not exist ..\build mkdir ..\build

:: fxc.exe: the SDK the MSVC environment just selected, else the newest Windows 10 SDK installed
if not defined MGS4_FXC if defined WindowsSdkVerBinPath if exist "%WindowsSdkVerBinPath%x64\fxc.exe" set "MGS4_FXC=%WindowsSdkVerBinPath%x64\fxc.exe"
if not defined MGS4_FXC for /f "delims=" %%d in ('dir /b /ad /o-n "%ProgramFiles(x86)%\Windows Kits\10\bin\10.*" 2^>nul') do (
    if not defined MGS4_FXC if exist "%ProgramFiles(x86)%\Windows Kits\10\bin\%%d\x64\fxc.exe" set "MGS4_FXC=%ProgramFiles(x86)%\Windows Kits\10\bin\%%d\x64\fxc.exe"
)
if not defined MGS4_FXC (echo fxc.exe not found - set MGS4_FXC in "%CFG%" or in the environment & exit /b 1)
set FXC="%MGS4_FXC%"

%FXC% /nologo /T cs_5_0 /E main /O3 /Fh ..\build\mv_cs.h /Vn g_mv_cs src\mv_cs.hlsl || exit /b 1
%FXC% /nologo /T cs_5_0 /E main /O3 /Fh ..\build\mv_vis.h /Vn g_mv_vis src\mv_vis.hlsl || exit /b 1

%FXC% /nologo /T cs_5_0 /E main /O3 /Fh ..\build\hudless_cs.h /Vn g_hudless_cs src\hudless_cs.hlsl || exit /b 1

%FXC% /nologo /T vs_5_0 /E main /O3 /Fh ..\build\velocity_vs.h /Vn g_velocity_vs src\velocity_vs.hlsl || exit /b 1

%FXC% /nologo /T ps_5_0 /E main /O3 /Fh ..\build\velocity_ps.h /Vn g_velocity_ps src\velocity_ps.hlsl || exit /b 1

%FXC% /nologo /T cs_5_0 /E main /O3 /Fh ..\build\resample_cs.h /Vn g_resample_cs src\resample_cs.hlsl || exit /b 1
%FXC% /nologo /T cs_5_0 /E main /O3 /Fh ..\build\depth_stretch_cs.h /Vn g_depth_stretch_cs src\depth_stretch_cs.hlsl || exit /b 1
%FXC% /nologo /T cs_5_0 /E main /O3 /Fh ..\build\uimask_cs.h /Vn g_uimask_cs src\uimask_cs.hlsl || exit /b 1
%FXC% /nologo /T cs_5_0 /E main /O3 /Fh ..\build\dof_coc_cs.h /Vn g_dof_coc_cs src\dof_coc_cs.hlsl || exit /b 1
%FXC% /nologo /T cs_5_0 /E main /O3 /Fh ..\build\dof_pack_cs.h /Vn g_dof_pack_cs src\dof_pack_cs.hlsl || exit /b 1
%FXC% /nologo /T cs_5_0 /E main /O3 /Fh ..\build\dof_gather_cs.h /Vn g_dof_gather_cs src\dof_gather_cs.hlsl || exit /b 1
%FXC% /nologo /T cs_5_0 /E main /O3 /Fh ..\build\dof_composite_cs.h /Vn g_dof_composite_cs src\dof_composite_cs.hlsl || exit /b 1
%FXC% /nologo /T cs_5_0 /E main /O3 /Fh ..\build\probe_cs.h /Vn g_probe_cs src\probe_cs.hlsl || exit /b 1
set INC=/I ..\build /I ..\third_party\imgui /I ..\third_party\reshade\include /I ..\third_party\DLSS\include /I ..\third_party\streamline\include /I ..\third_party\minhook\include
set LIBS=d3d12.lib dxgi.lib advapi32.lib user32.lib ..\third_party\DLSS\lib\Windows_x86_64\x64\nvsdk_ngx_s.lib
cl /nologo /std:c++17 /O2 /MT /EHsc /W3 /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS %INC% /LD src\mgs4_dlss.cpp src\fg.cpp src\objmv.cpp ..\third_party\minhook\src\buffer.c ..\third_party\minhook\src\hook.c ..\third_party\minhook\src\trampoline.c ..\third_party\minhook\src\hde\hde64.c /Fe:..\build\mgs4_dlss.addon64 /Fo:..\build\ /link %LIBS%
if errorlevel 1 exit /b 1
dir ..\build\mgs4_dlss.addon64 | findstr addon64
