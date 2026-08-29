@echo off
setlocal
cd /d "%~dp0"
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || (echo vcvars64 not found & exit /b 1)
if not exist ..\build mkdir ..\build
set FXC="C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\fxc.exe"
%FXC% /nologo /T cs_5_0 /E main /O3 /Fh ..\build\mv_cs.h /Vn g_mv_cs src\mv_cs.hlsl || exit /b 1
%FXC% /nologo /T cs_5_0 /E main /O3 /Fh ..\build\mv_vis.h /Vn g_mv_vis src\mv_vis.hlsl || exit /b 1

%FXC% /nologo /T cs_5_0 /E main /O3 /Fh ..\build\hudless_cs.h /Vn g_hudless_cs src\hudless_cs.hlsl || exit /b 1

%FXC% /nologo /T vs_5_0 /E main /O3 /Fh ..\build\velocity_vs.h /Vn g_velocity_vs src\velocity_vs.hlsl || exit /b 1

%FXC% /nologo /T ps_5_0 /E main /O3 /Fh ..\build\velocity_ps.h /Vn g_velocity_ps src\velocity_ps.hlsl || exit /b 1

%FXC% /nologo /T cs_5_0 /E main /O3 /Fh ..\build\resample_cs.h /Vn g_resample_cs src\resample_cs.hlsl || exit /b 1
%FXC% /nologo /T cs_5_0 /E main /O3 /Fh ..\build\depth_stretch_cs.h /Vn g_depth_stretch_cs src\depth_stretch_cs.hlsl || exit /b 1
%FXC% /nologo /T cs_5_0 /E main /O3 /Fh ..\build\uimask_cs.h /Vn g_uimask_cs src\uimask_cs.hlsl || exit /b 1
set INC=/I ..\build /I ..\third_party\imgui /I ..\third_party\reshade\include /I ..\third_party\DLSS\include /I ..\third_party\streamline\include /I ..\third_party\minhook\include
set LIBS=d3d12.lib dxgi.lib advapi32.lib user32.lib ..\third_party\DLSS\lib\Windows_x86_64\x64\nvsdk_ngx_s.lib
cl /nologo /std:c++17 /O2 /MT /EHsc /W3 /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS %INC% /LD src\mgs4_dlss.cpp src\fg.cpp src\objmv.cpp ..\third_party\minhook\src\buffer.c ..\third_party\minhook\src\hook.c ..\third_party\minhook\src\trampoline.c ..\third_party\minhook\src\hde\hde64.c /Fe:..\build\mgs4_dlss.addon64 /Fo:..\build\ /link %LIBS%
if errorlevel 1 exit /b 1
dir ..\build\mgs4_dlss.addon64 | findstr addon64
