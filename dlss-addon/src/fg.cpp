// DLSS Frame Generation via NVIDIA Streamline (manual hooking mode).
//
// The game (bgfx) creates its swapchain with IDXGIFactory::CreateSwapChain. ReShade already vtable-hooks that call
// (it sits between the game and DXGI). We hook the same entry once more, so we run FIRST, and route the call through
// Streamline's proxy factory: game -> SL proxy swapchain -> ReShade proxy -> DXGI. Streamline then intercepts Present
// on its proxy and inserts generated frames; ReShade (and our present event) see every presented frame, so the
// add-on counts game frames only when scene draws happened.
#define WIN32_LEAN_AND_MEAN
#include "fg.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include "MinHook.h"
// Streamline SDK headers (types only; every function is fetched from sl.interposer.dll at runtime)
#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss_g.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
#include <sl_matrix_helpers.h>

namespace fg {

static LogFn g_log = nullptr;
#define LOG(...) do { if (g_log) g_log(__VA_ARGS__); } while (0)

static Settings g_set;
static Status g_st;
static ID3D12Device* g_device = nullptr;
static HMODULE g_sl = nullptr;
static std::mutex g_mutex;

// core API
static PFun_slInit* p_slInit = nullptr;
static PFun_slShutdown* p_slShutdown = nullptr;
static PFun_slSetD3DDevice* p_slSetD3DDevice = nullptr;
static PFun_slUpgradeInterface* p_slUpgradeInterface = nullptr;
static PFun_slGetNativeInterface* p_slGetNativeInterface = nullptr;
static PFun_slIsFeatureSupported* p_slIsFeatureSupported = nullptr;
static PFun_slIsFeatureLoaded* p_slIsFeatureLoaded = nullptr;
static PFun_slSetFeatureLoaded* p_slSetFeatureLoaded = nullptr;
static PFun_slGetFeatureFunction* p_slGetFeatureFunction = nullptr;
static PFun_slGetNewFrameToken* p_slGetNewFrameToken = nullptr;
static PFun_slSetConstants* p_slSetConstants = nullptr;
static PFun_slSetTagForFrame* p_slSetTagForFrame = nullptr;
static PFun_slGetFeatureRequirements* p_slGetFeatureRequirements = nullptr;
static PFun_slGetFeatureVersion* p_slGetFeatureVersion = nullptr;
// feature API
static PFun_slDLSSGSetOptions* p_slDLSSGSetOptions = nullptr;
static PFun_slDLSSGGetState* p_slDLSSGGetState = nullptr;
static PFun_slPCLSetMarker* p_slPCLSetMarker = nullptr;
static PFun_slReflexSetOptions* p_slReflexSetOptions = nullptr;
static PFun_slReflexSleep* p_slReflexSleep = nullptr;

static const sl::ViewportHandle kViewport(0u);
static sl::FrameToken* g_token = nullptr;       // current game frame's token
static uint32_t g_tokenFrame = UINT32_MAX;
static bool g_optionsDirty = true;
static bool g_reflexDirty = true;
static ULONGLONG g_lastPoll = 0;
static uint32_t g_gameFramesSincePoll = 0;
static uint32_t g_lastOptFrames = 1; static int g_lastOptMode = -1; static float g_lastOptTarget = -1;
static uint32_t g_lastSizes[4] = { 0, 0, 0, 0 };   // render w/h, colour w/h of the last apply_options call

// swapchain hooking
typedef HRESULT (STDMETHODCALLTYPE* PFN_CreateSwapChain)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
typedef HRESULT (STDMETHODCALLTYPE* PFN_CreateSwapChainForHwnd)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
typedef HRESULT (STDMETHODCALLTYPE* PFN_Present)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE* PFN_Present1)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
typedef HRESULT (STDMETHODCALLTYPE* PFN_GetBuffer)(IDXGISwapChain*, UINT, REFIID, void**);
typedef HRESULT (STDMETHODCALLTYPE* PFN_ResizeBuffers)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
static PFN_GetBuffer o_GetBuffer = nullptr;
static PFN_ResizeBuffers o_ResizeBuffers = nullptr;
static std::unordered_set<uint64_t> g_appBackbuffers;   // buffers the proxy swapchain hands to the game
static std::mutex g_bbMutex;
static void (*g_frameCb)() = nullptr;
static ID3D12Device* g_slDevice = nullptr;               // device handed to Streamline (the one the game's queue reports)
static PFN_CreateSwapChain o_CreateSwapChain = nullptr;
static PFN_CreateSwapChainForHwnd o_CreateSwapChainForHwnd = nullptr;
static PFN_Present o_Present = nullptr;
static PFN_Present1 o_Present1 = nullptr;
static thread_local bool t_inside = false;
static thread_local int t_slDepth = 0;            // >0 while inside one of our Streamline calls
static DWORD g_renderThread = 0;                  // thread that presents (bgfx render thread)
struct SlCall { SlCall() { ++t_slDepth; } ~SlCall() { --t_slDepth; } };
static IDXGISwapChain* g_proxySwapchain = nullptr;
static std::unordered_map<void*, void*> g_proxyFactories;   // native factory -> SL proxy factory
static uint32_t g_presentFrame = 0;                          // frame index for the present markers
static bool g_frameHasDraws = false;

