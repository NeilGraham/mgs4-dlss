// MGS4_D3D12.asi - forces bgfx to skip its Direct3D 11 backend so it falls back to Direct3D 12.
// Mechanism: bgfx::rendererCreate() tries backends in score order (D3D11 > D3D12 > ...) and moves to
// the next one when creation fails. bgfx's D3D11 backend calls D3D11CreateDevice with
// D3D11_CREATE_DEVICE_SINGLETHREADED (0x1). We fail exactly those calls.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "MinHook.h"

typedef HRESULT (WINAPI *PFN_CreateDevice)(void*, int, HMODULE, UINT, const int*, UINT, UINT, void**, int*, void**);
typedef HRESULT (WINAPI *PFN_CreateDeviceAndSwapChain)(void*, int, HMODULE, UINT, const int*, UINT, UINT, const void*, void**, void**, int*, void**);

static PFN_CreateDevice             g_origCreateDevice;
static PFN_CreateDeviceAndSwapChain g_origCreateDeviceAndSwapChain;
static FILE* g_log;
static int   g_enabled  = 1;
static UINT  g_failMask = 0x1;   // D3D11_CREATE_DEVICE_SINGLETHREADED
static int   g_maxFails = 16;
static int   g_fails    = 0;
static char  g_gameDir[MAX_PATH];
static char  g_iniPath[MAX_PATH];

static void logmsg(const char* fmt, ...)
{
    if (!g_log) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log); fflush(g_log);
}

static int should_fail(UINT flags)
{
    return g_enabled && (flags & g_failMask) == g_failMask && g_fails < g_maxFails;
}

static HRESULT WINAPI hk_CreateDevice(void* adapter, int driverType, HMODULE sw, UINT flags, const int* levels, UINT numLevels,
                                      UINT sdk, void** device, int* outLevel, void** ctx)
{
    logmsg("D3D11CreateDevice(adapter=%p, driverType=%d, flags=0x%X, featureLevels=%u)", adapter, driverType, flags, numLevels);
    if (should_fail(flags)) {
        ++g_fails;
        logmsg("  -> forced failure #%d (E_FAIL): bgfx should now try Direct3D 12", g_fails);
        return E_FAIL;
    }
    HRESULT hr = g_origCreateDevice(adapter, driverType, sw, flags, levels, numLevels, sdk, device, outLevel, ctx);
    logmsg("  -> passed through, hr=0x%08lX", (unsigned long)hr);
    return hr;
}

static HRESULT WINAPI hk_CreateDeviceAndSwapChain(void* adapter, int driverType, HMODULE sw, UINT flags, const int* levels, UINT numLevels,
                                                  UINT sdk, const void* scDesc, void** sc, void** device, int* outLevel, void** ctx)
{
    logmsg("D3D11CreateDeviceAndSwapChain(adapter=%p, driverType=%d, flags=0x%X, featureLevels=%u, scDesc=%p)", adapter, driverType, flags, numLevels, scDesc);
    if (should_fail(flags)) {
        ++g_fails;
        logmsg("  -> forced failure #%d (E_FAIL): bgfx should now try Direct3D 12", g_fails);
        return E_FAIL;
    }
    HRESULT hr = g_origCreateDeviceAndSwapChain(adapter, driverType, sw, flags, levels, numLevels, sdk, scDesc, sc, device, outLevel, ctx);
    logmsg("  -> passed through, hr=0x%08lX", (unsigned long)hr);
    return hr;
}

static void init_paths(HMODULE self)
{
    GetModuleFileNameA(NULL, g_gameDir, MAX_PATH);
    char* p = strrchr(g_gameDir, '\\'); if (p) *p = 0;

    GetModuleFileNameA(self, g_iniPath, MAX_PATH);
    p = strrchr(g_iniPath, '.'); if (p) strcpy(p, ".ini"); // scripts\MGS4_D3D12.ini

    char logPath[MAX_PATH];
    snprintf(logPath, MAX_PATH, "%s\\logs", g_gameDir); CreateDirectoryA(logPath, NULL);
    snprintf(logPath, MAX_PATH, "%s\\logs\\MGS4_D3D12.log", g_gameDir);
    g_log = fopen(logPath, "w");
}

static void load_config(void)
{
    g_enabled  = GetPrivateProfileIntA("Settings", "Enabled", 1, g_iniPath);
    g_failMask = (UINT)GetPrivateProfileIntA("Settings", "FailFlagsMask", 1, g_iniPath);
    g_maxFails = GetPrivateProfileIntA("Settings", "MaxFails", 16, g_iniPath);
    logmsg("config %s: Enabled=%d FailFlagsMask=0x%X MaxFails=%d", g_iniPath, g_enabled, g_failMask, g_maxFails);
}

static int install_hooks(void)
{
    // Pin the game-folder d3d11.dll proxy (the ASI loader) so bgfx's dlclose() after the forced failure can't unload it.
    char path[MAX_PATH];
    snprintf(path, MAX_PATH, "%s\\d3d11.dll", g_gameDir);
    HMODULE proxy = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_PIN, path, &proxy);
    logmsg("proxy d3d11.dll %s (%p)", proxy ? "pinned" : "not found", proxy);

    // Hook the real system d3d11.dll: the proxy forwards to it, and bgfx's own GetProcAddress lands there too.
    GetSystemDirectoryA(path, MAX_PATH); strcat(path, "\\d3d11.dll");
    HMODULE sys = LoadLibraryA(path); // holds a reference for the life of the process
    if (!sys) { logmsg("FATAL: cannot load %s", path); return 0; }
    void* pCD  = (void*)GetProcAddress(sys, "D3D11CreateDevice");
    void* pCDS = (void*)GetProcAddress(sys, "D3D11CreateDeviceAndSwapChain");
    logmsg("system d3d11.dll=%p D3D11CreateDevice=%p D3D11CreateDeviceAndSwapChain=%p", sys, pCD, pCDS);
    if (!pCD || !pCDS) return 0;

    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) { logmsg("MH_Initialize failed: %d", s); return 0; }
    if ((s = MH_CreateHook(pCD,  (void*)hk_CreateDevice,             (void**)&g_origCreateDevice))             != MH_OK) { logmsg("hook CreateDevice failed: %d", s); return 0; }
    if ((s = MH_CreateHook(pCDS, (void*)hk_CreateDeviceAndSwapChain, (void**)&g_origCreateDeviceAndSwapChain)) != MH_OK) { logmsg("hook CreateDeviceAndSwapChain failed: %d", s); return 0; }
    if ((s = MH_EnableHook(MH_ALL_HOOKS)) != MH_OK) { logmsg("MH_EnableHook failed: %d", s); return 0; }
    logmsg("hooks installed");
    return 1;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        init_paths(inst);
        logmsg("MGS4_D3D12 v0.1 loaded from %s", g_iniPath);
        load_config();
        if (g_enabled) install_hooks(); else logmsg("disabled by config; doing nothing");
    }
    return TRUE;
}