static void sl_log(sl::LogType type, const char* msg)
{
    if (type == sl::LogType::eError || type == sl::LogType::eWarn) {
        LOG("[SL] %s", msg);
        if (type == sl::LogType::eError) strncpy_s(g_st.lastError, msg, _TRUNCATE);
    }
}

template <typename T> static bool feature_fn(sl::Feature f, T*& fn, const char* name)
{
    void* p = nullptr;
    sl::Result r = p_slGetFeatureFunction(f, name, p);
    fn = reinterpret_cast<T*>(p);
    if (r != sl::Result::eOk || !fn) LOG("FG: slGetFeatureFunction(%s) failed %d", name, (int)r);
    return fn != nullptr;
}

// The game holds ReShade's proxies for the device and queue. Streamline must work on the same objects the queue
// reports (that is also what happens in a native Streamline game with ReShade underneath), so the device is taken
// from the queue passed to CreateSwapChain rather than from the add-on's native device pointer.
static bool ensure_device(IUnknown* pDevice)
{
    if (!pDevice) return false;
    ID3D12CommandQueue* q = nullptr;
    if (FAILED(pDevice->QueryInterface(IID_PPV_ARGS(&q))) || !q) return false;
    if (!g_slDevice) {
        ID3D12Device* d = nullptr; q->GetDevice(IID_PPV_ARGS(&d));
        if (d) {
            sl::Result r = p_slSetD3DDevice(d);
            if (r != sl::Result::eOk) { LOG("FG: slSetD3DDevice failed %d", (int)r); d->Release(); q->Release(); return false; }
            g_slDevice = d;   // keep the reference
            LOG("FG: Streamline device set (%p%s)", (void*)d, d == g_device ? ", native" : ", proxy of the native device");
            feature_fn(sl::kFeatureDLSS_G, p_slDLSSGSetOptions, "slDLSSGSetOptions");
            feature_fn(sl::kFeatureDLSS_G, p_slDLSSGGetState, "slDLSSGGetState");
            feature_fn(sl::kFeaturePCL, p_slPCLSetMarker, "slPCLSetMarker");
            feature_fn(sl::kFeatureReflex, p_slReflexSetOptions, "slReflexSetOptions");
            feature_fn(sl::kFeatureReflex, p_slReflexSleep, "slReflexSleep");
        }
    }
    q->Release();
    return g_slDevice != nullptr;
}

static void* proxy_factory(void* native)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_proxyFactories.find(native);
    if (it != g_proxyFactories.end()) return it->second;
    void* proxy = native;
    sl::Result r = p_slUpgradeInterface(&proxy);
    if (r != sl::Result::eOk) { LOG("FG: slUpgradeInterface(factory) failed %d", (int)r); return nullptr; }
    g_proxyFactories[native] = proxy;
    LOG("FG: factory %p upgraded to Streamline proxy %p", native, proxy);
    return proxy;
}

static HRESULT STDMETHODCALLTYPE hk_Present(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    if (sc != g_proxySwapchain || t_inside) return o_Present(sc, sync, flags);
    g_renderThread = GetCurrentThreadId();
    if (g_token && p_slPCLSetMarker) { p_slPCLSetMarker(sl::PCLMarker::eRenderSubmitEnd, *g_token); p_slPCLSetMarker(sl::PCLMarker::ePresentStart, *g_token); }
    if (g_frameCb) g_frameCb();
    HRESULT hr; { SlCall guard; t_inside = true; hr = o_Present(sc, sync, flags); t_inside = false; }
    if (g_token && p_slPCLSetMarker) p_slPCLSetMarker(sl::PCLMarker::ePresentEnd, *g_token);
    return hr;
}
static HRESULT STDMETHODCALLTYPE hk_Present1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp)
{
    if ((IDXGISwapChain*)sc != g_proxySwapchain || t_inside) return o_Present1(sc, sync, flags, pp);
    g_renderThread = GetCurrentThreadId();
    if (g_token && p_slPCLSetMarker) { p_slPCLSetMarker(sl::PCLMarker::eRenderSubmitEnd, *g_token); p_slPCLSetMarker(sl::PCLMarker::ePresentStart, *g_token); }
    if (g_frameCb) g_frameCb();
    HRESULT hr; { SlCall guard; t_inside = true; hr = o_Present1(sc, sync, flags, pp); t_inside = false; }
    if (g_token && p_slPCLSetMarker) p_slPCLSetMarker(sl::PCLMarker::ePresentEnd, *g_token);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_GetBuffer(IDXGISwapChain* sc, UINT idx, REFIID riid, void** out)
{
    HRESULT hr = o_GetBuffer(sc, idx, riid, out);
    if (SUCCEEDED(hr) && sc == g_proxySwapchain && out && *out) {
        std::lock_guard<std::mutex> lock(g_bbMutex);
        if (g_appBackbuffers.insert(reinterpret_cast<uint64_t>(*out)).second) LOG("FG: game backbuffer %u -> %p (%s)", idx, *out, g_st.swapchainProxied ? "via Streamline proxy" : "?");
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE hk_ResizeBuffers(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags)
{
    if (sc == g_proxySwapchain) { std::lock_guard<std::mutex> lock(g_bbMutex); g_appBackbuffers.clear(); LOG("FG: ResizeBuffers %ux%u", w, h); }
    return o_ResizeBuffers(sc, count, w, h, fmt, flags);
}

static void hook_present(IDXGISwapChain* proxy)
{
    if (o_Present) return;
    void** vt = *reinterpret_cast<void***>(proxy);
    if (MH_CreateHook(vt[8], (void*)hk_Present, (void**)&o_Present) == MH_OK) MH_EnableHook(vt[8]);
    if (MH_CreateHook(vt[9], (void*)hk_GetBuffer, (void**)&o_GetBuffer) == MH_OK) MH_EnableHook(vt[9]);
    if (MH_CreateHook(vt[13], (void*)hk_ResizeBuffers, (void**)&o_ResizeBuffers) == MH_OK) MH_EnableHook(vt[13]);
    IDXGISwapChain1* sc1 = nullptr;
    if (SUCCEEDED(proxy->QueryInterface(IID_PPV_ARGS(&sc1))) && sc1) {
        void** vt1 = *reinterpret_cast<void***>(sc1);
        if (MH_CreateHook(vt1[22], (void*)hk_Present1, (void**)&o_Present1) == MH_OK) MH_EnableHook(vt1[22]);
        sc1->Release();
    }
    LOG("FG: Present hooks installed on the Streamline proxy swapchain");
}

static HRESULT STDMETHODCALLTYPE hk_CreateSwapChain(IDXGIFactory* self, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out)
{
    if (t_inside || !g_st.initialised || !ensure_device(pDevice)) return o_CreateSwapChain(self, pDevice, desc, out);
    IDXGIFactory* proxy = reinterpret_cast<IDXGIFactory*>(proxy_factory(self));
    if (!proxy) return o_CreateSwapChain(self, pDevice, desc, out);
    g_renderThread = GetCurrentThreadId();
    HRESULT hr; { SlCall guard; t_inside = true; hr = proxy->CreateSwapChain(pDevice, desc, out); t_inside = false; }
    if (SUCCEEDED(hr) && out && *out) { g_proxySwapchain = *out; g_st.swapchainProxied = true; hook_present(*out); LOG("FG: swapchain %ux%u created through Streamline (proxy %p)", desc->BufferDesc.Width, desc->BufferDesc.Height, (void*)*out); }
    else LOG("FG: Streamline CreateSwapChain failed 0x%08lX", (unsigned long)hr);
    return hr;
}
static HRESULT STDMETHODCALLTYPE hk_CreateSwapChainForHwnd(IDXGIFactory2* self, IUnknown* pDevice, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* output, IDXGISwapChain1** out)
{
    if (t_inside || !g_st.initialised || !ensure_device(pDevice)) return o_CreateSwapChainForHwnd(self, pDevice, hwnd, desc, fs, output, out);
    IDXGIFactory2* proxy = reinterpret_cast<IDXGIFactory2*>(proxy_factory(self));
    if (!proxy) return o_CreateSwapChainForHwnd(self, pDevice, hwnd, desc, fs, output, out);
    g_renderThread = GetCurrentThreadId();
    HRESULT hr; { SlCall guard; t_inside = true; hr = proxy->CreateSwapChainForHwnd(pDevice, hwnd, desc, fs, output, out); t_inside = false; }
    if (SUCCEEDED(hr) && out && *out) { g_proxySwapchain = *out; g_st.swapchainProxied = true; hook_present(*out); LOG("FG: swapchain %ux%u created through Streamline (proxy %p, ForHwnd)", desc->Width, desc->Height, (void*)*out); }
    else LOG("FG: Streamline CreateSwapChainForHwnd failed 0x%08lX", (unsigned long)hr);
    return hr;
}

static bool install_factory_hooks()
{
    typedef HRESULT (WINAPI* PFN_CreateDXGIFactory1)(REFIID, void**);
    HMODULE dxgi = GetModuleHandleA("dxgi.dll");
    if (!dxgi) dxgi = LoadLibraryA("dxgi.dll");
    auto create = reinterpret_cast<PFN_CreateDXGIFactory1>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
    IDXGIFactory2* f = nullptr;
    if (!create || FAILED(create(IID_PPV_ARGS(&f))) || !f) { LOG("FG: cannot create a DXGI factory to hook"); return false; }
    void** vt = *reinterpret_cast<void***>(f);
    MH_Initialize();
    bool ok = true;
    if (MH_CreateHook(vt[10], (void*)hk_CreateSwapChain, (void**)&o_CreateSwapChain) != MH_OK || MH_EnableHook(vt[10]) != MH_OK) { ok = false; LOG("FG: hook CreateSwapChain failed"); }
    if (MH_CreateHook(vt[15], (void*)hk_CreateSwapChainForHwnd, (void**)&o_CreateSwapChainForHwnd) != MH_OK || MH_EnableHook(vt[15]) != MH_OK) { ok = false; LOG("FG: hook CreateSwapChainForHwnd failed"); }
    f->Release();
    if (ok) LOG("FG: IDXGIFactory::CreateSwapChain/ForHwnd hooked (outermost, in front of ReShade)");
    return ok;
}

template <typename T> static bool load_fn(T*& fn, const char* name)
{
    fn = reinterpret_cast<T*>(GetProcAddress(g_sl, name));
    if (!fn) LOG("FG: sl.interposer.dll lacks %s", name);
    return fn != nullptr;
}
void init(ID3D12Device* device, const wchar_t* gameDirW, LogFn log)
{
    g_log = log; g_device = device;
    wchar_t path[MAX_PATH]; swprintf_s(path, L"%s\\sl.interposer.dll", gameDirW);
    g_sl = LoadLibraryW(path);
    if (!g_sl) { LOG("FG: sl.interposer.dll not found next to the game (Streamline runtime missing) - frame generation unavailable"); return; }
    g_st.loaded = true;
    bool ok = load_fn(p_slInit, "slInit") & load_fn(p_slShutdown, "slShutdown") & load_fn(p_slSetD3DDevice, "slSetD3DDevice") & load_fn(p_slUpgradeInterface, "slUpgradeInterface")
        & load_fn(p_slGetNativeInterface, "slGetNativeInterface") & load_fn(p_slIsFeatureSupported, "slIsFeatureSupported") & load_fn(p_slIsFeatureLoaded, "slIsFeatureLoaded")
        & load_fn(p_slSetFeatureLoaded, "slSetFeatureLoaded") & load_fn(p_slGetFeatureFunction, "slGetFeatureFunction") & load_fn(p_slGetNewFrameToken, "slGetNewFrameToken")
        & load_fn(p_slSetConstants, "slSetConstants") & load_fn(p_slSetTagForFrame, "slSetTagForFrame") & load_fn(p_slGetFeatureRequirements, "slGetFeatureRequirements") & load_fn(p_slGetFeatureVersion, "slGetFeatureVersion");
    if (!ok) return;

    static wchar_t pluginPath[MAX_PATH]; wcscpy_s(pluginPath, gameDirW);
    static wchar_t logPath[MAX_PATH]; swprintf_s(logPath, L"%s\\logs", gameDirW);
    static const wchar_t* paths[] = { pluginPath };
    static const sl::Feature features[] = { sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL };
    sl::Preferences pref;
    pref.showConsole = false;
    pref.logLevel = sl::LogLevel::eDefault;
    pref.pathsToPlugins = paths; pref.numPathsToPlugins = 1;
    pref.pathToLogsAndData = logPath;
    pref.logMessageCallback = sl_log;
    pref.flags = sl::PreferenceFlags::eUseManualHooking | sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    pref.featuresToLoad = features; pref.numFeaturesToLoad = 3;
    pref.applicationId = 0;
    pref.engine = sl::EngineType::eCustom; pref.engineVersion = "0.1"; pref.projectId = "7a2f8c3e-5d41-4b9a-9e0c-3f6d2b1a8c47";
    pref.renderAPI = sl::RenderAPI::eD3D12;
    sl::Result r = p_slInit(pref, sl::kSDKVersion);
    if (r != sl::Result::eOk) { LOG("FG: slInit failed %d", (int)r); snprintf(g_st.lastError, sizeof(g_st.lastError), "slInit failed (%d)", (int)r); return; }
    g_st.initialised = true;

    sl::FeatureVersion ver;
    if (p_slGetFeatureVersion(sl::kFeatureDLSS_G, ver) == sl::Result::eOk) snprintf(g_st.slVersion, sizeof(g_st.slVersion), "SL %u.%u.%u / DLSS-G %u.%u.%u", ver.versionSL.major, ver.versionSL.minor, ver.versionSL.build, ver.versionNGX.major, ver.versionNGX.minor, ver.versionNGX.build);
    LUID luid = device->GetAdapterLuid();
    sl::AdapterInfo ai; ai.deviceLUID = reinterpret_cast<uint8_t*>(&luid); ai.deviceLUIDSizeInBytes = sizeof(LUID);
    sl::Result sup = p_slIsFeatureSupported(sl::kFeatureDLSS_G, ai);
    g_st.supported = sup == sl::Result::eOk;
    LOG("FG: Streamline initialised (%s); DLSS-G supported: %s (%d)", g_st.slVersion, g_st.supported ? "yes" : "no", (int)sup);
    sl::FeatureRequirements req;
    if (p_slGetFeatureRequirements(sl::kFeatureDLSS_G, req) == sl::Result::eOk)
        LOG("FG: DLSS-G requirements flags 0x%X (vsync-off required: %s), driver %u.%u detected / %u.%u required", (unsigned)req.flags, ((unsigned)req.flags & (unsigned)sl::FeatureRequirementFlags::eVSyncOffRequired) ? "yes" : "no", req.driverVersionDetected.major, req.driverVersionDetected.minor, req.driverVersionRequired.major, req.driverVersionRequired.minor);
    if (!g_st.supported) { if (sup != sl::Result::eOk && !g_st.lastError[0]) snprintf(g_st.lastError, sizeof(g_st.lastError), "DLSS-G not supported on this adapter/driver (%d)", (int)sup); }
    install_factory_hooks();
}

void set_frame_callback(void (*fn)()) { g_frameCb = fn; }
bool inside_streamline()
{
    if (t_slDepth > 0) return true;
    return g_st.swapchainProxied && g_renderThread != 0 && GetCurrentThreadId() != g_renderThread;
}
bool is_app_backbuffer(uint64_t handle)
{
    if (!handle) return false;
    std::lock_guard<std::mutex> lock(g_bbMutex);
    return g_appBackbuffers.count(handle) != 0;
}

void shutdown()
{
    if (g_st.initialised && p_slShutdown) p_slShutdown();
    g_st.initialised = false;
}

void set_settings(const Settings& s) { g_set = s; g_optionsDirty = true; g_reflexDirty = true; }
const Settings& settings() { return g_set; }
const Status& status() { return g_st; }

static void apply_reflex()
{
    if (!p_slReflexSetOptions) return;
    sl::ReflexOptions ro;
    ro.mode = g_set.reflex == 0 ? sl::ReflexMode::eOff : (g_set.reflex == 2 ? sl::ReflexMode::eLowLatencyWithBoost : sl::ReflexMode::eLowLatency);
    ro.useMarkersToOptimize = true;
    sl::Result r = p_slReflexSetOptions(ro);
    if (r != sl::Result::eOk) LOG("FG: slReflexSetOptions failed %d", (int)r);
    g_reflexDirty = false;
}

static void on_api_error(const sl::APIError& e) { LOG("FG: DLSS-G API error hr=0x%08lX", (unsigned long)e.hres); snprintf(g_st.lastError, sizeof(g_st.lastError), "DLSS-G API error 0x%08lX", (unsigned long)e.hres); }

static void apply_options(uint32_t renderW, uint32_t renderH, uint32_t bbW, uint32_t bbH)
{
    if (!p_slDLSSGSetOptions || !g_st.supported || !g_st.swapchainProxied) return;
    g_lastSizes[0] = renderW; g_lastSizes[1] = renderH; g_lastSizes[2] = bbW; g_lastSizes[3] = bbH;
    sl::DLSSGOptions o;
    uint32_t frames = 1;
    if (g_set.mode == 0) o.mode = sl::DLSSGMode::eOff;
    else if (g_set.mode == 4) {
        if (g_st.dynamicSupported) { o.mode = sl::DLSSGMode::eDynamic; o.dynamicTargetFrameRate = g_set.targetFps; }
        else { o.mode = sl::DLSSGMode::eOn; frames = g_st.adaptiveFrames ? g_st.adaptiveFrames : 1; }   // fallback: adaptive controller
    } else { o.mode = sl::DLSSGMode::eOn; frames = (uint32_t)g_set.mode; }
    if (frames > g_st.maxFrames && g_st.maxFrames) frames = g_st.maxFrames;
    if (frames < 1) frames = 1;
    o.numFramesToGenerate = frames;
    o.mvecDepthWidth = renderW; o.mvecDepthHeight = renderH;
    o.colorWidth = bbW; o.colorHeight = bbH;
    o.onErrorCallback = on_api_error;
    if ((int)o.mode == g_lastOptMode && frames == g_lastOptFrames && o.dynamicTargetFrameRate == g_lastOptTarget && !g_optionsDirty) return;
    sl::Result r; { SlCall guard; r = p_slDLSSGSetOptions(kViewport, o); }
    if (r != sl::Result::eOk) { LOG("FG: slDLSSGSetOptions failed %d", (int)r); snprintf(g_st.lastError, sizeof(g_st.lastError), "slDLSSGSetOptions failed (%d)", (int)r); }
    else LOG("FG: options applied: mode %s, frames %u, target fps %.0f (render %ux%u, colour %ux%u)", o.mode == sl::DLSSGMode::eOff ? "off" : (o.mode == sl::DLSSGMode::eDynamic ? "dynamic" : "on"), frames, o.dynamicTargetFrameRate, renderW, renderH, bbW, bbH);
    g_lastOptMode = (int)o.mode; g_lastOptFrames = frames; g_lastOptTarget = o.dynamicTargetFrameRate; g_optionsDirty = false;
    g_st.active = o.mode != sl::DLSSGMode::eOff;
}

void poll()
{
    if (!g_st.initialised) return;
    g_gameFramesSincePoll++;
    if (g_optionsDirty && g_set.mode == 0 && g_lastSizes[0]) apply_options(g_lastSizes[0], g_lastSizes[1], g_lastSizes[2], g_lastSizes[3]);   // switching off: inputs are no longer sent
    const ULONGLONG now = GetTickCount64();
    if (g_lastPoll == 0) g_lastPoll = now;
    if (now - g_lastPoll < 1000) return;
    const float gameFps = g_gameFramesSincePoll * 1000.0f / float(now - g_lastPoll);
    g_lastPoll = now; g_gameFramesSincePoll = 0;
    if (p_slDLSSGGetState && g_st.swapchainProxied) {
        sl::DLSSGState st;
        SlCall guard;
        if (p_slDLSSGGetState(kViewport, st, nullptr) == sl::Result::eOk) {
            g_st.statusFlags = (uint32_t)st.status; g_st.framesPresented = st.numFramesActuallyPresented; g_st.maxFrames = st.numFramesToGenerateMax ? st.numFramesToGenerateMax : 1;
            g_st.vramBytes = st.estimatedVRAMUsageInBytes; g_st.dynamicSupported = st.bIsDynamicMFGSupported == sl::Boolean::eTrue; g_st.vsyncSupported = st.bIsVsyncSupportAvailable == sl::Boolean::eTrue;
        }
    }
    // adaptive fallback for the target-fps mode when dynamic MFG is not available: pick the multiplier that lands
    // closest to the target from the measured game frame rate
    if (g_set.mode == 4 && !g_st.dynamicSupported && gameFps > 5.0f) {
        float target = g_set.targetFps > 0 ? g_set.targetFps : 120.0f;
        uint32_t want = (uint32_t)std::lround(target / gameFps);
        if (want < 2) want = 2; if (want > g_st.maxFrames + 1) want = g_st.maxFrames + 1;
        if (want - 1 != g_st.adaptiveFrames) { g_st.adaptiveFrames = want - 1; g_optionsDirty = true; LOG("FG: adaptive: game %.0f fps, target %.0f -> %ux", gameFps, target, want); }
    }
}

void frame_begin(uint32_t frameIndex)
{
    if (!g_st.initialised) return;
    if (g_tokenFrame != frameIndex) {
        sl::FrameToken* t = nullptr;
        if (p_slGetNewFrameToken(t, &frameIndex) == sl::Result::eOk) { g_token = t; g_tokenFrame = frameIndex; }
    }
    if (!g_token) return;
    if (g_reflexDirty) apply_reflex();
    if (p_slReflexSleep) { SlCall guard; p_slReflexSleep(*g_token); }
    if (p_slPCLSetMarker) { p_slPCLSetMarker(sl::PCLMarker::eSimulationStart, *g_token); p_slPCLSetMarker(sl::PCLMarker::eSimulationEnd, *g_token); p_slPCLSetMarker(sl::PCLMarker::eRenderSubmitStart, *g_token); }
    g_frameHasDraws = true;
}

// our row-major (column-vector) 4x4 -> SL row-major (row-vector): transpose
static void to_sl(const float* m, sl::float4x4& out)
{
    for (int r = 0; r < 4; ++r) out.row[r] = sl::float4(m[0 * 4 + r], m[1 * 4 + r], m[2 * 4 + r], m[3 * 4 + r]);
}
static void mul4(const float* a, const float* b, float* out)   // out = a*b (row-major, column-vector convention)
{
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) { float s = 0; for (int k = 0; k < 4; ++k) s += a[r * 4 + k] * b[k * 4 + c]; out[r * 4 + c] = s; }
}
static bool inv4(const float* m, float* out)
{
    float inv[16];
    inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
    double det = (double)m[0]*inv[0] + (double)m[1]*inv[4] + (double)m[2]*inv[8] + (double)m[3]*inv[12];
    if (fabs(det) < 1e-30) return false;
    for (int i = 0; i < 16; ++i) out[i] = (float)(inv[i] / det);
    return true;
}
static bool camera_from_vp(const float* m, float* pos)   // point where clip x, y and w are all zero
{
    const float a[3][3] = { { m[0], m[1], m[2] }, { m[4], m[5], m[6] }, { m[12], m[13], m[14] } };
    const float b[3] = { -m[3], -m[7], -m[15] };
    const float det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    if (fabsf(det) < 1e-12f) return false;
    for (int i = 0; i < 3; ++i) {
        float c[3][3]; memcpy(c, a, sizeof(c));
        for (int r = 0; r < 3; ++r) c[r][i] = b[r];
        pos[i] = (c[0][0] * (c[1][1] * c[2][2] - c[1][2] * c[2][1]) - c[0][1] * (c[1][0] * c[2][2] - c[1][2] * c[2][0]) + c[0][2] * (c[1][0] * c[2][1] - c[1][1] * c[2][0])) / det;
    }
    return true;
}

void frame_inputs(uint32_t frameIndex, const FrameInputs& in, const CameraInput& cam)
{
    if (!g_st.initialised || !g_st.supported) return;
    if (g_tokenFrame != frameIndex) frame_begin(frameIndex);
    if (!g_token) return;
    apply_options(in.renderW, in.renderH, in.bbW, in.bbH);

    // ---- constants ----
    sl::Constants c;
    const float* vp = cam.vp;
    float sx = sqrtf(vp[0] * vp[0] + vp[1] * vp[1] + vp[2] * vp[2]);
    float sy = sqrtf(vp[4] * vp[4] + vp[5] * vp[5] + vp[6] * vp[6]);
    const float nearZ = vp[11] != 0 ? fabsf(vp[11]) : 1.0f;
    if (sx < 1e-6f) sx = 1; if (sy < 1e-6f) sy = 1;
    // projection (this port: y scale negative, reversed-Z with z_clip = near)
    float P[16] = { sx, 0, 0, 0,  0, -sy, 0, 0,  0, 0, 0, nearZ,  0, 0, 1, 0 };
    float Pinv[16] = { 1 / sx, 0, 0, 0,  0, -1 / sy, 0, 0,  0, 0, 0, 1,  0, 0, 1 / nearZ, 0 };
    float vpInv[16], prevVpInv[16], c2p[16], p2c[16];
    bool okI = inv4(vp, vpInv) && inv4(cam.prevVp, prevVpInv);
    if (okI) { mul4(cam.prevVp, vpInv, c2p); mul4(vp, prevVpInv, p2c); }
    else { memset(c2p, 0, sizeof(c2p)); memset(p2c, 0, sizeof(p2c)); for (int i = 0; i < 4; ++i) { c2p[i * 5] = 1; p2c[i * 5] = 1; } }
    to_sl(P, c.cameraViewToClip); to_sl(Pinv, c.clipToCameraView);
    to_sl(c2p, c.clipToPrevClip); to_sl(p2c, c.prevClipToClip);
    c.clipToLensClip = c.cameraViewToClip;   // no lens distortion
    c.jitterOffset = sl::float2(cam.jitterX, cam.jitterY);
    c.mvecScale = sl::float2(1.0f / float(in.renderW), 1.0f / float(in.renderH));   // motion vectors are in pixels
    c.cameraPinholeOffset = sl::float2(0, 0);
    float pos[3] = { 0, 0, 0 }; camera_from_vp(vp, pos);
    c.cameraPos = sl::float3(pos[0], pos[1], pos[2]);
    c.cameraFwd = sl::float3(vp[12], vp[13], vp[14]);
    c.cameraRight = sl::float3(vp[0] / sx, vp[1] / sx, vp[2] / sx);
    c.cameraUp = sl::float3(-vp[4] / sy, -vp[5] / sy, -vp[6] / sy);
    c.cameraNear = nearZ; c.cameraFar = 1.0e7f;
    c.cameraFOV = 2.0f * atanf(1.0f / sy); c.cameraAspectRatio = sy / sx;
    c.motionVectorsInvalidValue = sl::INVALID_FLOAT;
    c.depthInverted = sl::Boolean::eTrue; c.cameraMotionIncluded = sl::Boolean::eTrue; c.motionVectors3D = sl::Boolean::eFalse;
    c.reset = cam.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    c.orthographicProjection = sl::Boolean::eFalse; c.motionVectorsDilated = sl::Boolean::eFalse; c.motionVectorsJittered = sl::Boolean::eFalse;
    SlCall guard;
    sl::Result r = p_slSetConstants(c, *g_token, kViewport);
    if (r != sl::Result::eOk) { static int n = 0; if (n++ < 3) LOG("FG: slSetConstants failed %d", (int)r); }

    // ---- tags ----
    sl::Extent full{ 0, 0, in.renderW, in.renderH };
    sl::Resource depthRes(sl::ResourceType::eTex2d, in.depth, in.depthState); depthRes.width = in.renderW; depthRes.height = in.renderH; depthRes.nativeFormat = in.depthFormat;
    sl::Resource mvRes(sl::ResourceType::eTex2d, in.mv, in.mvState); mvRes.width = in.renderW; mvRes.height = in.renderH; mvRes.nativeFormat = DXGI_FORMAT_R16G16_FLOAT;
    sl::ResourceTag tags[4] = {
        sl::ResourceTag(&depthRes, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow, &full),
        sl::ResourceTag(&mvRes, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow, &full),
        sl::ResourceTag(nullptr, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent),
        sl::ResourceTag(nullptr, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle::eValidUntilPresent),
    };
    uint32_t n = 2;
    sl::Resource hudRes(sl::ResourceType::eTex2d, in.hudless, in.hudlessState);
    if (in.hudless) { hudRes.width = in.renderW; hudRes.height = in.renderH; hudRes.nativeFormat = in.hudlessFormat; tags[n++] = sl::ResourceTag(&hudRes, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eOnlyValidNow, &full); }
    sl::Extent bbExt{ (uint32_t)(in.vpY < 0 ? 0 : in.vpY), (uint32_t)(in.vpX < 0 ? 0 : in.vpX), in.vpW, in.vpH };
    if (in.vpW && in.vpH && (in.vpW != in.bbW || in.vpH != in.bbH)) tags[n++] = sl::ResourceTag(nullptr, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle::eValidUntilPresent, &bbExt);   // FG only on the game image rectangle
    r = p_slSetTagForFrame(*g_token, kViewport, tags, n, in.cmd);
    if (r != sl::Result::eOk) { static int m = 0; if (m++ < 3) LOG("FG: slSetTagForFrame failed %d", (int)r); }
}

void frame_end(uint32_t) { }

} // namespace fg
