// mgs4_dlss.addon64 - injects NGX DLSS into Metal Gear Solid 4 (Master Collection, bgfx on D3D12).
//
// How it works (see docs/renderer-notes.md):
//  * The port renders the 3D scene (UI included) into "internal resolution" RGBA8 targets with a shared R24G8 depth, then
//    one draw per frame composites the finished, double-buffered final texture into the backbuffer.
//  * DLAA mode: at that composite draw we resolve the sampled texture (bgfx fills its shader-visible heap with
//    CopyDescriptors, which ReShade does not track, so we mirror those copies ourselves), run DLSS on it with the depth
//    that was bound with it, copy the result back over it and natively restore bgfx's command-list state.
//  * Upscaling modes (Quality .. UltraPerformance): every texture created at the internal resolution is shrunk to the
//    DLSS render resolution, viewports/scissors of draws into shrunk targets are scaled, DLSS upscales the shrunk final
//    texture into a full-size output, and the composite draw's SRV descriptor is rewritten to point at that output.
//    Needs InternalRes in the ini (auto-detected and written on the first run) and a restart to change Mode.
//  * NGX-hooking add-ons (RenoDX's DLSS add-on: renodx-dlss.addon64, renodx-dlss5.addon64 in older builds) install hooks when nvngx_dlss.dll loads (inside our first CreateFeature), so the
//    feature is re-created once after a few frames to let them capture CreateFeature.
//  * Still zero motion vectors and zero jitter (camera jitter + camera-only MVs are the next step).
// Log: <game>\logs\mgs4_dlss.log. Config: <game>\mgs4_dlss.ini (Enabled/Sharpness/DebugMode live-reloaded).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>   // IDXGIFactory4::EnumWarpAdapter (NrKick)
#include <imgui.h>
#include <reshade.hpp>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#include "mv_cs.h"   // g_mv_cs[]: compiled src/mv_cs.hlsl (camera-only motion vectors from depth)
#include "mv_vis.h"  // g_mv_vis[]: compiled src/mv_vis.hlsl (debug visualization of the motion vectors)
#include "hudless_cs.h"  // g_hudless_cs[]: compiled src/hudless_cs.hlsl (HUD-less color for frame generation)
#include "resample_cs.h"
#include "depth_stretch_cs.h"   // g_depth_stretch_cs
#include "probe_cs.h"          // g_probe_cs: per-frame 240x135 luminance readback of pipeline stages (Probe=1)
#include "dof_coc_cs.h"        // g_dof_coc_cs / g_dof_pack_cs / g_dof_gather_cs / g_dof_composite_cs: the game's DoF re-applied after DLSS (PostDof=1)
#include "dof_pack_cs.h"
#include "dof_gather_cs.h"
#include "dof_composite_cs.h"
#include "uimask_cs.h"          // g_uimask_cs  // g_resample_cs[]: compiled src/resample_cs.hlsl (DLSS output -> the game's dynamic-resolution sub-rect)
#include "fg.h"      // DLSS Frame Generation via Streamline (fg.cpp)
#include "objmv.h"   // per-object motion vectors via stream output (objmv.cpp)
#include "MinHook.h" // FileTrace hooks CreateFileW/A (MinHook is already linked, for fg.cpp)
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>
#include <cmath>
#include <mutex>

using namespace reshade::api;

// ---- logging / config --------------------------------------------------------------------------------------------
static FILE* g_log = nullptr;
static std::mutex g_logMutex;
static char g_gameDir[MAX_PATH];
static wchar_t g_gameDirW[MAX_PATH];
static char g_iniPath[MAX_PATH];
static int g_cfgEnabled = 1;
static int g_cfgPreset = 11;          // NVSDK_NGX_DLSS_Hint_Render_Preset_K (transformer)
static int g_cfgSharpness100 = 0;
static int g_cfgLogEveryN = 600;
static int g_cfgRecreateAfter = 0;
// The add-on's version, as the log and ReShade's Add-ons page say it. Kept in step with docs/releases.md.
#define MGS4_DLSS_VERSION "1.3.3"
static int g_cfgDebugMode = 0;        // 0 normal, 1 = paint the displayed texture magenta, 2 = bypass DLSS, 3 = trace 3 frames again
static int g_cfgLastDebugMode = 0;
// DebugKey: a virtual key that flips DebugMode between 0 and DebugKeyMode in the game, with no overlay open - for
// showing someone the motion vectors. 0 = no key. Read live like DebugMode itself.
static unsigned g_cfgDebugKey = 0;
static int g_cfgDebugKeyMode = 9;
// PauseKey: a key that stops the world while the game keeps rendering it. The port pauses its simulation when its
// window loses focus (the Windows key) and goes on drawing the frozen scene every frame - the one moment NR, DLAA
// and the native image can be compared on the same picture - but with the window unfocused the ReShade overlay
// takes no input. So the pause is the game's own: on the key the add-on sends the game's window the messages a
// focus loss brings (WM_ACTIVATEAPP, WM_ACTIVATE, WM_KILLFOCUS - PauseMessages picks which), from a thread of its
// own, while the window really stays in front; the matching "active" set resumes. Nothing in the game's state is
// touched directly. A real alt-tab while paused hands the game a real activation, which resumes it: press the key
// twice then. The state lives in memory only - a game never starts paused.
static unsigned g_cfgPauseKey = 0;
static int g_cfgPauseMessages = 7;           // 1 WM_ACTIVATEAPP, 2 WM_ACTIVATE, 4 WM_KILLFOCUS/WM_SETFOCUS
static HWND g_hwnd = nullptr;                // the game's window, from ReShade's swapchain
static bool g_worldPaused = false;
static uint32_t g_pauseToggles = 0;
static NVSDK_NGX_PerfQuality_Value g_cfgMode = NVSDK_NGX_PerfQuality_Value_DLAA;
static char g_cfgModeName[32] = "DLAA";
static uint32_t g_internalW = 0, g_internalH = 0;   // from ini InternalRes (size of the game's render targets)
static uint32_t g_cfgRenderResW = 0, g_cfgRenderResH = 0;   // RenderRes: an explicit source resolution for super-resolution (0 = Mode decides)
static int g_cfgFgMode = 0;           // FrameGen: 0 off, 1 = 2x, 2 = 3x, 3 = 4x, 4 = dynamic (target fps)
static float g_cfgFgTargetFps = 0.0f; // FGTargetFps (dynamic mode; 0 = monitor refresh rate)
static int g_cfgReflex = 1;           // Reflex: 0 off, 1 on, 2 on + boost
static float g_gameVp[4] = { 0, 0, 0, 0 };   // composite draw viewport (x, y, w, h): where the game image sits in the backbuffer

static void logmsg(const char* fmt, ...)
{
    if (!g_log) return;
    std::lock_guard<std::mutex> lock(g_logMutex);
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log); fflush(g_log);
}

// ---- file trace (FileTrace=1) ----------------------------------------------------------------------------------
// Diagnostic for the numbered-section crash (mgs4.exe+0x74C5A3): the game dies in a file-extension lookup whose
// key is null, i.e. strrchr(name, '.') found no dot in the name it was classifying. This logs every file the game
// opens and every one it fails to open, so a crashing stage id can be diffed against a working one. Off by
// default; it costs a log line per open.
static int g_cfgFileTrace = 0;
// AssetTrace=1 is the cheap sibling: one "SCENE-ASSET open|miss f<frame> <path>" line per file opened *under the
// game folder* whose name is not a content hash (the ~1300 hash-named meshes and textures every stage loads
// regardless of scene). What is left - e_d###.bank (the demo, MGS4's own cutscene number), env_<stage>_NN.bank
// (a gameplay entry's environment), the localization tables, the .bk2 videos - identifies the scene from the
// engine's side, without a recording. See docs/scene-identity.md. The frame number orders an open against
// FIRST-3D-FRAME, so a demo loaded at boot can be told from one chained in later.
static int g_cfgAssetTrace = 0;
static void asset_trace(const char* path, bool ok);
typedef HANDLE (WINAPI* PFN_CreateFileW)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HANDLE (WINAPI* PFN_CreateFileA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static PFN_CreateFileW o_CreateFileW = nullptr;
static PFN_CreateFileA o_CreateFileA = nullptr;
static thread_local bool t_inFileTrace = false;   // logmsg must not re-enter the hook
static HANDLE WINAPI hk_CreateFileW(LPCWSTR n, DWORD a, DWORD sh, LPSECURITY_ATTRIBUTES sa, DWORD d, DWORD f, HANDLE t)
{
    HANDLE h = o_CreateFileW(n, a, sh, sa, d, f, t);
    if ((g_cfgFileTrace || g_cfgAssetTrace) && n && !t_inFileTrace) {
        const DWORD err = (h == INVALID_HANDLE_VALUE) ? GetLastError() : 0;
        t_inFileTrace = true;
        if (g_cfgFileTrace) { if (h == INVALID_HANDLE_VALUE) logmsg("FILE MISS (%lu) %ls", err, n); else logmsg("FILE open      %ls", n); }
        if (g_cfgAssetTrace) { char nn[MAX_PATH * 2]; if (WideCharToMultiByte(CP_ACP, 0, n, -1, nn, sizeof(nn), nullptr, nullptr) > 0) asset_trace(nn, h != INVALID_HANDLE_VALUE); }
        t_inFileTrace = false;
        if (h == INVALID_HANDLE_VALUE) SetLastError(err);
    }
    return h;
}
static HANDLE WINAPI hk_CreateFileA(LPCSTR n, DWORD a, DWORD sh, LPSECURITY_ATTRIBUTES sa, DWORD d, DWORD f, HANDLE t)
{
    HANDLE h = o_CreateFileA(n, a, sh, sa, d, f, t);
    if ((g_cfgFileTrace || g_cfgAssetTrace) && n && !t_inFileTrace) {
        const DWORD err = (h == INVALID_HANDLE_VALUE) ? GetLastError() : 0;
        t_inFileTrace = true;
        if (g_cfgFileTrace) { if (h == INVALID_HANDLE_VALUE) logmsg("FILE MISS (%lu) %s", err, n); else logmsg("FILE open      %s", n); }
        if (g_cfgAssetTrace) asset_trace(n, h != INVALID_HANDLE_VALUE);
        t_inFileTrace = false;
        if (h == INVALID_HANDLE_VALUE) SetLastError(err);
    }
    return h;
}
static void install_file_trace()
{
    if (!g_cfgFileTrace && !g_cfgAssetTrace) return;
    MH_Initialize();   // already initialized when frame generation is on; harmless either way
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    if (!k32) { logmsg("FileTrace: kernel32 not loaded"); return; }
    void* w = (void*)GetProcAddress(k32, "CreateFileW");
    void* a = (void*)GetProcAddress(k32, "CreateFileA");
    const bool okw = w && MH_CreateHook(w, (void*)hk_CreateFileW, (void**)&o_CreateFileW) == MH_OK && MH_EnableHook(w) == MH_OK;
    const bool oka = a && MH_CreateHook(a, (void*)hk_CreateFileA, (void**)&o_CreateFileA) == MH_OK && MH_EnableHook(a) == MH_OK;
    logmsg("%s on: CreateFileW %s, CreateFileA %s", g_cfgFileTrace ? "FileTrace" : "AssetTrace", okw ? "hooked" : "FAILED", oka ? "hooked" : "FAILED");
}

// ---- per-command-list state (what bgfx has bound) ----------------------------------------------------------------
struct cl_state {
    resource rt = { 0 }, ds = { 0 };
    resource_view rtvs[8] = {}; uint32_t rtv_count = 0; resource_view dsv = { 0 };   // as bound (to restore after a replayed draw)
    uint32_t rt_w = 0, rt_h = 0;
    bool rt_scaled = false;             // current RT/DS is one of the shrunk textures
    viewport vp = {};                   // as set by the game (unscaled)
    rect sc = {};
    bool vp_valid = false, sc_valid = false;
    descriptor_table tables[5] = {};
    bool table_set[5] = {};
    D3D12_GPU_VIRTUAL_ADDRESS cbv[5] = {};
    bool cbv_set[5] = {};
    resource cbv_res[5] = {};           // buffer + offset behind each root CBV (for reading draw constants)
    uint64_t cbv_off[5] = {};
    ID3D12RootSignature* root_sig = nullptr;
    ID3D12PipelineState* pso = nullptr;
    uint32_t bb_draws = 0;
    resource vb0 = { 0 }; uint64_t vb0_off = 0;   // first vertex buffer / index buffer (draw identity across frames)
    resource ib = { 0 }; uint64_t ib_off = 0;
    uint32_t topology = 0;                        // D3D primitive topology as set by the game
};
static std::unordered_map<command_list*, cl_state> g_cl;
static std::mutex g_clMutex;

static ID3D12Device* g_d3d = nullptr;
static void reload_config();
static bool g_dumpPending = false;
static bool tracing();
static bool scene_sized(device* dev, resource r, resource_desc* out = nullptr);
static std::string desc_str(device* dev, resource r);

// ---- resolution scaling (upscaling modes) ---------------------------------------------------------------------------
static uint32_t g_renderW = 0, g_renderH = 0;       // DLSS render resolution (== internal in DLAA mode)
static bool g_scaling = false;                      // render != internal
static std::unordered_set<uint64_t> g_scaledTex;    // resources created at the shrunk size
static std::unordered_set<uint64_t> g_liveTex;      // every live 2D texture the game created (handles are reused after destruction!)
static std::mutex g_scaledMutex;
static bool is_live(uint64_t h) { if (!h) return false; std::lock_guard<std::mutex> lock(g_scaledMutex); return g_liveTex.count(h) != 0; }
static thread_local bool t_reentrant = false;       // guards our own viewport/scissor re-issues

static bool is_scaled(resource r)
{
    if (!r.handle) return false;
    std::lock_guard<std::mutex> lock(g_scaledMutex);
    return g_scaledTex.count(r.handle) != 0;
}

// ---- CPU access to the game's upload-heap constant buffers ---------------------------------------------------------
// bgfx writes its draw constants into upload-heap buffers that stay alive for the run. Every scene draw used to Map /
// Unmap its region (hundreds of pairs per frame on the render thread); the buffers are now mapped once and the pointer
// kept until the resource is destroyed (D3D12 allows nested maps, and upload memory on PC is coherent). nullptr = not
// CPU-readable (default heap) - remembered too, so the heap query happens once per buffer as well.
struct mapped_buf { uint8_t* ptr; uint64_t size; };
static std::unordered_map<uint64_t, mapped_buf> g_mapped;
static std::mutex g_mapMutex;
static uint8_t* map_upload(ID3D12Resource* r, uint64_t* size)
{
    const uint64_t key = reinterpret_cast<uint64_t>(r);
    {
        std::lock_guard<std::mutex> lock(g_mapMutex);
        auto it = g_mapped.find(key);
        if (it != g_mapped.end()) { *size = it->second.size; return it->second.ptr; }
    }
    mapped_buf mb = { nullptr, r->GetDesc().Width };
    D3D12_HEAP_PROPERTIES hp = {}; D3D12_HEAP_FLAGS hf = {};
    const HRESULT hr = r->GetHeapProperties(&hp, &hf);
    const bool readable = SUCCEEDED(hr) && (hp.Type == D3D12_HEAP_TYPE_UPLOAD || (hp.Type == D3D12_HEAP_TYPE_CUSTOM && hp.CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE));
    if (readable) { void* p = nullptr; if (SUCCEEDED(r->Map(0, nullptr, &p))) mb.ptr = static_cast<uint8_t*>(p); }
    std::lock_guard<std::mutex> lock(g_mapMutex);
    g_mapped[key] = mb;
    *size = mb.size;
    return mb.ptr;
}
static void forget_mapped(uint64_t handle) { std::lock_guard<std::mutex> lock(g_mapMutex); g_mapped.erase(handle); }

// ---- descriptor resolution ----------------------------------------------------------------------------------------
// ReShade registers views created with Create*View under their ORIGINAL CPU descriptor handle, but for CBV/SRV/UAV heaps
// its CopyDescriptors hooks do NOT register the destination slots (they only fire copy_descriptor_tables). bgfx fills its
// shader-visible heap by copying, so we mirror those copies ourselves: original CPU handle of the slot -> resource.
static std::unordered_map<uint64_t, uint64_t> g_copiedViews;
static std::mutex g_cvMutex;

static bool table_to_cpu(device* dev, descriptor_table t, uint32_t binding, uint64_t* cpu, uint64_t* size, D3D12_DESCRIPTOR_HEAP_TYPE* type, char* diag = nullptr, size_t diagLen = 0)
{
    if (!t.handle || !g_d3d) return false;
    if ((t.handle & 0xF000000000000000ull) == 0xF000000000000000ull && (((t.handle & ~0xF000000000000000ull) >> 28) >= 4096)) return false;
    descriptor_heap heap = { 0 }; uint32_t off = 0;
    dev->get_descriptor_heap_offset(t, 0, 0, &heap, &off);
    if (!heap.handle) { if (diag) snprintf(diag, diagLen, "heap? handle=%llx", (unsigned long long)t.handle); return false; }
    ID3D12DescriptorHeap* h = reinterpret_cast<ID3D12DescriptorHeap*>(heap.handle);
    // Heap type, increment and CPU start are fixed for the life of a heap; this runs for every one of bgfx's ~700
    // descriptor copies per frame and for every SRV resolved from a draw's table, so they are looked up once per heap.
    struct heap_info { D3D12_DESCRIPTOR_HEAP_TYPE type; uint32_t inc, num; uint64_t cpuStart; };
    static std::unordered_map<uint64_t, heap_info> cache; static std::mutex cacheMutex;
    heap_info hi;
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        auto it = cache.find(heap.handle);
        if (it != cache.end()) hi = it->second;
        else {
            const D3D12_DESCRIPTOR_HEAP_DESC hd = h->GetDesc();
            hi = { hd.Type, g_d3d->GetDescriptorHandleIncrementSize(hd.Type), hd.NumDescriptors, (uint64_t)h->GetCPUDescriptorHandleForHeapStart().ptr };
            if (cache.size() > 256) cache.clear();   // heaps are few and long-lived; a destroyed heap's handle is simply refreshed
            cache[heap.handle] = hi;
        }
    }
    *type = hi.type;
    *size = hi.inc;
    *cpu = hi.cpuStart + (uint64_t(off) + binding) * (*size);
    if (diag) snprintf(diag, diagLen, "heap=%p type=%u n=%u off=%u cpu=%llx", (void*)h, (unsigned)hi.type, (unsigned)hi.num, off, (unsigned long long)*cpu);
    return true;
}
static bool is_live(uint64_t h);
static resource lookup_view(device* dev, uint64_t cpu)
{
    {
        std::lock_guard<std::mutex> lock(g_cvMutex);
        auto it = g_copiedViews.find(cpu);
        if (it != g_copiedViews.end()) { if (is_live(it->second)) return resource{ it->second }; g_copiedViews.erase(it); }   // stale (texture destroyed, e.g. level load)
    }
    resource r = dev->get_resource_from_view(resource_view{ cpu });
    return is_live(r.handle) ? r : resource{ 0 };
}
static resource resolve_descriptor(device* dev, descriptor_table table, uint32_t index, char* diag = nullptr, size_t diagLen = 0)
{
    uint64_t cpu = 0, size = 0; D3D12_DESCRIPTOR_HEAP_TYPE type;
    if (!table_to_cpu(dev, table, index, &cpu, &size, &type, diag, diagLen)) return { 0 };
    if (type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) return { 0 };
    return lookup_view(dev, cpu);
}

// ---- per-frame tracking --------------------------------------------------------------------------------------------
static uint32_t g_frame = 0;

// The filter behind AssetTrace: paths under the game folder (absolute, or relative as the engine mostly asks for
// them) whose file name is not an 8-hex-digit content hash. The game's own logs and the ini are not assets.
static void asset_trace(const char* path, bool ok)
{
    if (!path || !*path) return;
    if (path[0] == '\\' && path[1] == '\\' && path[2] == '?' && path[3] == '\\') path += 4;   // \\?\D:\... is how the sound banks are asked for
    const char* rel = path;
    const size_t gl = strlen(g_gameDir);
    if (_strnicmp(path, g_gameDir, gl) == 0 && (path[gl] == '\\' || path[gl] == '/')) rel = path + gl + 1;
    else if ((path[0] && path[1] == ':') || path[0] == '\\' || path[0] == '/') return;   // elsewhere on the disk
    if (_strnicmp(rel, "logs\\", 5) == 0 || _strnicmp(rel, "logs/", 5) == 0 || _strnicmp(rel, "crash_dumps", 11) == 0) return;
    const char* base = rel;
    for (const char* q = rel; *q; ++q) if (*q == '\\' || *q == '/') base = q + 1;
    size_t hex = 0; while (isxdigit((unsigned char)base[hex])) ++hex;
    if (hex == 8 && base[8] == '.') return;             // 0003a157.mdn: a content hash, loaded by every scene
    const char* ext = strrchr(base, '.');
    if (ext && (_stricmp(ext, ".ini") == 0 || _stricmp(ext, ".log") == 0 || _stricmp(ext, ".dll") == 0 || _stricmp(ext, ".addon64") == 0)) return;
    if (!*rel) return;
    // once per path: a streaming bank is reopened a hundred times in ten seconds, and only the first open says
    // anything (its frame number is what places it before or after the scene's first frame)
    static std::mutex seenMutex; static std::unordered_set<std::string> seen;
    { std::lock_guard<std::mutex> lk(seenMutex); std::string key(rel); for (char& c : key) c = (char)tolower((unsigned char)c); if (!seen.insert(key).second) return; }
    logmsg("SCENE-ASSET %s f%u %s", ok ? "open" : "miss", g_frame, rel);
}
static uint32_t g_bbW = 0, g_bbH = 0;
static std::unordered_set<uint64_t> g_backbuffers;
static uint32_t g_sceneDrawsThisFrame = 0;
static bool g_injectedThisFrame = false;
static bool g_windowInjectedThisFrame = false;   // DLSS ran on a 3D window's own target this frame (Codec caller)
static uint32_t g_windowInjections = 0;
static uint64_t g_winPostRt = 0;                 // where the window target's first reader draws (its post-process target)
static bool g_finalPreHudThisFrame = false;   // DLSS ran on the final texture before its first HUD draw (composite mode + HUD)
static uint32_t g_finalPreHudInjections = 0;
static bool g_skipThisDraw = false;           // DebugMode=7 in that mode: the HUD draws are dropped (HUD-less view)

static std::unordered_map<uint64_t, uint32_t> g_drawsPerRt;
static std::unordered_map<uint64_t, uint32_t> g_drawsPerDs;
static std::unordered_map<uint64_t, uint64_t> g_dsForRt;
static uint32_t g_traceUntil = 0;
static bool g_traceArmed = false;
static uint32_t g_viewEvents = 0, g_copyEvents = 0, g_viewSamples = 0;

// ---- DLSS state ----------------------------------------------------------------------------------------------------
static bool g_ngxInitTried = false, g_ngxReady = false;
static NVSDK_NGX_Parameter* g_ngxParams = nullptr;
static NVSDK_NGX_Parameter* g_ngxCaps = nullptr;      // capability parameters (carry the optimal-settings callback)
static NVSDK_NGX_Handle* g_dlss = nullptr;
static uint32_t g_dlssW = 0, g_dlssH = 0, g_dlssOutW = 0, g_dlssOutH = 0;
static format g_dlssFmt = format::unknown;
static resource g_mv = { 0 }, g_out = { 0 };
// Frozen screens (pause menu, Codec): the frame before the world stops, the game downsamples the final texture into a
// 1920x1080 seed (after its upscale, before the HUD - and before the pre-HUD insertion) and blits that seed back over
// the final texture every frame the screen stays frozen. DLSS therefore runs before that capture (FrozenBackground=1).
static uint32_t g_frozenInjections = 0;
// ---- DoF after DLSS / NR (PostDof=1) ---------------------------------------------------------------------------------
// The port applies its depth of field inside the scene target before the tonemap / upscale into the final texture, so
// DLSS and the DLSS 5 NR add-on only ever see the defocused image: out-of-focus characters carry no NR detail and the
// look "pops" on every rack focus. With PostDof the three DoF draws are skipped (identified by their pixel-shader
// bytecode hash), the CoC constants and the depth copy are taken from the skipped pass, and an exact transcription of
// the three passes runs on the DLSS output before the copy back (see dof_*.hlsl).
static int g_cfgPostDof = 0;
static float g_cfgDofStep = 2.0f, g_cfgDofRadius = 1.0f; static int g_cfgDofMask = 1;   // DofMask=0: no overlay replay / mask (diagnostics)
static int g_cfgDofJitterSign = 1;   // DofJitterSign: the depth copy is rendered with this frame's camera jitter while the DLSS output is de-jittered; +1 reads the depth at the jittered position (measured: blur-boundary wobble 7.7k -> vs 9.7k ppm without, 11.9k with -1), 0 = off
static const uint64_t PS_DOF_COC = 0xbf2a546d733f4efcull, PS_DOF_GATHER = 0x9feb2d2e92bbc108ull, PS_DOF_COMPOSITE = 0x01978e62bca9c941ull;
static float g_dofCb[10 * 4] = {}; static bool g_dofCbValid = false;   // the game's cb0[8..17] at this frame's CoC pass
static float g_dofCbG[10 * 4] = {}; static bool g_dofCbGValid = false; // ... and at its spiral gather pass (each draw has its own constants)
static uint64_t g_dofDepth = 0;                                          // the depth copy the CoC pass sampled (t1)
static bool g_dofCocSeen = false, g_dofSeenThisFrame = false, g_dofCocUnorm = false;
static bool g_dofPrevOk = false;
static bool g_dofSkipFrame = false;      // decided at this frame's CoC draw: the game's DoF draws are skipped (re-applied after DLSS)
static float g_dofKx = 1.0f, g_dofKy = 1.0f;   // this frame's dynamic-resolution scale, exact: the CoC pass viewport is half the scene sub-rect
static int g_cfgDofSubRect = 1;          // 1 = PostDof also on sub-rect frames (depth / step / mask scaled by the exact k), 0 = leave those to the game
static uint32_t g_dofSubRectFrames = 0;
// Overlays the game draws AFTER its DoF (title cards, captions: quads into the graded scene texture between the DoF
// combine and the upscale into the final texture) must stay sharp: they are replayed into a mask layer that the
// composite pass excludes from the blur.
static const uint64_t PS_DOF_COMBINE = 0xa3e1f0c86ca4e96dull;
static bool g_dofCombineSeen = false, g_dofMaskCleared = false;
static resource g_dofMask = { 0 }; static resource_view g_dofMaskRtv = { 0 }; static resource_usage g_dofMaskState = resource_usage::render_target; static format g_dofMaskFmt = format::unknown;
static uint32_t g_dofOverlaysThisFrame = 0, g_dofOverlays = 0;
// Pre-warm (PreWarm=1): the DLSS feature (and with it the DLSS 5 NR add-on's feature, created inside our CreateFeature)
// plus a few evaluations are done on frames without a 3D scene - the title / loading screens - so the model setup and
// first-evaluation stalls do not land in the first half second of the first cutscene.
static int g_cfgPreWarm = 1;
static bool g_warmDone = false; static uint32_t g_warmEvals = 0, g_noSceneFrames = 0;
// ---- pipeline probe (Probe=1): programmatic layout verification ---------------------------------------------------
// Each frame, up to three pipeline stages are downsampled to 240x135 luminance on the GPU and read back through an
// 8-frame ring. On the CPU every image is scanned for a dynamic-resolution sub-rect signature: a sharp vertical /
// horizontal gradient wall at x = k*W (k in 0.4..0.97). Stages: 0 = the color DLSS evaluates, 1 = our output after
// the DoF re-apply, 2 = the texture the game's composite actually samples (what is displayed).
static int g_cfgProbe = 0;
static ID3D12PipelineState* g_probePso = nullptr; static ID3D12DescriptorHeap* g_probeHeap = nullptr;
static ID3D12Resource* g_probeRb = nullptr; static uint8_t* g_probeRbPtr = nullptr;
static resource g_probeTex[4] = {}; static resource_usage g_probeTexState[4] = { resource_usage::unordered_access, resource_usage::unordered_access, resource_usage::unordered_access, resource_usage::unordered_access };
static uint32_t g_probeMeta[4][8] = {};
static uint32_t g_probeSlot = 0;
static bool g_probeReady = false, g_probeInitTried = false;
static uint32_t g_probeFlags[4] = {}, g_probeFrames = 0, g_probeLogs = 0;
static const uint32_t PROBE_W = 240, PROBE_H = 135, PROBE_PITCH = 256, PROBE_SLOT_BYTES = 35328;   // 512-aligned slot
static void probe_dispatch(command_list* cmd, resource src, resource_usage srcState, int stage);
static bool g_dofReady = false, g_dofInitTried = false;
static uint32_t g_dofSkipped = 0, g_dofFrames = 0, g_dofMissed = 0;      // draws skipped / frames re-applied / frames skipped but not re-applied
// The circle of confusion is evaluated at the game's own CoC draw (the depth copy, constants and viewport are exactly
// the game's at that moment) into a half-res R16F texture on the full grid; the insertion only adds the color.
static resource g_dofCocOnly = { 0 }; static resource_usage g_dofCocOnlyState = resource_usage::unordered_access;
static bool g_dofCocReadyThisFrame = false; static uint32_t g_dofCocFrame = 0;   // this frame's CoC was written (and for which frame)
static uint32_t g_dofFallbacks = 0;                                              // frames whose inputs could not be gathered at the CoC draw: the game's DoF ran
// Every per-frame GPU input of the DoF passes (constants, descriptors) is ring-buffered: the CPU records frame N+1
// (and with frame generation N+2) while the GPU still runs frame N's passes, so nothing frame N reads may be rewritten.
static const uint32_t DOF_RING = 8, DOF_CB_STRIDE = 512, DOF_CB_PER_FRAME = 2, DOF_DESC_PER_FRAME = 16;
static const uint32_t MV_HEAP_SLOTS = 32;   // 4-descriptor slots of the motion-vector / utility passes in the shared heap (24-31: FG hint rescale); the DoF ring follows them
static uint32_t g_lateSceneWrites = 0;
static bool g_frameKStep = false;        // this frame the dynamic-resolution scale stepped or the chain disagreed about it
static int g_cfgDofStepFreeze = 1;       // 1 = on such frames skip the evaluation and show the previous DLSS output (one-frame hold instead of the game's inconsistent frame)
static uint32_t g_stepFreezes = 0;   // scene writes into the final AFTER this frame's pre-HUD insertion: the insertion fired too early
static resource g_dofCoc = { 0 }, g_dofBlur = { 0 }; static resource_usage g_dofCocState = resource_usage::unordered_access, g_dofBlurState = resource_usage::unordered_access;
static uint32_t g_dofW = 0, g_dofH = 0;
static bool g_prevFrameHadScene = false;       // the previous frame rendered a full-frame 3D scene
// Frozen screens hold an image DLSS (+NR) already produced: the seed, or the other final texture, or simply the final
// texture left as it was. Evaluating DLSS on such a frame would run NR a second time on it (visible as a sharpness
// jump for the frames before the pause menu's 3D window appears, for the whole Codec list, and while the controller
// connect / disconnect message is up), so those frames are passed through instead.
static std::unordered_set<uint64_t> g_seedTex;   // small targets a few-vertex draw wrote from a final texture (the seed captures)
static uint64_t g_finalSceneSrc = 0;             // slot-0 texture of this frame's scene write into the final texture
static bool g_frozen = false;                    // the world stopped through the game's seed capture; cleared by a fresh scene write
// The seed is a 1920x1080 downsample, so even with DLSS in it the frozen background is softer than the live frame.
// A full-size copy of the DLSS output is kept at the capture and the game's seed blit is redirected to sample it.
static resource g_keep = { 0 }; static resource_usage g_keepState = resource_usage::copy_dest;
static uint64_t g_keepSeed = 0; static bool g_keepValid = false; static uint32_t g_keepRedirects = 0;
static uint32_t g_wipeRedirects = 0;   // cutscene captures of the final texture redirected to the previous frame's DLSS+DoF output (the WIPE transitions display them; pre-insertion the final texture has no DoF)
static bool g_freshWrite = false;                // this frame the final texture received a scene write from a live source (geometry target, video)
static uint32_t g_frozenPassFrames = 0;
// Discontinuities in what DLSS sees: after a pass-through frame, or when the insertion switches between the final
// texture and a 3D window's target, the history holds something else entirely (the Codec caller's first frame came out
// warped: stale history reprojected with meaningless vectors). The next evaluation resets.
static bool g_forceReset = false; static int g_lastEvalWindow = -1; static uint32_t g_discontResets = 0;
static uint32_t g_fgCutFrames = 0;   // evaluations after a discontinuity during which DLSS-G is told 'cut' (no interpolation)
// Resuming from a frozen screen (unpause, a dismissed dialog) with the camera where it was: the DLSS / NR history from
// the last live frame is still valid, so nothing is reset; only the rectangle a 3D window (pause-menu model) occupied
// in the meantime is excluded from the history for that frame (bias-current-color mask), since window-mode
// evaluations overwrote the history there.
static float g_liveVP[16]; static bool g_haveLiveVP = false;
static viewport g_lastWinRect = {}; static bool g_lastWinRectValid = false;
static uint32_t g_resumesKept = 0;
static bool camera_position(const float* m, float* out);
// Camera-cut heuristic: a history reset when the orientation turns by more than ~20 degrees in a frame, or the position
// jumps by more than CutPosLimit units. The game's units are millimeters (near plane ~49, camera ~30 m from the origin):
// the old limit of 1500 fired on every 2 m camera snap - aiming in and out, cover - a third of all resets in gameplay,
// each a frame of raw aliasing on the characters. Real cuts turn the camera as well; 6000 keeps them.
static float g_cfgCutPosLimit = 6000.0f;
static bool camera_close(const float* a, const float* b)
{
    float pa[3], pb[3];
    const bool okA = camera_position(a, pa), okB = camera_position(b, pb);
    const float dp = okA && okB ? sqrtf((pa[0] - pb[0]) * (pa[0] - pb[0]) + (pa[1] - pb[1]) * (pa[1] - pb[1]) + (pa[2] - pb[2]) * (pa[2] - pb[2])) : 0.0f;
    const float dr = 1.0f - (a[12] * b[12] + a[13] * b[13] + a[14] * b[14]);
    return dr <= 0.06f && dp <= g_cfgCutPosLimit;
}

static int g_cfgTraceFreeze = 0; static uint32_t g_freezeTracedAt = 0;
// Freeze trace (TraceFreeze=1): the full-size draw / copy chain of the last frames is kept in a ring and written to the
// log when the world stops rendering, followed by the next non-empty frames. (The game stalls for a few empty frames
// at the freeze while it loads the menu, so a fixed frame window there logged nothing.)
struct frz_line { uint32_t frame; std::string text; };
static std::deque<frz_line> g_frzRing; static std::mutex g_frzMutex;
static uint32_t g_frzTraceFrames = 0;   // non-empty frames still to log after the freeze
static bool g_frzDumping = false;
static uint32_t g_frzFreezes = 0;
static void frz_record(const char* fmt, ...)
{
    if (!g_cfgTraceFreeze) return;
    char b[640]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
    if (g_frzDumping || tracing()) { logmsg("FRZ %s", b); return; }
    std::lock_guard<std::mutex> lock(g_frzMutex);
    g_frzRing.push_back(frz_line{ g_frame, b });
    if (g_frzRing.size() > 4000) g_frzRing.pop_front();
}
static resource g_depthFull = { 0 }; static resource_usage g_depthFullState = resource_usage::unordered_access;   // scene depth stretched to the full grid (dynamic resolution)
static resource_usage g_outState = resource_usage::unordered_access;
static uint32_t g_evalCount = 0, g_failCount = 0;
static bool g_featureCreatedThisFrame = false;
static uint32_t g_createdFrame = 0;
static bool g_recreated = false;
static bool g_recreateRequested = false;   // overlay changed the preset
static uint32_t g_autoRecreateAt = 0;       // an NGX-hooking add-on installed its hooks during our first CreateFeature: re-create once at this evaluation count
static NVSDK_NGX_Handle* g_oldFeature = nullptr;
static uint32_t g_oldFeatureFrame = 0;
static ULONGLONG g_evalRateT0 = 0; static uint32_t g_evalRateN = 0; static float g_evalRate = 0;
static float g_lastFrameDeltaMs = 0.0f;   // the most recent game-frame interval (measured at the rollover): DLSS's frame-time hint, 0 = unknown

// ---- Phase 1b: camera jitter ---------------------------------------------------------------------------------------
// Scene draws carry a row-major clip matrix (rows = clip x,y,z,w) in their vertex constants: c[0..3] for the main
// geometry shaders, c[1..4] for others. Signature: the w-row's xyz is unit length (view-space depth direction) and the
// z-row has no x/y (reversed-Z: z_clip = near). We add the sub-pixel jitter to the x/y rows in place
// (x' = x + jx_ndc * w) once per constant region and report the same offset to DLSS.
static int g_cfgJitter = 1;
static int g_cfgMotionVectors = 1;
static int g_cfgPrePostMode = -1;            // -1 auto (pre-post unless the DLSS 5 NR add-on is loaded), 0 off, 1 on
static int g_cfgPrePost = 1;                 // effective: DLAA runs before the post-process/HUD passes
static bool g_nrAddonLoaded = false;         // one of the CompositeIfLoaded modules is present in the process
static char g_nrAddonName[64] = "";          // which one
// NrPreload (default 1): with RenoDX's DLSS add-on in the process, load NVIDIA's nvngx_dlssnr.dll from the game
// folder ourselves, once, as soon as the add-on is seen. RenoDX binds its NR runtime lazily and, with Streamline
// present, has been seen never getting round to it on its own (BindDevice "unavailable runtime state" every frame
// until a setting was changed in its tab); it hooks module loads and says its NGX hooks attach "when
// reconstruction modules appear", so the snippet appearing is the nudge this tries. Harmless when it is not:
// RenoDX loads the same file itself when it does attach.
static int g_cfgNrPreload = 1;
static bool g_nrPreloadDone = false;
// NrKick (default 1): RenoDX's DLSS add-on only attaches its Neural Rendering runtime (nvngx_dlssnr.dll) from three
// places - its own load (too early: no device, no settings), ReShade's init_device event (which it never received
// in any run here) and a change in its settings tab. Until then every NR evaluation is refused with "BindDevice
// rejected unavailable runtime state", so NR did nothing until the user flipped a setting. The kick creates a
// WARP D3D12 device through ReShade's hooked D3D12CreateDevice once the game's swapchain exists: ReShade raises
// init_device for it, RenoDX's handler runs its attach with the settings loaded, and the runtime binds to the
// game's device on the next evaluation. The WARP device is kept alive for the whole run (RenoDX resets its runtime
// state on destroy_device) and this add-on ignores it.
static int g_cfgNrKick = 1;
static bool g_nrKickDone = false;
static bool g_nrKickInProgress = false;
static ID3D12Device* g_nrKickDevice = nullptr;
// Phase 2: dynamic-object mask. Draws whose constants do not carry the camera VP at c[0] (characters, props) are replayed
// into a private depth buffer; the MV pass turns that into DLSS's bias-current-color mask (and optionally zero motion).
static int g_cfgDynMask = 0;
static int g_cfgUiMask = 1;
static int g_cfgWindowScene = 1;
static int g_cfgFrozenBg = 1;                // frozen screens (pause / Codec): run DLSS before the game captures its background seed
static int g_cfgDynZeroMV = 1;
static resource g_dynDepth = { 0 }; static resource_view g_dynDsv = { 0 };
static resource_usage g_dynState = resource_usage::depth_stencil_write;
// HUD layer for frame generation in composite mode: the game's HUD draws are replayed into this RGBA target (cleared to
// zero each frame) and handed to DLSS-G as UI color + alpha, so generated frames get the HUD re-composited unwarped.
static resource g_ui = { 0 }; static resource_view g_uiRtv = { 0 };
static resource_usage g_uiState = resource_usage::render_target;
static uint32_t g_uiDrawsThisFrame = 0, g_uiDrawsLast = 0, g_uiPostSkippedThisFrame = 0, g_uiPostSkippedLast = 0;
// Scene state classification (always on): HUD draws are the depth-off draws into the final texture, after the 3D
// scene, that sample no scene-sized texture (post-process passes do). No HUD + a 3D scene = in-game cutscene;
// HUD + a 3D scene = gameplay; no 3D scene at all = menu, loading screen or a prerecorded video playing.
static uint32_t g_hudDrawsThisFrame = 0, g_hudDrawsLast = 0;
static int g_sceneState = -1, g_sceneStateRaw = -1; static uint32_t g_sceneStateFrames = 0;
static uint32_t g_sceneDrawsLast = 0;   // scene draws of the frame just finished (the counter itself is reset each frame)
static int g_cfgSceneLog = 1;
static int g_cfgHudMin = 25;   // a cutscene can issue a handful of 2D draws; gameplay's HUD is dozens
static bool g_uiClearedThisFrame = false;
static bool g_finalSceneWritten = false;      // this frame's scene has been written into the final texture (HUD comes after)
static uint64_t g_finalSceneRt = 0;           // ... and into which texture (the final image is double-buffered)
static uint32_t g_uiPreSceneThisFrame = 0;
// pre-HUD capture of the final texture (taken right before the first HUD draw) and the HUD-less image built from the
// DLAA output + that capture under the UI layer (DLSS-G derives the UI from backbuffer - HUD-less when its own UI
// recomposition is unavailable, e.g. with the NVIDIA app's frame-generation preset override)
static resource g_preHud = { 0 }, g_hudless = { 0 };
static resource_usage g_preHudState = resource_usage::copy_dest, g_hudlessState = resource_usage::copy_dest;
static bool g_preHudCaptured = false;
static uint32_t g_hudlessFrames = 0;
static resource g_mask = { 0 };
static resource_usage g_maskState = resource_usage::unordered_access;
static uint64_t g_lastDepth = 0;             // depth buffer DLSS used last frame (only draws with it bound are replayed)
static int g_cfgObjectMV = 1;                // per-object motion vectors (stream-out of the game's vertex shaders), see objmv.h
static int g_cfgObjMvProps = 0;              // ObjectMVProps: also capture rigid props with their own model matrix (vehicles, the Mk. II, doors), not only skinned meshes
// Object vectors are accepted per fragment by the velocity pixel shader only when plausible: at most ObjectMVMaxPixels of
// motion and at most ObjectMVMaxGradient pixels of motion change per pixel across the surface (velocity_ps.hlsl). A
// wrong pairing of stream-out captures - another instance of the mesh, the same mesh in another projection - fails one
// of the two and the pixel keeps its camera vector; it showed as a one-frame flash on characters that frame generation
// amplified. Relative to the camera vector was wrong: a followed third-person player legitimately differs from the
// camera vector by the full parallax of a near object.
static int g_cfgObjMvMaxPixels = 200, g_cfgObjMvMaxGradient = 4;
// FGHintRescale=1: in a window that is not the render size, rescale the HUD-less / UI hints to the backbuffer size for
// DLSS-G (two 4K resample passes at the composite draw, plus DLSS-G's own UI work). Off by default: the GPU budget for
// generated frames is what is left of the game's 16.7 ms, and the hints' benefit was not visible in testing.
static int g_cfgFgHintRescale = 0;
static viewport g_sceneVp = {};              // viewport of the last dynamic scene draw (the port can render into a sub-viewport of its targets)
static bool g_sceneVpValid = false;
static viewport g_sceneVpFrame = {}; static bool g_sceneVpFrameValid = false;   // most common viewport of this frame's depth draws into the scene target
static viewport g_winVpFrame = {}; static bool g_winVpFrameValid = false;       // the same for 'window' viewports (smaller than half the frame or at an offset): Codec / pause 3D windows
static std::unordered_map<uint64_t, std::pair<uint32_t, viewport>> g_winVpHist;
// Layout windows (mission briefings). The frame's 3D scene does not always fill the image: the Act 2 briefing renders
// its main view into a 2562x1440 window at the top-left of the frame (other layouts: 2284x2160, 3168x1782, ...) with the
// camera windows and text panels around it, and the port's dynamic resolution scales that window on top (2562x1440 ->
// 2006x1128 at k = 0.78). The scene viewport alone cannot tell a 1:1 window from an upscaled sub-rect; treating the
// window as a sub-rect stretched the depth and every motion vector by 1/k over the whole frame. The game's upscale pass
// into the final texture (a full-viewport 3-vertex draw sampling the scene texture) is scissored to exactly the
// rectangle the scene occupies in the final image, in full-grid pixels - the whole texture normally - and its vertex
// constants carry the scale itself (c[0].xy target size, c[1].zw the source extent in UV, c[2].x = k). The scissor is
// read at that draw; the scene viewport divided by it is the port's scale, exactly, and with it depth, camera vectors,
// object vectors and the jitter are expressed in the window's own pixels. The rectangle is kept for the next frame's
// scene draws (the jitter is applied before the frame's scene write reveals it).
static viewport g_layoutRect = {}; static uint32_t g_layoutFrame = 0; static bool g_layoutValid = false;   // this frame's (from its scene write)
static viewport g_layoutLast = {}; static bool g_layoutLastValid = false;                                     // the most recent frame's
static float g_layoutK = 1.0f;   // the scale the upscale pass was given (c[2].x), for the log / overlay
static uint32_t g_layoutFrames = 0;   // evaluations inside a layout window (stats)
static uint32_t g_layoutDumpFrames = 0, g_layoutDumps = 0;   // diagnostics: dump the scene writes and viewports of the frames after a layout change
static std::unordered_map<uint64_t, uint32_t> g_sceneClassDrawsPerRt;   // per RT: depth-bound draws at a scene-class viewport (full frame / layout window)
static uint32_t g_geoRepicks = 0;   // frames whose 3D target moved to a target with clearly more scene-class draws (stats)
static std::unordered_map<uint64_t, std::pair<uint32_t, std::pair<uint64_t, viewport>>> g_dumpVpHist;   // diagnostics: (rt, viewport) -> depth-tested draws
static const viewport* layout_now() { if (g_layoutValid && g_layoutFrame == g_frame) return &g_layoutRect; if (g_layoutLastValid) return &g_layoutLast; return nullptr; }
static bool layout_is_window(const viewport& l) { return g_internalW && g_internalH && (l.width < float(g_internalW) - 1.0f || l.height < float(g_internalH) - 1.0f || l.x > 0.5f || l.y > 0.5f); }
static const viewport* layout_window() { const viewport* l = layout_now(); return (l && layout_is_window(*l)) ? l : nullptr; }
// A viewport that is the layout window scaled by the port's dynamic resolution: same origin (the scaled scene keeps the
// window's position), no larger than the window, the window's aspect ratio.
static bool vp_in_layout(const viewport& v, const viewport& L)
{
    if (L.width <= 0 || L.height <= 0 || v.width <= 0 || v.height <= 0) return false;
    if (fabsf(v.x - L.x) > 1.5f || fabsf(v.y - L.y) > 1.5f) return false;
    if (v.width > L.width + 1.5f || v.height > L.height + 1.5f || v.width < L.width * 0.2f) return false;
    const float a = v.width / v.height, la = L.width / L.height;
    return fabsf(a - la) <= la * 0.03f;
}
// The main 3D view of a layout window, whatever the port's scale (below half the frame the plain size test files it as
// a 3D window, and DLSS then flapped between the window insertion and the final texture with a history reset each time).
// Until a layout is known the frame itself is the window, so a scaled main view at the origin counts as the scene too.
static bool vp_is_layout_scene(const viewport& v) { const viewport* l = layout_now(); const viewport full = { 0.0f, 0.0f, float(g_internalW), float(g_internalH), 0.0f, 1.0f }; return g_internalW && vp_in_layout(v, l ? *l : full); }
// A plausible full-frame viewport: at the origin, at least half the target, and the target's aspect. The aspect matters:
// the briefing's video call renders the caller (Naomi, Campbell) into a (0,0 2284x2160) view that is only ever seen on
// the Nomad's monitor, before the main view; without the aspect test it passed for the frame's scene, took the 3D-target
// slot and the layout, and its silhouette was rasterized into the main window while the main view's characters were
// rasterized into its 2284x2160 rectangle.
static bool vp_full_frame(const viewport& v, float rtW, float rtH)
{
    if (rtW <= 0 || rtH <= 0 || v.height <= 0) return false;
    const float a = v.width / v.height, fa = rtW / rtH;
    return v.width >= rtW * 0.5f && v.height >= rtH * 0.5f && v.x <= 1.0f && v.y <= 1.0f && fabsf(a - fa) <= fa * 0.03f;
}
static bool rects_overlap(const viewport& a, const viewport& b) { return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height; }
// The camera window of the briefings (F CAM, (2562,0 1278x900)): the scene write whose scissor fits one of the frame's
// window viewports without overlapping the main view's rectangle; kept for two frames. Its objects rasterize into it and
// its depth (the views share one depth texture) is brought to the full grid there.
static viewport g_winLayoutRect = {}, g_winLayoutVp = {}; static uint32_t g_winLayoutFrame = 0; static bool g_winLayoutValid = false;
static viewport g_winLayoutLast = {}, g_winLayoutLastVp = {}; static uint32_t g_winLayoutLastFrame = 0; static bool g_winLayoutLastValid = false;
static const viewport* win_layout_now(const viewport** vp = nullptr)
{
    if (g_winLayoutValid && g_winLayoutFrame == g_frame) { if (vp) *vp = &g_winLayoutVp; return &g_winLayoutRect; }
    if (g_winLayoutLastValid && g_frame - g_winLayoutLastFrame <= 2) { if (vp) *vp = &g_winLayoutLastVp; return &g_winLayoutLast; }
    return nullptr;
}
static uint32_t g_objMvSkippedOther = 0;   // object captures skipped: the draw belongs to neither the main view nor a known window (stats)
// The camera window's depth. The views share one depth texture and the game clears it whole between passes (dozens of
// whole clears per frame), so by the insertion the window's depth is gone: its characters tested against far and every
// polygon of them showed, through the floor too. The window's region is stretched into the full-grid depth copy at the
// window's first reader (its post pass), the last moment its depth is intact; the main view's stretch at the insertion
// then keeps that region.
static uint32_t g_winDepthFrame = 0; static float g_winDepthRect[4] = {}; static uint32_t g_winDepthCopies = 0;
static std::unordered_map<uint64_t, uint32_t> g_winFitDrawsPerRt; static uint64_t g_winFitRt = 0; static uint32_t g_winFitDraws = 0; static viewport g_winFitVp = {};   // the target / viewport of the draws that fit the camera window's rectangle (the caller's view has more window-class draws at times)
// The video call's caller feed (Naomi, Campbell): a third 3D view at (0,0 2284x2160), rendered first, written into the
// final texture under the main view and copied from there into the texture the Nomad's monitor samples. Its rectangle
// is the scene write that overlaps the main view's and fits one of the frame's window viewports; its target is the one
// drawing at that viewport; its depth is copied at its first reader (like the camera window's) into g_feedDepth; its
// objects' vectors are rasterized into g_feedMv in that rectangle; and the monitor draws of the main view - the draws
// sampling a texture that was copied from the final texture this frame - are captured with their texture coordinates
// and project the feed's motion onto the screen (objmv view 3).
static viewport g_feedLayoutRect = {}, g_feedLayoutVp = {}; static uint32_t g_feedLayoutFrame = 0; static bool g_feedLayoutValid = false;
static viewport g_feedLayoutLast = {}, g_feedLayoutLastVp = {}; static uint32_t g_feedLayoutLastFrame = 0; static bool g_feedLayoutLastValid = false;
static const viewport* feed_layout_now(const viewport** vp = nullptr)
{
    if (g_feedLayoutValid && g_feedLayoutFrame == g_frame) { if (vp) *vp = &g_feedLayoutVp; return &g_feedLayoutRect; }
    if (g_feedLayoutLastValid && g_frame - g_feedLayoutLastFrame <= 2) { if (vp) *vp = &g_feedLayoutLastVp; return &g_feedLayoutLast; }
    return nullptr;
}
static std::unordered_map<uint64_t, uint32_t> g_feedDrawsPerRt; static uint64_t g_feedGeoRt = 0; static uint32_t g_feedGeoDraws = 0; static viewport g_feedVpFrame = {}; static bool g_feedVpFrameValid = false;
static resource g_feedDepth = { 0 }; static resource_usage g_feedDepthState = resource_usage::unordered_access; static uint32_t g_feedDepthFrame = 0, g_feedDepthCopies = 0;
static resource g_feedMv = { 0 }; static resource_view g_feedMvRtv = { 0 }; static resource_usage g_feedMvState = resource_usage::render_target;
static std::unordered_set<uint64_t> g_feedTexSet;   // this frame: targets of the few-vertex copies from the final texture (the monitor's texture among them)
static uint32_t g_feedFrames = 0, g_monitorFrames = 0; static int g_cfgMonitorFlipV = 0;
// MonitorProject=1 (experimental, off): project the caller's vectors through the monitor. The screen is a 5-vertex quad
// whose texture coordinates are not in the first TEXCOORD output (mask 6 in TEXCOORD0, the projector took TEXCOORD1),
// so the motion landed beside the caller. Off, the caller's draws are captured into the feed texture only, i.e. ignored.
static int g_cfgMonitorProject = 0;
static std::unordered_map<uint64_t, std::pair<uint32_t, std::pair<uint64_t, resource_desc>>> g_dumpTexHist;   // diagnostics: textures the main view's draws sample
static std::unordered_set<uint64_t> g_dumpRtSet;   // diagnostics: every target drawn into this frame
static std::unordered_map<uint64_t, uint32_t> g_rtSeen;   // target -> last frame drawn into (the flashback video is a 30 fps clip: written every other frame)
// Flashback footage (mash X at a flashback prompt). The game's post chain gains a pass while a flashback is up: a
// 6-vertex full-viewport quad into a frame-sized target that reads the scene texture, the 512x256 video the game
// uploads (never a render target) and its noise textures, and writes the two composited - before its upscale into the
// final texture. Not an overlay draw: it cannot be lifted out of the chain and drawn after DLSS (that loses the grading
// and the scene under the footage), and left alone DLSS reprojected the footage with the scene's vectors: the scene
// smeared across it (skipping exactly this pass makes the flashback vanish; the 1024x256-strip quad that runs every
// frame in the same place is the film grain). So while the pass is in the frame DLSS takes the current frame for the
// whole picture (the HUD mask's bias-current-color and zero motion everywhere, as for a frame without 3D), like the HUD.
static uint32_t g_flashFrame = 0, g_flashFrames = 0;   // frame the footage pass was last seen in; frames handled (stats)
static bool g_preHudLast = false;                       // last frame ran the pre-HUD insertion on the final texture (the only path with the mask)
static uint64_t g_winGeoRt = 0, g_winGeoRtLast = 0; static uint32_t g_winGeoDraws = 0;   // RT receiving depth-tested draws at a window viewport (Codec caller scene)
static std::unordered_map<uint64_t, uint32_t> g_winGeoDrawsPerRt;
static uint32_t g_depthOnDrawsThisFrame = 0, g_depthOnDrawsLast = 0;      // depth-tested draws with a depth buffer (0 = no 3D scene: Codec)
static bool g_windowMode = false; static viewport g_windowVp = {};            // this frame's DLSS pass: the 3D scene is a window of a frozen screen
static const viewport* layout_window();
static float jitter_ref_w() { if (!g_sceneVpFrameValid && g_winVpFrameValid && g_winVpFrame.width > 0) return g_winVpFrame.width; if (const viewport* l = layout_window()) if (l->width > 0) return l->width; const uint32_t w = g_scaling ? g_renderW : g_internalW; return float(w); }
static float jitter_ref_h() { if (!g_sceneVpFrameValid && g_winVpFrameValid && g_winVpFrame.height > 0) return g_winVpFrame.height; if (const viewport* l = layout_window()) if (l->height > 0) return l->height; const uint32_t h = g_scaling ? g_renderH : g_internalH; return float(h); }
// The scene viewport of the frame being rendered (the game changes it every second under load); the hysteresis copy
// g_sceneVp is only a fallback before this frame's first scene draw.
static const viewport& scene_vp_now() { return g_sceneVpFrameValid ? g_sceneVpFrame : g_sceneVp; }
static bool scene_vp_now_valid() { return g_sceneVpFrameValid || g_sceneVpValid; }
static std::unordered_map<uint64_t, std::pair<uint32_t, viewport>> g_vpHist;   // this frame: (w,h) -> draw count, viewport
// Dynamic resolution: the port renders the scene into a variable sub-viewport of its targets. With DRS=1 the DLSS
// feature is created in a scalable mode, evaluated on the sub-rect and its full-size output is resampled back into it.
static int g_cfgDRS = 1;
static resource g_scratch = { 0 }; static resource_usage g_scratchState = resource_usage::unordered_access;
static uint32_t g_drsMinW = 0, g_drsMinH = 0;    // DLSS dynamic minimum for the feature's mode
static uint32_t g_drsFrames = 0, g_drsSubW = 0, g_drsSubH = 0; static bool g_drsActiveLast = false;
static const viewport& scene_vp_now(); static bool scene_vp_now_valid();
static float drs_factor_x() { if (g_cfgDRS != 2) return 1.0f; const viewport& v = scene_vp_now(); return (g_cfgDRS && scene_vp_now_valid() && g_internalW && v.width > 0 && v.width < g_internalW) ? v.width / float(g_internalW) : 1.0f; }
static float drs_factor_y() { if (g_cfgDRS != 2) return 1.0f; const viewport& v = scene_vp_now(); return (g_cfgDRS && scene_vp_now_valid() && g_internalH && v.height > 0 && v.height < g_internalH) ? v.height / float(g_internalH) : 1.0f; }
static std::unordered_map<uint64_t, resource_view> g_dsvForDs;   // depth texture -> the DSV the game binds it with
static resource_view g_mvRtv = { 0 };
static uint32_t g_objMvFrames = 0;
static uint32_t g_dynDrawsThisFrame = 0, g_dynDrawsLastFrame = 0;
static bool g_dynClearedThisFrame = false;
static uint32_t g_prevBusiestDraws = 0;      // draws into the busiest scene RT last frame
static uint64_t g_prevBusiestRt = 0;
static uint64_t g_prevBusiestRt2 = 0;        // the frame before (geometry targets are double-buffered)
static uint32_t g_prePostInjections = 0, g_compositeInjections = 0;
// DLAA pre-HUD insertion: the final texture (what the composite samples) receives 3D draws (depth-tested PSOs) and then
// the post/HUD 2D draws (depth disabled). DLSS runs at the first 2D draw after the bulk of the 3D draws.
struct pso_info { bool depth = true; bool skinned = false; };
static std::unordered_map<uint64_t, pso_info> g_psoDepth;   // pipeline -> depth test enabled / uses skinning attributes
static std::mutex g_psoMutex;
static uint64_t g_finalRt[2] = { 0, 0 };                 // textures sampled by the composite in the last two frames
static uint32_t g_depthDrawsIntoFinal = 0, g_depthDrawsIntoFinalLast = 0;
static std::unordered_map<uint64_t, uint32_t> g_depthDrawsPerRt;   // this frame: RT -> draws with a depth buffer bound (3D draws)
static uint64_t g_geoRt = 0; static uint32_t g_geoDrawsLast = 0;   // last frame's 3D target (most depth-bound draws) and its count
static uint64_t g_curGeoRt = 0;                                     // this frame's 3D target (detected in-frame: geometry targets are multi-buffered)
static int g_cfgDynMaskProps = 0;                                   // also mask props with their own model matrix (default: skinned meshes only)
static pso_info pso_get(ID3D12PipelineState* p) { std::lock_guard<std::mutex> lock(g_psoMutex); auto it = g_psoDepth.find((uint64_t)p); return it != g_psoDepth.end() ? it->second : pso_info{}; }
static bool pso_depth_enabled(ID3D12PipelineState* p) { return pso_get(p).depth; }
static uint32_t g_skinnedDrawsThisFrame = 0, g_skinnedDrawsLast = 0;
static float g_cfgJitterSignX = 1.0f, g_cfgJitterSignY = -1.0f;   // NDC y is up, DLSS jitter is reported in pixel space (y down)
static float g_jitterX = 0.0f, g_jitterY = 0.0f;   // pixels, this frame
static uint32_t g_jitterIndex = 0;
// per frame: constant region -> draw class (0 static world, 1 dynamic, 2 unknown) and the head of its constants (the
// per-instance signature the object-vector pairing uses; read once per region along with the jitter patch, so the
// capture path costs no second read of write-combined memory)
struct region_info { int cls = -1; uint32_t anchorN = 0; float anchor[32]; };
static std::unordered_map<uint64_t, region_info> g_patchedRegions;
static const region_info* region_of(const cl_state& s)
{
    if (!s.cbv_set[2] || !s.cbv_res[2].handle) return nullptr;
    auto it = g_patchedRegions.find(s.cbv_res[2].handle ^ (s.cbv_off[2] * 0x9E3779B97F4A7C15ull));
    return it == g_patchedRegions.end() ? nullptr : &it->second;
}
static uint32_t g_patchedDraws = 0, g_matrixMisses = 0;
static float g_frameVP[16] = {}; static bool g_haveFrameVP = false;   // unjittered VP of this frame (majority of c[0] blocks)
static float g_prevVP[16] = {};  static bool g_havePrevVP = false;
struct vp_vote { uint32_t count = 0; uint32_t winCount = 0, mainCount = 0; float m[16]; };   // winCount / mainCount: draws at the camera window's / the main view's viewport
// The camera window's camera. The security feed cycles its cameras (E CAM, F CAM, C CAM...), and every switch is a cut
// inside the window: for that frame the window's characters pair with positions in the old camera's space, and what
// survives the plausibility filter draws as exploded triangles. The window's view-projection is the vote most of its
// draws share; a jump from last frame's drops the window's object vectors for the frame.
static float g_winVP[16] = {}, g_winVPPrev[16] = {}; static bool g_haveWinVP = false, g_haveWinVPPrev = false, g_winCut = false; static uint32_t g_winCuts = 0;
static std::unordered_map<uint64_t, vp_vote> g_vpVotes;              // per frame: hash -> block
static uint32_t g_missLogBudget = 0;

static float halton(uint32_t i, uint32_t b) { float f = 1.0f, r = 0.0f; while (i > 0) { f /= b; r += f * (i % b); i /= b; } return r; }
static void advance_jitter()
{
    if (!g_cfgJitter) { g_jitterX = g_jitterY = 0; return; }
    const float ratio = (g_scaling ? float(g_internalW) / float(g_renderW) : 1.0f) / drs_factor_x();
    const uint32_t phases = (uint32_t)(8.0f * ratio * ratio + 0.5f);
    g_jitterIndex = (g_jitterIndex + 1) % (phases ? phases : 8);
    g_jitterX = halton(g_jitterIndex + 1, 2) - 0.5f;
    g_jitterY = halton(g_jitterIndex + 1, 3) - 0.5f;
}
static bool looks_like_clip_matrix(const float* m)   // m = 16 floats, rows of 4
{
    const float* zr = m + 8; const float* wr = m + 12;
    const float n = sqrtf(wr[0] * wr[0] + wr[1] * wr[1] + wr[2] * wr[2]);
    if (fabsf(n - 1.0f) > 0.05f) return false;
    if (fabsf(zr[0]) > 1e-3f || fabsf(zr[1]) > 1e-3f) return false;
    for (int i = 0; i < 16; ++i) if (!(m[i] == m[i])) return false;
    return true;
}
// The NDC offset the jitter patch adds to this frame's clip matrices (x, y); the previous frame's is kept for the
// velocity pass, which removes both from the captured positions.
static float g_prevJitNdc[2] = { 0, 0 };
// Width/height the jitter refers to: the frame (the post chain upscales any sub-rect to it), or the 3D window when the
// scene is a window of a frozen screen (Codec / pause), where a jitter of jx pixels is 2*jx/windowWidth in NDC.
static float jitter_ref_w();
static float jitter_ref_h();
static void jitter_ndc(float* o)
{
    const float w = jitter_ref_w(), h = jitter_ref_h();
    o[0] = (g_cfgJitter && w > 0) ? g_cfgJitterSignX * 2.0f * g_jitterX / (w * drs_factor_x()) : 0.0f;
    o[1] = (g_cfgJitter && h > 0) ? g_cfgJitterSignY * 2.0f * g_jitterY / (h * drs_factor_y()) : 0.0f;
}
static uint32_t g_jitStat[7] = {};   // 0 calls, 1 no cbv, 2 already patched, 3 map failed, 4 patched, 5 no matrix, 6 patched at a learned deep offset (skinned shaders)
static float g_lastGoodVP[16] = {}; static bool g_haveLastGoodVP = false;   // most recent frame whose camera matrix was found
// Jitters the draw's clip matrix in place and classifies the draw: 0 = static world (camera VP at c[0]),
// 1 = dynamic (matrix elsewhere: prop/character with its own transform), 2 = no clip matrix found, -1 = not evaluated.
static int jitter_scene_draw(const cl_state& s)
{
    g_jitStat[0]++;
    if (!s.cbv_set[2] || !s.cbv_res[2].handle || (g_renderW == 0 && g_internalW == 0)) { g_jitStat[1]++; return -1; }
    const uint64_t key = s.cbv_res[2].handle ^ (s.cbv_off[2] * 0x9E3779B97F4A7C15ull);
    auto ins = g_patchedRegions.emplace(key, region_info{});
    if (!ins.second) { g_jitStat[2]++; return ins.first->second.cls; }   // this constant region was already handled
    ID3D12Resource* r = reinterpret_cast<ID3D12Resource*>(s.cbv_res[2].handle);
    uint64_t size = 0; uint8_t* base = map_upload(r, &size);
    if (!base) { g_jitStat[3]++; return -1; }
    const size_t nread = (s.cbv_off[2] + 36 * 4 <= size) ? 36 : 20;
    if (s.cbv_off[2] + nread * 4 > size) { g_jitStat[3]++; return -1; }
    float* c = reinterpret_cast<float*>(base + s.cbv_off[2]);
    float m[36] = {}; memcpy(m, c, nread * 4);     // read once (write-combined memory)
    { region_info& ri = ins.first->second; ri.anchorN = (uint32_t)(nread < 32 ? nread : 32); memcpy(ri.anchor, m, ri.anchorN * 4); }   // signature for the object-vector pairing
    int k = -1;
    for (int cand = 0; cand + 16 <= (int)nread; cand += 4) if (looks_like_clip_matrix(m + cand)) { k = cand; break; }
    const float* mm = k >= 0 ? m + k : nullptr;   // the clip matrix as read
    float deep[16];
    if (k < 0 && g_haveLastGoodVP && s.pso) {
        // Skinned character shaders keep their bone palette from register 0 on and the view-projection after it, far
        // past the 9 registers above - those draws were never jittered, so the characters sat still while DLSS assumed
        // the frame's jitter: a sub-pixel wobble on every character, which frame generation turns into a flicker (its
        // interpolated frames follow the exact object vectors, the real frames carry the wobble). Where the matrix sits
        // is learned once per vertex shader: the region is scanned (up to 1024 floats) for the clip-matrix signature,
        // which must also be the camera (close to last frame's VP); a miss is retried every 600 frames (a cut frame
        // fails the camera test).
        struct vs_off { int off; uint32_t tried; };
        static std::unordered_map<uint64_t, vs_off> byVs;
        const uint64_t vsh = objmv::pso_vs_hash(s.pso);
        auto it = byVs.find(vsh);
        int off = -1;
        if (it != byVs.end() && (it->second.off >= 0 || g_frame - it->second.tried < 600)) off = it->second.off;
        else {
            const uint64_t avail = (size - s.cbv_off[2]) / 4;
            const size_t maxF = (size_t)(avail < 1024 ? avail : 1024);
            std::vector<float> buf(maxF); memcpy(buf.data(), c, maxF * 4);
            for (size_t cand = 36; cand + 16 <= maxF; cand += 4) if (looks_like_clip_matrix(&buf[cand]) && camera_close(&buf[cand], g_lastGoodVP)) { off = (int)cand; break; }
            const bool first = it == byVs.end();
            byVs[vsh] = vs_off{ off, g_frame };
            if (off >= 0) logmsg("clip matrix of vertex shader %016llx found at c[%d] (after %d registers of bone/model data): its draws are jittered from now on", (unsigned long long)vsh, off / 4, off / 4);
            else if (first && byVs.size() <= 64) logmsg("vertex shader %016llx: no camera matrix in its first %zu registers (draws stay unjittered; retried later)", (unsigned long long)vsh, maxF / 4);
        }
        if (off >= 0 && s.cbv_off[2] + uint64_t(off + 16) * 4 <= size) { memcpy(deep, c + off, 64); if (looks_like_clip_matrix(deep)) { k = off; mm = deep; g_jitStat[6]++; } }
    }
    int cls = 2;
    if (k >= 0) {
        cls = (k == 0) ? 0 : 1;
        if (k == 0) {   // vote: the block shared by most draws is the view-projection (identity model matrix)
            uint64_t hsh = 1469598103934665603ull; const uint32_t* u = reinterpret_cast<const uint32_t*>(m);
            for (int i = 0; i < 16; ++i) { hsh ^= u[i]; hsh *= 1099511628211ull; }
            vp_vote& v = g_vpVotes[hsh]; if (v.count++ == 0) memcpy(v.m, m, 64);
            if (s.vp_valid && (vp_full_frame(s.vp, float(s.rt_w), float(s.rt_h)) || vp_is_layout_scene(s.vp))) v.mainCount++;
            else if (s.vp_valid) { const viewport* wl = win_layout_now(); if (wl && vp_in_layout(s.vp, *wl)) v.winCount++; }
        }
        if (g_cfgJitter && !g_injectedThisFrame) {   // draws after DLSS ran (transparents, particles, HUD) stay unjittered
            const float ox = g_cfgJitterSignX * 2.0f * g_jitterX / (jitter_ref_w() * drs_factor_x()), oy = g_cfgJitterSignY * 2.0f * g_jitterY / (jitter_ref_h() * drs_factor_y());
            float row0[4], row1[4];
            for (int i = 0; i < 4; ++i) { row0[i] = mm[i] + ox * mm[12 + i]; row1[i] = mm[4 + i] + oy * mm[12 + i]; }
            memcpy(c + k, row0, 16); memcpy(c + k + 4, row1, 16);
            static bool once = false;
            if (!once) { once = true; logmsg("first jittered matrix at c[%d]: row0 %.4f %.4f %.4f %.2f -> %.4f %.4f %.4f %.2f (jitter %.3f,%.3f px)", k / 4, mm[0], mm[1], mm[2], mm[3], row0[0], row0[1], row0[2], row0[3], g_jitterX, g_jitterY); }
        }
        g_patchedDraws++; g_jitStat[4]++;
    } else {
        g_matrixMisses++; g_jitStat[5]++;
        if (g_missLogBudget > 0) { g_missLogBudget--; logmsg("no clip matrix (pso=%p rt=%p): c0=(%.3f %.3f %.3f %.2f) c1=(%.3f %.3f %.3f %.2f) c2=(%.3f %.3f %.3f %.2f) c3=(%.3f %.3f %.3f %.2f) c4=(%.3f %.3f %.3f %.2f)", (void*)s.pso, (void*)s.rt.handle, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15], m[16], m[17], m[18], m[19]); }
    }
    ins.first->second.cls = cls;
    return cls;
}

static vp_vote g_topVotes[3] = {};
static void select_window_vp()
{
    g_haveWinVP = false; g_winCut = false;
    const vp_vote* best = nullptr;
    for (auto& kv : g_vpVotes) { const vp_vote& v = kv.second; if (v.winCount >= 10 && (!best || v.winCount > best->winCount)) best = &v; }
    if (best) { memcpy(g_winVP, best->m, 64); g_haveWinVP = true; if (g_haveWinVPPrev && !camera_close(g_winVP, g_winVPPrev)) { g_winCut = true; g_winCuts++; if (g_winCuts <= 20) logmsg("camera window: cut at frame %u (the feed switched cameras) - its object vectors dropped for the frame", g_frame); } }
}
static void select_frame_vp()
{
    g_haveFrameVP = false;
    vp_vote top[3] = {};
    for (auto& kv : g_vpVotes) {
        vp_vote v = kv.second;
        if (v.mainCount) v.count = 1000000u + v.mainCount;   // the main view's draws outrank everything: the camera window's and the caller's cameras have hundreds of identity-matrix draws of their own
        // Skip bare projection matrices (view = identity: w-row (0,0,1|0)) - those are view-space/HUD draws, not the camera.
        if (fabsf(v.m[15]) < 1.0f && fabsf(v.m[12]) < 1e-3f && fabsf(v.m[13]) < 1e-3f) continue;
        if (v.count > top[0].count) { top[2] = top[1]; top[1] = top[0]; top[0] = v; }
        else if (v.count > top[1].count) { top[2] = top[1]; top[1] = v; }
        else if (v.count > top[2].count) top[2] = v;
    }
    if (top[0].count) { memcpy(g_frameVP, top[0].m, 64); g_haveFrameVP = true; }
    memcpy(g_topVotes, top, sizeof(top));
}

// ---- Phase 1b: camera-only motion vectors (compute pass, see src/mv_cs.hlsl) ---------------------------------------
static ID3D12RootSignature* g_mvRootSig = nullptr;
static ID3D12PipelineState* g_mvPso = nullptr;
static ID3D12PipelineState* g_visPso = nullptr;
static ID3D12PipelineState* g_hudlessPso = nullptr;
static ID3D12PipelineState* g_resamplePso = nullptr;
static ID3D12PipelineState* g_stretchPso = nullptr;
static ID3D12PipelineState* g_uimaskPso = nullptr;
static ID3D12DescriptorHeap* g_mvHeap = nullptr;    // shader visible: 4 slots x (SRV depth, UAV mv) + 4 slots x (SRV mv, UAV out)
static ID3D12Resource* g_mvCb = nullptr; static uint8_t* g_mvCbPtr = nullptr;   // 8 x 256 B upload ring
static ID3D12Resource* g_dummyUav = nullptr;   // 8x8 R8 texture bound where a shader declares a UAV it never writes
static uint32_t g_mvSlot = 0;
static bool g_mvReady = false, g_mvInitTried = false;
static resource_usage g_mvState = resource_usage::shader_resource_non_pixel;
static uint32_t g_mvDispatches = 0, g_mvResets = 0;
static float g_camDeltaRot = 0, g_camDeltaPos = 0, g_camDeltaRotMax = 0, g_camDeltaPosMax = 0;
static uint32_t g_vpChanges = 0;   // frames (in the logging window) whose VP differed from the previous frame's
struct VisCB { float inSize[2]; float outSize[2]; float scale; float blend; float pad[2]; };
struct MvCB { float invVP[16]; float prevVP[16]; float size[2]; float nearZ; float reset; float dynZeroMV; float depthScale[2]; float pad; float rect[4]; float depthOrigin[2]; float pad2[2]; };

static bool invert4x4(const float* m, float* out)
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

// Camera position from a row-major clip matrix: solve rows x, y, w for the point where they all evaluate to zero.
static bool camera_position(const float* m, float* out)
{
    const float a[3][3] = { { m[0], m[1], m[2] }, { m[4], m[5], m[6] }, { m[12], m[13], m[14] } };
    const float b[3] = { -m[3], -m[7], -m[15] };
    const float det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    if (fabsf(det) < 1e-12f) return false;
    for (int i = 0; i < 3; ++i) {
        float c[3][3]; memcpy(c, a, sizeof(c));
        for (int r = 0; r < 3; ++r) c[r][i] = b[r];
        const float d = c[0][0] * (c[1][1] * c[2][2] - c[1][2] * c[2][1]) - c[0][1] * (c[1][0] * c[2][2] - c[1][2] * c[2][0]) + c[0][2] * (c[1][0] * c[2][1] - c[1][1] * c[2][0]);
        out[i] = d / det;
    }
    return true;
}

static bool mv_init()
{
    if (g_mvInitTried) return g_mvReady;
    g_mvInitTried = true;
    D3D12_DESCRIPTOR_RANGE srvRange = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };
    D3D12_DESCRIPTOR_RANGE uavRange = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };
    D3D12_ROOT_PARAMETER params[3] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; params[0].Descriptor = { 0, 0 }; params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; params[1].DescriptorTable = { 1, &srvRange }; params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; params[2].DescriptorTable = { 1, &uavRange }; params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rs = { 3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ID3DBlob* blob = nullptr; ID3DBlob* err = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
    if (FAILED(hr)) { logmsg("MV: root signature serialize failed 0x%08lX %s", (unsigned long)hr, err ? (const char*)err->GetBufferPointer() : ""); return false; }
    hr = g_d3d->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_mvRootSig)); blob->Release();
    if (FAILED(hr)) { logmsg("MV: CreateRootSignature failed 0x%08lX", (unsigned long)hr); return false; }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {}; pso.pRootSignature = g_mvRootSig; pso.CS = { g_mv_cs, sizeof(g_mv_cs) };
    hr = g_d3d->CreateComputePipelineState(&pso, IID_PPV_ARGS(&g_mvPso));
    if (FAILED(hr)) { logmsg("MV: CreateComputePipelineState failed 0x%08lX", (unsigned long)hr); return false; }
    pso.CS = { g_mv_vis, sizeof(g_mv_vis) };
    hr = g_d3d->CreateComputePipelineState(&pso, IID_PPV_ARGS(&g_visPso));
    if (FAILED(hr)) { logmsg("MV: visualization PSO failed 0x%08lX", (unsigned long)hr); return false; }
    pso.CS = { g_hudless_cs, sizeof(g_hudless_cs) };
    hr = g_d3d->CreateComputePipelineState(&pso, IID_PPV_ARGS(&g_hudlessPso));
    if (FAILED(hr)) { logmsg("MV: HUD-less PSO failed 0x%08lX", (unsigned long)hr); return false; }
    pso.CS = { g_resample_cs, sizeof(g_resample_cs) };
    hr = g_d3d->CreateComputePipelineState(&pso, IID_PPV_ARGS(&g_resamplePso));
    if (FAILED(hr)) { logmsg("MV: resample PSO failed 0x%08lX", (unsigned long)hr); return false; }
    pso.CS = { g_depth_stretch_cs, sizeof(g_depth_stretch_cs) };
    hr = g_d3d->CreateComputePipelineState(&pso, IID_PPV_ARGS(&g_stretchPso));
    if (FAILED(hr)) { logmsg("MV: depth stretch PSO failed 0x%08lX", (unsigned long)hr); return false; }
    pso.CS = { g_uimask_cs, sizeof(g_uimask_cs) };
    hr = g_d3d->CreateComputePipelineState(&pso, IID_PPV_ARGS(&g_uimaskPso));
    if (FAILED(hr)) { logmsg("MV: UI mask PSO failed 0x%08lX", (unsigned long)hr); return false; }
    // One shader-visible heap for every add-on pass, so the insertion switches heaps once (bgfx -> ours) and once back
    // rather than per pass: 24 slots x [srv0, srv1, uav0, uav1] (mv 0-3, uimask 4-7, vis 8-11, hudless 12-15, resample
    // 16-19, depth stretch 20-23, resume mask 23), then the DoF passes' 8-frame ring (DOF_RING x DOF_DESC_PER_FRAME).
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, MV_HEAP_SLOTS * 4 + DOF_RING * DOF_DESC_PER_FRAME, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
    hr = g_d3d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_mvHeap));
    if (FAILED(hr)) { logmsg("MV: CreateDescriptorHeap failed 0x%08lX", (unsigned long)hr); return false; }
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_UPLOAD, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1 };
    // 32 x 256 B constant slots: mv 0-3, vis 4-7, resample 8-15, object-vector merge 16-19
    D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = 32 * 256; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc = { 1, 0 }; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hr = g_d3d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_mvCb));
    if (FAILED(hr)) { logmsg("MV: constant buffer creation failed 0x%08lX", (unsigned long)hr); return false; }
    D3D12_RANGE none = { 0, 0 }; g_mvCb->Map(0, &none, reinterpret_cast<void**>(&g_mvCbPtr));
    D3D12_HEAP_PROPERTIES hpDef = { D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1 };
    D3D12_RESOURCE_DESC td = {}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = 8; td.Height = 8; td.DepthOrArraySize = 1; td.MipLevels = 1; td.Format = DXGI_FORMAT_R8_UNORM; td.SampleDesc = { 1, 0 }; td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    hr = g_d3d->CreateCommittedResource(&hpDef, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&g_dummyUav));
    if (FAILED(hr)) { logmsg("MV: dummy UAV texture failed 0x%08lX", (unsigned long)hr); return false; }
    g_mvReady = g_mvCbPtr != nullptr;
    logmsg("MV: compute pass ready (%s)", g_mvReady ? "ok" : "map failed");
    return g_mvReady;
}

// Writes camera-only motion vectors into g_mv (depth must be in a shader-readable state). Returns the reset flag used.
static uint32_t g_vpMissStreak = 0, g_vpMissesCovered = 0;

static int mv_dispatch(command_list* cmd, resource depth, format depthFmt, uint32_t w, uint32_t h, float depthScaleX, float depthScaleY, const float* depthOrigin, const float* rect)
{
    if (!mv_init()) return 1;
    int reset = 0;
    // The camera matrix is recovered from the draw constants and is occasionally missed for a frame. Clearing the
    // DLSS history on such a frame is far worse than a frame of stale motion: it throws away the accumulated detail
    // (and the history of anything layered on it, such as DLSS 5 NR), which shows up as a periodic flicker. So a
    // short run of misses reuses the last known camera - the camera is simply treated as still for that frame.
    const float* curVP = g_frameVP;
    const float* prevVP = g_prevVP;
    bool haveCam = g_haveFrameVP && g_havePrevVP;
    if (!g_cfgMotionVectors) {
        reset = 1;
        if (g_mvResets++ <= 200) logmsg("RESET f%u: motion vectors disabled", g_frame);
    } else if (!haveCam) {
        // Codec calls / menus over a frozen 3D frame have no camera matrix for seconds at a time; resetting every one of
        // those frames wiped the DLSS (and NR) history for the whole duration. Keep the last camera for as long as it
        // takes: zero motion on a still image is harmless, and the camera-delta check below fires a single proper reset
        // when a real camera comes back somewhere else.
        if (g_haveLastGoodVP) {
            ++g_vpMissStreak;
            curVP = prevVP = g_lastGoodVP;   // no camera motion this frame, history kept
            haveCam = true; g_vpMissesCovered++;
            if (g_vpMissStreak == 31) logmsg("f%u: no camera matrix for 30+ frames - keeping the last camera (no history reset)", g_frame);
        } else {
            reset = 1;
            if (g_mvResets++ <= 200) logmsg("RESET f%u: no camera matrix for %u frames (thisVP %d, prevVP %d)", g_frame, g_vpMissStreak, (int)g_haveFrameVP, (int)g_havePrevVP);
        }
    }
    if (haveCam && !reset) {
        g_vpMissStreak = g_haveFrameVP && g_havePrevVP ? 0 : g_vpMissStreak;
        // Camera cut heuristic on the real camera: position (the point where clip x, y and w are all zero) and the
        // viewing direction (w-row xyz). Pure rotation keeps the position still; walking moves it a few units per frame.
        float cur[3], prev[3];
        const bool okC = camera_position(curVP, cur), okP = camera_position(prevVP, prev);
        float dp = okC && okP ? sqrtf((cur[0] - prev[0]) * (cur[0] - prev[0]) + (cur[1] - prev[1]) * (cur[1] - prev[1]) + (cur[2] - prev[2]) * (cur[2] - prev[2])) : 0.0f;
        float dot = curVP[12] * prevVP[12] + curVP[13] * prevVP[13] + curVP[14] * prevVP[14];
        float dr = 1.0f - dot;   // 0 = same direction; 0.06 ~ 20 degrees
        g_camDeltaRot = dr; g_camDeltaPos = dp;
        if (dr > g_camDeltaRotMax) g_camDeltaRotMax = dr; if (dp > g_camDeltaPosMax) g_camDeltaPosMax = dp;
        if (memcmp(curVP, prevVP, 64) != 0) g_vpChanges++;
        if (dr > 0.06f || dp > g_cfgCutPosLimit) {
            reset = 1; g_mvResets++;
            if (g_mvResets <= 200) logmsg("RESET f%u: camera delta rot %.4f (limit 0.06) pos %.1f (limit %.0f) -> DLSS history cleared", g_frame, dr, dp, g_cfgCutPosLimit);
        }
    }
    MvCB cb = {};
    if (!reset) {
        if (!invert4x4(curVP, cb.invVP)) { reset = 1; if (g_mvResets++ <= 200) logmsg("RESET f%u: view-projection not invertible", g_frame); }
        memcpy(cb.prevVP, prevVP, 64);
    }
    cb.size[0] = float(w); cb.size[1] = float(h); if (rect) memcpy(cb.rect, rect, sizeof(cb.rect)); else { cb.rect[0] = 0; cb.rect[1] = 0; cb.rect[2] = float(w); cb.rect[3] = float(h); } cb.depthScale[0] = depthScaleX > 0 ? depthScaleX : 1.0f; cb.depthScale[1] = depthScaleY > 0 ? depthScaleY : 1.0f; cb.nearZ = curVP[11] != 0 ? curVP[11] : 1.0f; cb.reset = reset ? 1.0f : 0.0f;
    cb.dynZeroMV = (g_cfgDynMask && g_cfgDynZeroMV) ? 1.0f : 0.0f;
    if (depthOrigin) { cb.depthOrigin[0] = depthOrigin[0]; cb.depthOrigin[1] = depthOrigin[1]; }
    const uint32_t slot = g_mvSlot++ % 4;
    memcpy(g_mvCbPtr + slot * 256, &cb, sizeof(cb));

    // descriptors: [0] SRV scene depth, [1] SRV dynamic depth, [2] UAV motion vectors, [3] UAV mask
    const UINT inc = g_d3d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_mvHeap->GetCPUDescriptorHandleForHeapStart(); cpu.ptr += SIZE_T(slot) * 4 * inc;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_mvHeap->GetGPUDescriptorHandleForHeapStart(); gpu.ptr += UINT64(slot) * 4 * inc;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = (depthFmt == format::r32_typeless || depthFmt == format::d32_float || depthFmt == format::r32_float) ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Texture2D.MipLevels = 1;
    g_d3d->CreateShaderResourceView(reinterpret_cast<ID3D12Resource*>(depth.handle), &srv, cpu);
    D3D12_CPU_DESCRIPTOR_HANDLE h1 = cpu; h1.ptr += inc;
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDyn = srv; srvDyn.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    g_d3d->CreateShaderResourceView(reinterpret_cast<ID3D12Resource*>(g_dynDepth.handle ? g_dynDepth.handle : depth.handle), g_dynDepth.handle ? &srvDyn : &srv, h1);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {}; uav.Format = DXGI_FORMAT_R16G16_FLOAT; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE h2 = cpu; h2.ptr += 2 * inc;
    g_d3d->CreateUnorderedAccessView(reinterpret_cast<ID3D12Resource*>(g_mv.handle), nullptr, &uav, h2);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavMask = {}; uavMask.Format = DXGI_FORMAT_R8_UNORM; uavMask.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE h3 = cpu; h3.ptr += 3 * inc;
    g_d3d->CreateUnorderedAccessView(reinterpret_cast<ID3D12Resource*>(g_mask.handle), nullptr, &uavMask, h3);

    cmd->barrier(g_mv, g_mvState, resource_usage::unordered_access); g_mvState = resource_usage::unordered_access;
    cmd->barrier(g_mask, g_maskState, resource_usage::unordered_access); g_maskState = resource_usage::unordered_access;
    if (g_dynDepth.handle && g_dynState != resource_usage::shader_resource_non_pixel) { cmd->barrier(g_dynDepth, g_dynState, resource_usage::shader_resource_non_pixel); g_dynState = resource_usage::shader_resource_non_pixel; }
    ID3D12GraphicsCommandList* native = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd->get_native());
    ID3D12DescriptorHeap* heaps[1] = { g_mvHeap };
    native->SetDescriptorHeaps(1, heaps);
    native->SetComputeRootSignature(g_mvRootSig);
    native->SetPipelineState(g_mvPso);
    native->SetComputeRootConstantBufferView(0, g_mvCb->GetGPUVirtualAddress() + slot * 256);
    native->SetComputeRootDescriptorTable(1, gpu);
    D3D12_GPU_DESCRIPTOR_HANDLE gpuUav = gpu; gpuUav.ptr += 2 * inc;
    native->SetComputeRootDescriptorTable(2, gpuUav);
    native->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    cmd->barrier(g_mv, resource_usage::unordered_access, resource_usage::shader_resource_non_pixel); g_mvState = resource_usage::shader_resource_non_pixel;
    cmd->barrier(g_mask, resource_usage::unordered_access, resource_usage::shader_resource_non_pixel); g_maskState = resource_usage::shader_resource_non_pixel;
    if (g_dynDepth.handle) {   // consumed: clear for the next frame's replayed draws
        cmd->barrier(g_dynDepth, g_dynState, resource_usage::depth_stencil_write); g_dynState = resource_usage::depth_stencil_write;
        const float zero = 0.0f; cmd->clear_depth_stencil_view(g_dynDsv, &zero, nullptr);
    }
    g_mvDispatches++;
    return reset;
}


// Dynamic resolution: nearest-neighbor stretch of the sub-rect scene depth into the full-size R32 copy (g_depthFull).
static void depth_stretch_dispatch(command_list* cmd, resource depth, format depthFmt, uint32_t fullW, uint32_t fullH, float kx, float ky, const float* origin, const float* rect, bool clearOutside, const float* keep, resource dst = { 0 }, resource_usage* dstState = nullptr)
{
    if (!dst.handle) { dst = g_depthFull; dstState = &g_depthFullState; }
    if (!g_mvReady || !g_stretchPso || !dst.handle) return;
    static ID3D12Resource* cbRes = nullptr; static uint8_t* cbPtr = nullptr; static uint32_t cbSlot = 0;
    if (!cbRes) {
        D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_UPLOAD, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1 };
        D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = 4 * 256; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc = { 1, 0 }; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(g_d3d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&cbRes)))) return;
        D3D12_RANGE none = { 0, 0 }; cbRes->Map(0, &none, reinterpret_cast<void**>(&cbPtr));
        if (!cbPtr) return;
    }
    const uint32_t slot = 20 + (cbSlot % 4);
    float cb[16] = { float(fullW), float(fullH), kx, ky, origin ? origin[0] : 0.0f, origin ? origin[1] : 0.0f, clearOutside ? 1.0f : 0.0f, 0.0f,
                     rect ? rect[0] : 0.0f, rect ? rect[1] : 0.0f, rect ? rect[2] : float(fullW), rect ? rect[3] : float(fullH),
                     keep ? keep[0] : 0.0f, keep ? keep[1] : 0.0f, keep ? keep[2] : 0.0f, keep ? keep[3] : 0.0f };
    memcpy(cbPtr + (cbSlot % 4) * 256, cb, sizeof(cb));
    const UINT inc = g_d3d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_mvHeap->GetCPUDescriptorHandleForHeapStart(); cpu.ptr += SIZE_T(slot) * 4 * inc;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_mvHeap->GetGPUDescriptorHandleForHeapStart(); gpu.ptr += UINT64(slot) * 4 * inc;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = (depthFmt == format::r32_typeless || depthFmt == format::d32_float || depthFmt == format::r32_float) ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Texture2D.MipLevels = 1;
    g_d3d->CreateShaderResourceView(reinterpret_cast<ID3D12Resource*>(depth.handle), &srv, cpu);
    D3D12_CPU_DESCRIPTOR_HANDLE h1 = cpu; h1.ptr += inc;
    g_d3d->CreateShaderResourceView(reinterpret_cast<ID3D12Resource*>(depth.handle), &srv, h1);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {}; uav.Format = DXGI_FORMAT_R32_FLOAT; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE h2 = cpu; h2.ptr += 2 * inc;
    g_d3d->CreateUnorderedAccessView(reinterpret_cast<ID3D12Resource*>(dst.handle), nullptr, &uav, h2);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDummy = {}; uavDummy.Format = DXGI_FORMAT_R8_UNORM; uavDummy.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE h3 = cpu; h3.ptr += 3 * inc;
    g_d3d->CreateUnorderedAccessView(g_dummyUav, nullptr, &uavDummy, h3);
    if (*dstState != resource_usage::unordered_access) { cmd->barrier(dst, *dstState, resource_usage::unordered_access); *dstState = resource_usage::unordered_access; }
    ID3D12GraphicsCommandList* native = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd->get_native());
    ID3D12DescriptorHeap* heaps[1] = { g_mvHeap };
    native->SetDescriptorHeaps(1, heaps);
    native->SetComputeRootSignature(g_mvRootSig);
    native->SetPipelineState(g_stretchPso);
    native->SetComputeRootConstantBufferView(0, cbRes->GetGPUVirtualAddress() + (cbSlot % 4) * 256);
    native->SetComputeRootDescriptorTable(1, gpu);
    D3D12_GPU_DESCRIPTOR_HANDLE gpuUav = gpu; gpuUav.ptr += 2 * inc;
    native->SetComputeRootDescriptorTable(2, gpuUav);
    native->Dispatch((fullW + 7) / 8, (fullH + 7) / 8, 1);
    cmd->barrier(dst, resource_usage::unordered_access, resource_usage::shader_resource); *dstState = resource_usage::shader_resource;
    cbSlot++;
}

// UI mask: the replayed UI layer marks HUD pixels in the bias-current-color mask and zeroes their motion vectors.
static uint32_t g_uiMaskFrames = 0;
static void uimask_dispatch(command_list* cmd, uint32_t w, uint32_t h, bool forceAll)
{
    if (!g_mvReady || !g_uimaskPso || !g_ui.handle || !g_mv.handle || !g_mask.handle) return;
    static ID3D12Resource* cbRes = nullptr; static uint8_t* cbPtr = nullptr; static uint32_t cbSlot = 0;
    if (!cbRes) {
        D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_UPLOAD, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1 };
        D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = 4 * 256; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc = { 1, 0 }; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(g_d3d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&cbRes)))) return;
        D3D12_RANGE none = { 0, 0 }; cbRes->Map(0, &none, reinterpret_cast<void**>(&cbPtr));
        if (!cbPtr) return;
    }
    const uint32_t slot = 4 + (cbSlot % 4);
    float cb[4] = { float(w), float(h), forceAll ? 1.0f : 0.0f, 0.0f };
    memcpy(cbPtr + (cbSlot % 4) * 256, cb, sizeof(cb));
    const UINT inc = g_d3d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_mvHeap->GetCPUDescriptorHandleForHeapStart(); cpu.ptr += SIZE_T(slot) * 4 * inc;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_mvHeap->GetGPUDescriptorHandleForHeapStart(); gpu.ptr += UINT64(slot) * 4 * inc;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {}; srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Texture2D.MipLevels = 1;
    g_d3d->CreateShaderResourceView(reinterpret_cast<ID3D12Resource*>(g_ui.handle), &srv, cpu);
    D3D12_CPU_DESCRIPTOR_HANDLE h1 = cpu; h1.ptr += inc;
    g_d3d->CreateShaderResourceView(reinterpret_cast<ID3D12Resource*>(g_ui.handle), &srv, h1);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {}; uav.Format = DXGI_FORMAT_R16G16_FLOAT; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE h2 = cpu; h2.ptr += 2 * inc;
    g_d3d->CreateUnorderedAccessView(reinterpret_cast<ID3D12Resource*>(g_mv.handle), nullptr, &uav, h2);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavMask = {}; uavMask.Format = DXGI_FORMAT_R8_UNORM; uavMask.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE h3 = cpu; h3.ptr += 3 * inc;
    g_d3d->CreateUnorderedAccessView(reinterpret_cast<ID3D12Resource*>(g_mask.handle), nullptr, &uavMask, h3);
    if (g_uiState != resource_usage::shader_resource_non_pixel) { cmd->barrier(g_ui, g_uiState, resource_usage::shader_resource_non_pixel); g_uiState = resource_usage::shader_resource_non_pixel; }
    cmd->barrier(g_mv, g_mvState, resource_usage::unordered_access); g_mvState = resource_usage::unordered_access;
    cmd->barrier(g_mask, g_maskState, resource_usage::unordered_access); g_maskState = resource_usage::unordered_access;
    ID3D12GraphicsCommandList* native = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd->get_native());
    ID3D12DescriptorHeap* heaps[1] = { g_mvHeap };
    native->SetDescriptorHeaps(1, heaps);
    native->SetComputeRootSignature(g_mvRootSig);
    native->SetPipelineState(g_uimaskPso);
    native->SetComputeRootConstantBufferView(0, cbRes->GetGPUVirtualAddress() + (cbSlot % 4) * 256);
    native->SetComputeRootDescriptorTable(1, gpu);
    D3D12_GPU_DESCRIPTOR_HANDLE gpuUav = gpu; gpuUav.ptr += 2 * inc;
    native->SetComputeRootDescriptorTable(2, gpuUav);
    native->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    cmd->barrier(g_mv, resource_usage::unordered_access, resource_usage::shader_resource_non_pixel); g_mvState = resource_usage::shader_resource_non_pixel;
    cmd->barrier(g_mask, resource_usage::unordered_access, resource_usage::shader_resource_non_pixel); g_maskState = resource_usage::shader_resource_non_pixel;
    g_uiMaskFrames++; cbSlot++;
}

// ---- DoF after DLSS (PostDof) --------------------------------------------------------------------------------------
static ID3D12RootSignature* g_dofRootSig = nullptr; static ID3D12PipelineState* g_dofCocPso = nullptr; static ID3D12PipelineState* g_dofPackPso = nullptr; static ID3D12PipelineState* g_dofGatherPso = nullptr; static ID3D12PipelineState* g_dofCompositePso = nullptr;
static ID3D12Resource* g_dofCbRes = nullptr; static uint8_t* g_dofCbPtr = nullptr;   // descriptors live in the shared g_mvHeap (after the MV_HEAP_SLOTS)
struct DofCB { float c[10][4]; float g[10][4]; float halfSize[2], fullSize[2], depthScale[2], depthSize[2], stepUV[2], cocUnorm, radiusScale, depthOff[2], debugView, hasMask, depthJitter[2], maskScale[2]; };
static_assert(sizeof(DofCB) <= DOF_CB_STRIDE, "DofCB must fit one constant-buffer ring slot");
static void restore_state(device* dev, command_list* cmd, const cl_state& s);
// Ring entry of the frame being recorded: constants (DOF_CB_PER_FRAME slots: 0 = the CoC pass, 1 = the insertion passes)
// and descriptors (DOF_DESC_PER_FRAME = 4 passes x [srv0, srv1, uav0, uav1]). Frame N's entry is not touched again
// until frame N + DOF_RING, long after the GPU has run it.
static uint32_t dof_ring() { return g_frame % DOF_RING; }
static D3D12_GPU_VIRTUAL_ADDRESS dof_cb_write(const DofCB& cb, uint32_t which)
{
    const uint32_t slot = dof_ring() * DOF_CB_PER_FRAME + which;
    memcpy(g_dofCbPtr + slot * DOF_CB_STRIDE, &cb, sizeof(cb));
    return g_dofCbRes->GetGPUVirtualAddress() + UINT64(slot) * DOF_CB_STRIDE;
}
static D3D12_CPU_DESCRIPTOR_HANDLE dof_cpu(uint32_t i) { D3D12_CPU_DESCRIPTOR_HANDLE h = g_mvHeap->GetCPUDescriptorHandleForHeapStart(); h.ptr += SIZE_T(MV_HEAP_SLOTS * 4 + dof_ring() * DOF_DESC_PER_FRAME + i) * g_d3d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV); return h; }
static D3D12_GPU_DESCRIPTOR_HANDLE dof_gpu(uint32_t i) { D3D12_GPU_DESCRIPTOR_HANDLE h = g_mvHeap->GetGPUDescriptorHandleForHeapStart(); h.ptr += UINT64(MV_HEAP_SLOTS * 4 + dof_ring() * DOF_DESC_PER_FRAME + i) * g_d3d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV); return h; }
static bool dof_init()
{
    if (g_dofInitTried) return g_dofReady;
    g_dofInitTried = true;
    if (!mv_init()) { logmsg("PostDof: the shared descriptor heap is unavailable"); return false; }   // descriptors live in g_mvHeap
    D3D12_DESCRIPTOR_RANGE srvRange = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };
    D3D12_DESCRIPTOR_RANGE uavRange = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };
    D3D12_ROOT_PARAMETER params[3] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; params[0].Descriptor = { 0, 0 }; params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; params[1].DescriptorTable = { 1, &srvRange }; params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; params[2].DescriptorTable = { 1, &uavRange }; params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC samp = {}; samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR; samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP; samp.MaxLOD = D3D12_FLOAT32_MAX; samp.ShaderRegister = 0; samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rs = { 3, params, 1, &samp, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ID3DBlob* blob = nullptr; ID3DBlob* err = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
    if (FAILED(hr)) { logmsg("PostDof: root signature serialize failed 0x%08lX %s", (unsigned long)hr, err ? (const char*)err->GetBufferPointer() : ""); return false; }
    hr = g_d3d->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_dofRootSig)); blob->Release();
    if (FAILED(hr)) { logmsg("PostDof: CreateRootSignature failed 0x%08lX", (unsigned long)hr); return false; }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {}; pso.pRootSignature = g_dofRootSig;
    pso.CS = { g_dof_coc_cs, sizeof(g_dof_coc_cs) }; hr = g_d3d->CreateComputePipelineState(&pso, IID_PPV_ARGS(&g_dofCocPso)); if (FAILED(hr)) { logmsg("PostDof: CoC PSO failed 0x%08lX", (unsigned long)hr); return false; }
    pso.CS = { g_dof_pack_cs, sizeof(g_dof_pack_cs) }; hr = g_d3d->CreateComputePipelineState(&pso, IID_PPV_ARGS(&g_dofPackPso)); if (FAILED(hr)) { logmsg("PostDof: pack PSO failed 0x%08lX", (unsigned long)hr); return false; }
    pso.CS = { g_dof_gather_cs, sizeof(g_dof_gather_cs) }; hr = g_d3d->CreateComputePipelineState(&pso, IID_PPV_ARGS(&g_dofGatherPso)); if (FAILED(hr)) { logmsg("PostDof: gather PSO failed 0x%08lX", (unsigned long)hr); return false; }
    pso.CS = { g_dof_composite_cs, sizeof(g_dof_composite_cs) }; hr = g_d3d->CreateComputePipelineState(&pso, IID_PPV_ARGS(&g_dofCompositePso)); if (FAILED(hr)) { logmsg("PostDof: composite PSO failed 0x%08lX", (unsigned long)hr); return false; }
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_UPLOAD, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1 };
    D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = UINT64(DOF_RING) * DOF_CB_PER_FRAME * DOF_CB_STRIDE; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc = { 1, 0 }; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hr = g_d3d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_dofCbRes)); if (FAILED(hr)) { logmsg("PostDof: constant buffer failed 0x%08lX", (unsigned long)hr); return false; }
    D3D12_RANGE none = { 0, 0 }; g_dofCbRes->Map(0, &none, reinterpret_cast<void**>(&g_dofCbPtr));
    if (g_dofCbPtr) memset(g_dofCbPtr, 0, (size_t)bd.Width);
    g_dofReady = g_dofCbPtr != nullptr;
    logmsg("PostDof: compute passes ready (%s; %u-frame ring of constants and descriptors)", g_dofReady ? "ok" : "map failed", DOF_RING);
    return g_dofReady;
}
// The half-resolution textures for a fullW x fullH output: color + CoC (the gather input), the CoC alone (written at
// the game's CoC draw), the blurred layer. Re-created on a size change.
static bool dof_ensure_textures(device* dev, uint32_t fullW, uint32_t fullH)
{
    const uint32_t hw = (fullW + 1) / 2, hh = (fullH + 1) / 2;
    if (g_dofW == hw && g_dofH == hh && g_dofCoc.handle && g_dofCocOnly.handle && g_dofBlur.handle) return true;
    for (resource* r : { &g_dofCoc, &g_dofCocOnly, &g_dofBlur }) if (r->handle) { dev->destroy_resource(*r); *r = { 0 }; }
    const resource_desc td(hw, hh, 1, 1, format::r16g16b16a16_float, 1, memory_heap::default_, resource_usage::shader_resource | resource_usage::unordered_access);
    const resource_desc cd(hw, hh, 1, 1, format::r16_float, 1, memory_heap::default_, resource_usage::shader_resource | resource_usage::unordered_access);
    if (!dev->create_resource(td, nullptr, resource_usage::unordered_access, &g_dofCoc) || !dev->create_resource(cd, nullptr, resource_usage::unordered_access, &g_dofCocOnly) || !dev->create_resource(td, nullptr, resource_usage::unordered_access, &g_dofBlur)) {
        logmsg("PostDof: half-res textures %ux%u failed", hw, hh);
        for (resource* r : { &g_dofCoc, &g_dofCocOnly, &g_dofBlur }) if (r->handle) { dev->destroy_resource(*r); *r = { 0 }; }
        g_dofW = g_dofH = 0; return false;
    }
    dev->set_resource_name(g_dofCoc, "MGS4DLSS DoF color+CoC"); dev->set_resource_name(g_dofCocOnly, "MGS4DLSS DoF CoC"); dev->set_resource_name(g_dofBlur, "MGS4DLSS DoF blur");
    g_dofCocState = g_dofCocOnlyState = g_dofBlurState = resource_usage::unordered_access; g_dofW = hw; g_dofH = hh;
    logmsg("PostDof: half-res textures %ux%u", hw, hh);
    return true;
}
static DXGI_FORMAT dof_depth_srv_format(device* dev, resource depth, resource_desc* dd)
{
    *dd = dev->get_resource_desc(depth);
    DXGI_FORMAT f = static_cast<DXGI_FORMAT>(dd->texture.format);
    if (f == DXGI_FORMAT_R32_TYPELESS) f = DXGI_FORMAT_R32_FLOAT; else if (f == DXGI_FORMAT_R16_TYPELESS) f = DXGI_FORMAT_R16_UNORM; else if (f == DXGI_FORMAT_R24G8_TYPELESS) f = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    return f;
}
// Pass 1a, at the game's CoC draw (which is then skipped): the circle of confusion of the full half-res grid from the
// depth copy that draw was about to sample, with that draw's constants - the same inputs at the same moment as the
// game's own pass, so nothing the game does to the depth copy later in the frame can reach the blur. fullW/H = the
// scene target (the DLSS output size), kx/ky = this frame's dynamic-resolution scale (the CoC viewport is half the
// scene sub-rect). The game's command-list state is restored afterwards. Returns false if nothing was written.
static bool dof_coc_dispatch(command_list* cmd, device* dev, const cl_state& s, resource depth, uint32_t fullW, uint32_t fullH, float kx, float ky)
{
    if (!dof_init() || !dof_ensure_textures(dev, fullW, fullH)) return false;
    resource_desc dd; const DXGI_FORMAT depthFmt = dof_depth_srv_format(dev, depth, &dd);
    DofCB cb = {}; memcpy(cb.c, g_dofCb, sizeof(cb.c));
    cb.halfSize[0] = float(g_dofW); cb.halfSize[1] = float(g_dofH); cb.fullSize[0] = float(fullW); cb.fullSize[1] = float(fullH);
    // depth sampling exactly as the game's CoC pass: uv = (2 * halfResPixel + c12.xy) / c16.xy, the game's half-res pixel
    // being on the sub-rect grid (ours * k)
    const float* c12 = g_dofCb + 4 * 4; const float* c16 = g_dofCb + 8 * 4;
    cb.depthScale[0] = kx; cb.depthScale[1] = ky; cb.depthOff[0] = c12[0]; cb.depthOff[1] = c12[1];
    cb.depthSize[0] = c16[0] >= 1.0f ? c16[0] : float(dd.texture.width); cb.depthSize[1] = c16[1] >= 1.0f ? c16[1] : float(dd.texture.height);
    cb.cocUnorm = g_dofCocUnorm ? 1.0f : 0.0f; cb.radiusScale = g_cfgDofRadius;
    // the projection jitter shifts the rendered (depth) image by (signX*jx, -signY*jy) full-grid pixels; a depth-copy
    // texel is a sub-rect pixel, i.e. k full-grid pixels
    cb.depthJitter[0] = g_cfgJitter ? float(g_cfgDofJitterSign) * g_cfgJitterSignX * g_jitterX * kx : 0.0f;
    cb.depthJitter[1] = g_cfgJitter ? float(g_cfgDofJitterSign) * -g_cfgJitterSignY * g_jitterY * ky : 0.0f;
    const D3D12_GPU_VIRTUAL_ADDRESS cbAddr = dof_cb_write(cb, 0);
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDep = {}; srvDep.Format = depthFmt; srvDep.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; srvDep.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srvDep.Texture2D.MipLevels = 1;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavCoc = {}; uavCoc.Format = DXGI_FORMAT_R16_FLOAT; uavCoc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    ID3D12Resource* depRes = reinterpret_cast<ID3D12Resource*>(depth.handle); ID3D12Resource* cocRes = reinterpret_cast<ID3D12Resource*>(g_dofCocOnly.handle);
    // pass 1a: [depth srv, depth srv, coc uav, coc uav]
    g_d3d->CreateShaderResourceView(depRes, &srvDep, dof_cpu(0)); g_d3d->CreateShaderResourceView(depRes, &srvDep, dof_cpu(1));
    g_d3d->CreateUnorderedAccessView(cocRes, nullptr, &uavCoc, dof_cpu(2)); g_d3d->CreateUnorderedAccessView(cocRes, nullptr, &uavCoc, dof_cpu(3));
    ID3D12GraphicsCommandList* native = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd->get_native());
    cmd->barrier(depth, resource_usage::shader_resource_pixel, resource_usage::shader_resource_non_pixel);   // bound as the skipped draw's SRV
    if (g_dofCocOnlyState != resource_usage::unordered_access) { cmd->barrier(g_dofCocOnly, g_dofCocOnlyState, resource_usage::unordered_access); g_dofCocOnlyState = resource_usage::unordered_access; }
    ID3D12DescriptorHeap* heaps[1] = { g_mvHeap };
    native->SetDescriptorHeaps(1, heaps);
    native->SetComputeRootSignature(g_dofRootSig);
    native->SetComputeRootConstantBufferView(0, cbAddr);
    native->SetPipelineState(g_dofCocPso); native->SetComputeRootDescriptorTable(1, dof_gpu(0)); native->SetComputeRootDescriptorTable(2, dof_gpu(2));
    native->Dispatch((g_dofW + 7) / 8, (g_dofH + 7) / 8, 1);
    cmd->barrier(depth, resource_usage::shader_resource_non_pixel, resource_usage::shader_resource_pixel);
    cmd->barrier(g_dofCocOnly, resource_usage::unordered_access, resource_usage::shader_resource_non_pixel); g_dofCocOnlyState = resource_usage::shader_resource_non_pixel;
    restore_state(dev, cmd, s);
    g_dofCocReadyThisFrame = true; g_dofCocFrame = g_frame;
    return true;
}
// Passes 1b..3 on g_out (in unordered_access state, left in that state): the half-res color of the DLSS output is
// packed with this frame's CoC, the spiral gather runs, and the blurred layer is blended over the output. kx/ky = this
// frame's dynamic-resolution scale (spiral step, and the overlay mask was replayed at the sub-rect viewport). Returns
// false when this frame's CoC is missing or on another grid.
static bool dof_apply(command_list* cmd, device* dev, uint32_t outW, uint32_t outH, DXGI_FORMAT outFmt, float kx, float ky)
{
    if (!dof_init() || !g_out.handle) return false;
    if (!g_dofCocReadyThisFrame || g_dofCocFrame != g_frame || !g_dofCocOnly.handle) { static uint32_t n = 0; if (n++ < 5) logmsg("PostDof: f%u has no CoC of its own (ready %d, from frame %u) - not re-applied", g_frame, (int)g_dofCocReadyThisFrame, g_dofCocFrame); return false; }
    const uint32_t hw = (outW + 1) / 2, hh = (outH + 1) / 2;
    if (g_dofW != hw || g_dofH != hh) { static uint32_t n = 0; if (n++ < 5) logmsg("PostDof: f%u CoC grid %ux%u is not the output's %ux%u - not re-applied", g_frame, g_dofW, g_dofH, outW, outH); return false; }
    // constants: the game's rows + ours. Spiral step: the game steps c17.x/(1280,720) in the UV of a 3840-wide texture
    // whose top-left quadrant holds the half-res image, i.e. 2x that in the half-res image's own UV; a dynamic-resolution
    // sub-rect is stretched to the full grid afterwards, so the step grows by 1/k there.
    DofCB cb = {}; memcpy(cb.c, g_dofCb, sizeof(cb.c)); memcpy(cb.g, g_dofCbG, sizeof(cb.g));
    cb.halfSize[0] = float(hw); cb.halfSize[1] = float(hh); cb.fullSize[0] = float(outW); cb.fullSize[1] = float(outH);
    cb.depthScale[0] = kx; cb.depthScale[1] = ky;
    const float c17x = g_dofCbG[9 * 4];
    cb.stepUV[0] = g_cfgDofStep * c17x / (1280.0f * (kx > 0.05f ? kx : 1.0f)); cb.stepUV[1] = g_cfgDofStep * c17x / (720.0f * (ky > 0.05f ? ky : 1.0f));
    cb.cocUnorm = g_dofCocUnorm ? 1.0f : 0.0f; cb.radiusScale = g_cfgDofRadius; cb.maskScale[0] = kx; cb.maskScale[1] = ky;   // overlays were replayed at the sub-rect viewport
    cb.debugView = g_cfgDebugMode == 10 ? 1.0f : (g_cfgDebugMode == 11 ? 2.0f : (g_cfgDebugMode == 12 ? 3.0f : 0.0f));   // DoF layer views
    const bool haveMask = g_cfgDofMask && g_dofMask.handle && g_dofMaskRtv.handle;
    cb.hasMask = haveMask ? 1.0f : 0.0f;
    const D3D12_GPU_VIRTUAL_ADDRESS cbAddr = dof_cb_write(cb, 1);
    D3D12_SHADER_RESOURCE_VIEW_DESC srvOut = {}; srvOut.Format = outFmt; srvOut.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; srvOut.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srvOut.Texture2D.MipLevels = 1;
    D3D12_SHADER_RESOURCE_VIEW_DESC srvHalf = srvOut; srvHalf.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    D3D12_SHADER_RESOURCE_VIEW_DESC srvCoc = srvOut; srvCoc.Format = DXGI_FORMAT_R16_FLOAT;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavHalf = {}; uavHalf.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; uavHalf.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavOut = uavHalf; uavOut.Format = outFmt;
    ID3D12Resource* outRes = reinterpret_cast<ID3D12Resource*>(g_out.handle); ID3D12Resource* cocOnlyRes = reinterpret_cast<ID3D12Resource*>(g_dofCocOnly.handle);
    ID3D12Resource* cocRes = reinterpret_cast<ID3D12Resource*>(g_dofCoc.handle); ID3D12Resource* blurRes = reinterpret_cast<ID3D12Resource*>(g_dofBlur.handle);
    // pass 1b: [out srv, coc-only srv, color+coc uav, color+coc uav]
    g_d3d->CreateShaderResourceView(outRes, &srvOut, dof_cpu(4)); g_d3d->CreateShaderResourceView(cocOnlyRes, &srvCoc, dof_cpu(5));
    g_d3d->CreateUnorderedAccessView(cocRes, nullptr, &uavHalf, dof_cpu(6)); g_d3d->CreateUnorderedAccessView(cocRes, nullptr, &uavHalf, dof_cpu(7));
    // pass 2: [color+coc srv, color+coc srv, blur uav, blur uav]
    g_d3d->CreateShaderResourceView(cocRes, &srvHalf, dof_cpu(8)); g_d3d->CreateShaderResourceView(cocRes, &srvHalf, dof_cpu(9));
    g_d3d->CreateUnorderedAccessView(blurRes, nullptr, &uavHalf, dof_cpu(10)); g_d3d->CreateUnorderedAccessView(blurRes, nullptr, &uavHalf, dof_cpu(11));
    // pass 3: [blur srv, overlay mask srv (or the color+coc texture when there is no mask layer), out uav, out uav]
    if (haveMask) {
        if (!g_dofMaskCleared) {   // no overlay this frame: an empty mask
            if (g_dofMaskState != resource_usage::render_target) { cmd->barrier(g_dofMask, g_dofMaskState, resource_usage::render_target); g_dofMaskState = resource_usage::render_target; }
            const float zero[4] = { 0, 0, 0, 0 }; cmd->clear_render_target_view(g_dofMaskRtv, zero); g_dofMaskCleared = true;
        }
        if (g_dofMaskState != resource_usage::shader_resource_non_pixel) { cmd->barrier(g_dofMask, g_dofMaskState, resource_usage::shader_resource_non_pixel); g_dofMaskState = resource_usage::shader_resource_non_pixel; }
        D3D12_SHADER_RESOURCE_VIEW_DESC srvMask = srvOut; srvMask.Format = static_cast<DXGI_FORMAT>(g_dofMaskFmt);
        g_d3d->CreateShaderResourceView(blurRes, &srvHalf, dof_cpu(12)); g_d3d->CreateShaderResourceView(reinterpret_cast<ID3D12Resource*>(g_dofMask.handle), &srvMask, dof_cpu(13));
    } else { g_d3d->CreateShaderResourceView(blurRes, &srvHalf, dof_cpu(12)); g_d3d->CreateShaderResourceView(cocRes, &srvHalf, dof_cpu(13)); }
    g_d3d->CreateUnorderedAccessView(outRes, nullptr, &uavOut, dof_cpu(14)); g_d3d->CreateUnorderedAccessView(outRes, nullptr, &uavOut, dof_cpu(15));
    ID3D12GraphicsCommandList* native = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd->get_native());
    ID3D12DescriptorHeap* heaps[1] = { g_mvHeap };
    native->SetDescriptorHeaps(1, heaps);
    native->SetComputeRootSignature(g_dofRootSig);
    native->SetComputeRootConstantBufferView(0, cbAddr);
    // pass 1b
    cmd->barrier(g_out, resource_usage::unordered_access, resource_usage::shader_resource_non_pixel);
    if (g_dofCocOnlyState != resource_usage::shader_resource_non_pixel) { cmd->barrier(g_dofCocOnly, g_dofCocOnlyState, resource_usage::shader_resource_non_pixel); g_dofCocOnlyState = resource_usage::shader_resource_non_pixel; }
    if (g_dofCocState != resource_usage::unordered_access) { cmd->barrier(g_dofCoc, g_dofCocState, resource_usage::unordered_access); g_dofCocState = resource_usage::unordered_access; }
    native->SetPipelineState(g_dofPackPso); native->SetComputeRootDescriptorTable(1, dof_gpu(4)); native->SetComputeRootDescriptorTable(2, dof_gpu(6));
    native->Dispatch((hw + 7) / 8, (hh + 7) / 8, 1);
    // pass 2
    cmd->barrier(g_dofCoc, resource_usage::unordered_access, resource_usage::shader_resource_non_pixel); g_dofCocState = resource_usage::shader_resource_non_pixel;
    if (g_dofBlurState != resource_usage::unordered_access) { cmd->barrier(g_dofBlur, g_dofBlurState, resource_usage::unordered_access); g_dofBlurState = resource_usage::unordered_access; }
    native->SetPipelineState(g_dofGatherPso); native->SetComputeRootDescriptorTable(1, dof_gpu(8)); native->SetComputeRootDescriptorTable(2, dof_gpu(10));
    native->Dispatch((hw + 7) / 8, (hh + 7) / 8, 1);
    // pass 3
    cmd->barrier(g_dofBlur, resource_usage::unordered_access, resource_usage::shader_resource_non_pixel); g_dofBlurState = resource_usage::shader_resource_non_pixel;
    cmd->barrier(g_out, resource_usage::shader_resource_non_pixel, resource_usage::unordered_access);
    native->SetPipelineState(g_dofCompositePso); native->SetComputeRootDescriptorTable(1, dof_gpu(12)); native->SetComputeRootDescriptorTable(2, dof_gpu(14));
    native->Dispatch((outW + 7) / 8, (outH + 7) / 8, 1);
    cmd->barrier(g_out, resource_usage::unordered_access, resource_usage::unordered_access);   // UAV -> UAV: make the writes visible to the copy that follows
    return true;
}

// Debug: paint the motion-vector field into g_out (must be in unordered_access state).
// HUD-less color: g_hudless (a copy of the DLAA output, in UAV state) gets the pre-HUD capture wherever the UI layer has content.
static void hudless_dispatch(command_list* cmd, uint32_t w, uint32_t h, DXGI_FORMAT fmt)
{
    if (!g_mvReady || !g_hudlessPso) return;
    const uint32_t slot = 12 + (g_mvSlot % 4);
    const UINT inc = g_d3d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_mvHeap->GetCPUDescriptorHandleForHeapStart(); cpu.ptr += SIZE_T(slot) * 4 * inc;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_mvHeap->GetGPUDescriptorHandleForHeapStart(); gpu.ptr += UINT64(slot) * 4 * inc;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {}; srv.Format = fmt; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Texture2D.MipLevels = 1;
    g_d3d->CreateShaderResourceView(reinterpret_cast<ID3D12Resource*>(g_preHud.handle), &srv, cpu);
    D3D12_CPU_DESCRIPTOR_HANDLE h1 = cpu; h1.ptr += inc;
    g_d3d->CreateShaderResourceView(reinterpret_cast<ID3D12Resource*>(g_ui.handle), &srv, h1);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {}; uav.Format = fmt; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE h2 = cpu; h2.ptr += 2 * inc;
    g_d3d->CreateUnorderedAccessView(reinterpret_cast<ID3D12Resource*>(g_hudless.handle), nullptr, &uav, h2);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDummy = {}; uavDummy.Format = DXGI_FORMAT_R8_UNORM; uavDummy.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE h3 = cpu; h3.ptr += 3 * inc;
    g_d3d->CreateUnorderedAccessView(g_dummyUav, nullptr, &uavDummy, h3);
    ID3D12GraphicsCommandList* native = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd->get_native());
    ID3D12DescriptorHeap* heaps[1] = { g_mvHeap };
    native->SetDescriptorHeaps(1, heaps);
    native->SetComputeRootSignature(g_mvRootSig);
    native->SetPipelineState(g_hudlessPso);
    native->SetComputeRootConstantBufferView(0, g_mvCb->GetGPUVirtualAddress());   // unused by this shader
    native->SetComputeRootDescriptorTable(1, gpu);
    D3D12_GPU_DESCRIPTOR_HANDLE gpuUav = gpu; gpuUav.ptr += 2 * inc;
    native->SetComputeRootDescriptorTable(2, gpuUav);
    native->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
}
// DRS: resample the full-size DLSS output (g_out, NPSR) into the sub-rect of g_scratch (UAV).
// Bilinear supersampled rescale of src (srcW x srcH, in NPSR state) into the top-left dstW x dstH of dst (UAV state).
// DRS=2 resamples the DLSS output into the game's sub-rect; the frame-generation hints use it to bring the HUD-less
// image and the UI layer to the backbuffer size. slotBase = descriptor slot group, cbSlot = constant ring (0..3 within).
static void resample_dispatch(command_list* cmd, resource src, resource dst, uint32_t dstW, uint32_t dstH, uint32_t srcW, uint32_t srcH, DXGI_FORMAT fmt, uint32_t slotBase, uint32_t cbBase)
{
    if (!g_mvReady || !g_resamplePso || !src.handle || !dst.handle) return;
    const uint32_t slot = slotBase + (g_mvSlot % 4), cbSlot = cbBase + (g_mvSlot % 4);
    float cb[4] = { float(dstW), float(dstH), float(srcW), float(srcH) };
    memcpy(g_mvCbPtr + cbSlot * 256, cb, sizeof(cb));
    const UINT inc = g_d3d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_mvHeap->GetCPUDescriptorHandleForHeapStart(); cpu.ptr += SIZE_T(slot) * 4 * inc;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_mvHeap->GetGPUDescriptorHandleForHeapStart(); gpu.ptr += UINT64(slot) * 4 * inc;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {}; srv.Format = fmt; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Texture2D.MipLevels = 1;
    g_d3d->CreateShaderResourceView(reinterpret_cast<ID3D12Resource*>(src.handle), &srv, cpu);
    D3D12_CPU_DESCRIPTOR_HANDLE h1 = cpu; h1.ptr += inc;
    g_d3d->CreateShaderResourceView(reinterpret_cast<ID3D12Resource*>(src.handle), &srv, h1);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {}; uav.Format = fmt; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE h2 = cpu; h2.ptr += 2 * inc;
    g_d3d->CreateUnorderedAccessView(reinterpret_cast<ID3D12Resource*>(dst.handle), nullptr, &uav, h2);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDummy = {}; uavDummy.Format = DXGI_FORMAT_R8_UNORM; uavDummy.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE h3 = cpu; h3.ptr += 3 * inc;
    g_d3d->CreateUnorderedAccessView(g_dummyUav, nullptr, &uavDummy, h3);
    ID3D12GraphicsCommandList* native = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd->get_native());
    ID3D12DescriptorHeap* heaps[1] = { g_mvHeap };
    native->SetDescriptorHeaps(1, heaps);
    native->SetComputeRootSignature(g_mvRootSig);
    native->SetPipelineState(g_resamplePso);
    native->SetComputeRootConstantBufferView(0, g_mvCb->GetGPUVirtualAddress() + cbSlot * 256);
    native->SetComputeRootDescriptorTable(1, gpu);
    D3D12_GPU_DESCRIPTOR_HANDLE gpuUav = gpu; gpuUav.ptr += 2 * inc;
    native->SetComputeRootDescriptorTable(2, gpuUav);
    native->Dispatch((dstW + 7) / 8, (dstH + 7) / 8, 1);
}
// Frame generation in a window whose backbuffer is not the render size (3619x2036 window, 3784x2128 render): DLSS-G only
// takes the HUD-less color and the UI layer at the color (backbuffer) size, and dropping them leaves it to guess the
// HUD from the backbuffer - artefacts on high-contrast detail in generated frames. So they are rescaled into
// backbuffer-sized copies at the composite draw (when the HUD layer of the frame is complete) and tagged valid-until-present.
static resource g_fgHudlessBb = { 0 }, g_fgUiBb = { 0 }; static resource_usage g_fgHudlessBbState = resource_usage::unordered_access, g_fgUiBbState = resource_usage::unordered_access;
static uint32_t g_fgBbW = 0, g_fgBbH = 0; static format g_fgBbFmt = format::unknown;
static bool g_fgScalePending = false; static uint32_t g_fgScaledFrames = 0;
static resource g_fgSrcHudless = { 0 }, g_fgSrcUi = { 0 }; static resource_usage* g_fgSrcHudlessState = nullptr; static resource_usage* g_fgSrcUiState = nullptr;
static uint32_t g_fgSrcW = 0, g_fgSrcH = 0;
static bool fg_bb_textures(device* dev, format fmt)
{
    if (g_fgHudlessBb.handle && g_fgUiBb.handle && g_fgBbW == g_bbW && g_fgBbH == g_bbH && g_fgBbFmt == fmt) return true;
    if (g_fgHudlessBb.handle) { dev->destroy_resource(g_fgHudlessBb); g_fgHudlessBb = { 0 }; }
    if (g_fgUiBb.handle) { dev->destroy_resource(g_fgUiBb); g_fgUiBb = { 0 }; }
    const resource_desc td(g_bbW, g_bbH, 1, 1, fmt, 1, memory_heap::default_, resource_usage::shader_resource | resource_usage::unordered_access);
    if (!dev->create_resource(td, nullptr, resource_usage::unordered_access, &g_fgHudlessBb) || !dev->create_resource(td, nullptr, resource_usage::unordered_access, &g_fgUiBb)) {
        logmsg("FG: backbuffer-sized hint textures %ux%u failed", g_bbW, g_bbH);
        if (g_fgHudlessBb.handle) { dev->destroy_resource(g_fgHudlessBb); g_fgHudlessBb = { 0 }; } return false;
    }
    dev->set_resource_name(g_fgHudlessBb, "MGS4DLSS FG HUD-less (backbuffer size)"); dev->set_resource_name(g_fgUiBb, "MGS4DLSS FG UI layer (backbuffer size)");
    g_fgHudlessBbState = g_fgUiBbState = resource_usage::unordered_access; g_fgBbW = g_bbW; g_fgBbH = g_bbH; g_fgBbFmt = fmt;
    logmsg("FG: HUD-less / UI hints rescaled to the backbuffer size %ux%u (render %ux%u)", g_bbW, g_bbH, g_fgSrcW, g_fgSrcH);
    return true;
}
static resource_usage* fg_state_of(uint64_t h)
{
    if (h && h == g_out.handle) return &g_outState;
    if (h && h == g_hudless.handle) return &g_hudlessState;
    if (h && h == g_preHud.handle) return &g_preHudState;
    if (h && h == g_ui.handle) return &g_uiState;
    return nullptr;
}
// At the composite draw: rescale the sources tagged this frame into the backbuffer-sized copies (left in NPSR for SL).
static void fg_scale_hints(command_list* cmd)
{
    if (!g_mvReady || !g_fgBbW) return;
    const DXGI_FORMAT fmt = static_cast<DXGI_FORMAT>(g_fgBbFmt);
    struct job { resource src; resource_usage* srcState; resource dst; resource_usage* dstState; uint32_t slot, cb; };
    const job jobs[2] = { { g_fgSrcHudless, g_fgSrcHudlessState, g_fgHudlessBb, &g_fgHudlessBbState, 24, 8 }, { g_fgSrcUi, g_fgSrcUiState, g_fgUiBb, &g_fgUiBbState, 28, 12 } };
    for (const job& j : jobs) {
        if (!j.src.handle || !j.srcState || !j.dst.handle || !is_live(j.src.handle)) continue;
        const resource_usage from = *j.srcState;
        if (from != resource_usage::shader_resource_non_pixel) cmd->barrier(j.src, from, resource_usage::shader_resource_non_pixel);
        if (*j.dstState != resource_usage::unordered_access) { cmd->barrier(j.dst, *j.dstState, resource_usage::unordered_access); *j.dstState = resource_usage::unordered_access; }
        resample_dispatch(cmd, j.src, j.dst, g_fgBbW, g_fgBbH, g_fgSrcW, g_fgSrcH, fmt, j.slot, j.cb);
        cmd->barrier(j.dst, resource_usage::unordered_access, resource_usage::shader_resource_non_pixel); *j.dstState = resource_usage::shader_resource_non_pixel;
        if (from != resource_usage::shader_resource_non_pixel) cmd->barrier(j.src, resource_usage::shader_resource_non_pixel, from);
    }
    g_fgScaledFrames++;
}
static void vis_dispatch(command_list* cmd, uint32_t inW, uint32_t inH, uint32_t outW, uint32_t outH, float blend = 0.0f)
{
    if (!g_mvReady || !g_visPso) return;
    const uint32_t slot = 8 + (g_mvSlot % 4);
    VisCB cb = {}; cb.inSize[0] = float(inW); cb.inSize[1] = float(inH); cb.outSize[0] = float(outW); cb.outSize[1] = float(outH); cb.scale = 0.05f; cb.blend = blend;   // 10 px of motion = full swing
    memcpy(g_mvCbPtr + (4 + slot % 4) * 256, &cb, sizeof(cb));
    // descriptors: [0] SRV motion vectors, [1] SRV mask, [2] UAV output, [3] UAV mask (unused dummy)
    const UINT inc = g_d3d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_mvHeap->GetCPUDescriptorHandleForHeapStart(); cpu.ptr += SIZE_T(slot) * 4 * inc;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_mvHeap->GetGPUDescriptorHandleForHeapStart(); gpu.ptr += UINT64(slot) * 4 * inc;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {}; srv.Format = DXGI_FORMAT_R16G16_FLOAT; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Texture2D.MipLevels = 1;
    g_d3d->CreateShaderResourceView(reinterpret_cast<ID3D12Resource*>(g_mv.handle), &srv, cpu);
    D3D12_CPU_DESCRIPTOR_HANDLE h1 = cpu; h1.ptr += inc;
    D3D12_SHADER_RESOURCE_VIEW_DESC srvMask = srv; srvMask.Format = DXGI_FORMAT_R8_UNORM;
    g_d3d->CreateShaderResourceView(reinterpret_cast<ID3D12Resource*>(g_mask.handle), &srvMask, h1);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {}; uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE h2 = cpu; h2.ptr += 2 * inc;
    g_d3d->CreateUnorderedAccessView(reinterpret_cast<ID3D12Resource*>(g_out.handle), nullptr, &uav, h2);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDummy = {}; uavDummy.Format = DXGI_FORMAT_R8_UNORM; uavDummy.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE h3 = cpu; h3.ptr += 3 * inc;
    g_d3d->CreateUnorderedAccessView(g_dummyUav, nullptr, &uavDummy, h3);
    if (g_maskState != resource_usage::shader_resource_non_pixel) { cmd->barrier(g_mask, g_maskState, resource_usage::shader_resource_non_pixel); g_maskState = resource_usage::shader_resource_non_pixel; }
    ID3D12GraphicsCommandList* native = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd->get_native());
    ID3D12DescriptorHeap* heaps[1] = { g_mvHeap };
    native->SetDescriptorHeaps(1, heaps);
    native->SetComputeRootSignature(g_mvRootSig);
    native->SetPipelineState(g_visPso);
    native->SetComputeRootConstantBufferView(0, g_mvCb->GetGPUVirtualAddress() + (4 + slot % 4) * 256);
    native->SetComputeRootDescriptorTable(1, gpu);
    D3D12_GPU_DESCRIPTOR_HANDLE gpuUav = gpu; gpuUav.ptr += 2 * inc;
    native->SetComputeRootDescriptorTable(2, gpuUav);
    native->Dispatch((outW + 7) / 8, (outH + 7) / 8, 1);
}

// ---- draw-constant analysis (Phase 1b: find the view-projection matrix) --------------------------------------------
static uint32_t g_dumpUntil = 0;           // frame index until which scene draw constants are analyzed
static uint32_t g_dumpDrawsLogged = 0;
struct block_stat { uint32_t count = 0; float row0[4] = {}; };
static std::unordered_map<uint64_t, block_stat> g_blockHist;   // (offset << 48) ^ hash(64 bytes) -> stats
static bool dumping() { return g_frame < g_dumpUntil; }

static bool read_cbv(const cl_state& s, int param, float* out, size_t nfloats)
{
    static int fails = 0;
    if (!s.cbv_set[param] || !s.cbv_res[param].handle) { if (fails++ < 3) logmsg("read_cbv: root[%d] not a CBV / no buffer (set=%d res=%p)", param, (int)s.cbv_set[param], (void*)s.cbv_res[param].handle); return false; }
    ID3D12Resource* r = reinterpret_cast<ID3D12Resource*>(s.cbv_res[param].handle);
    uint64_t size = 0; uint8_t* base = map_upload(r, &size);
    if (!base) { if (fails++ < 3) logmsg("read_cbv: buffer %p is not CPU-readable (not an upload heap)", (void*)r); return false; }
    if (s.cbv_off[param] + nfloats * 4 > size) { if (fails++ < 3) logmsg("read_cbv: offset %llu + %zu > buffer size %llu", (unsigned long long)s.cbv_off[param], nfloats * 4, (unsigned long long)size); return false; }
    memcpy(out, base + s.cbv_off[param], nfloats * 4);
    return true;
}
static void analyse_scene_draw(const cl_state& s)
{
    float c[128];
    if (!read_cbv(s, 2, c, 128)) return;
    if (g_dumpDrawsLogged < 6) {
        g_dumpDrawsLogged++;
        logmsg("draw constants (root[2], pso=%p, rt=%p ds=%p):", (void*)s.pso, (void*)s.rt.handle, (void*)s.ds.handle);
        for (int i = 0; i < 16; ++i) logmsg("   c[%2d] = %10.4f %10.4f %10.4f %10.4f", i, c[i * 4], c[i * 4 + 1], c[i * 4 + 2], c[i * 4 + 3]);
    }
    for (uint32_t off = 0; off + 16 <= 128; off += 4) {
        const float* b = c + off;
        bool zero = true, ident = true, finite = true;
        for (int i = 0; i < 16; ++i) { if (b[i] != 0) zero = false; if (b[i] != ((i % 5 == 0) ? 1.0f : 0.0f)) ident = false; if (!(b[i] == b[i]) || fabsf(b[i]) > 1e6f) finite = false; }
        if (zero || ident || !finite) continue;
        uint64_t hsh = 1469598103934665603ull; const uint32_t* u = reinterpret_cast<const uint32_t*>(b);
        for (int i = 0; i < 16; ++i) { hsh ^= u[i]; hsh *= 1099511628211ull; }
        block_stat& st = g_blockHist[(uint64_t(off) << 48) ^ (hsh & 0xFFFFFFFFFFFFull)];
        if (st.count++ == 0) memcpy(st.row0, b, 16);
    }
}
static void report_blocks()
{
    std::vector<std::pair<uint64_t, block_stat>> v(g_blockHist.begin(), g_blockHist.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second.count > b.second.count; });
    logmsg("f%u: most shared 4x4 blocks across scene draws (offset in float4s, draws sharing it, first row):", g_frame);
    for (size_t i = 0; i < v.size() && i < 8; ++i)
        logmsg("   c[%2llu] x%-5u  %10.4f %10.4f %10.4f %10.4f", (unsigned long long)(v[i].first >> 48), v[i].second.count, v[i].second.row0[0], v[i].second.row0[1], v[i].second.row0[2], v[i].second.row0[3]);
    g_blockHist.clear();
}

static const char* ngx_str(NVSDK_NGX_Result r)
{
    static char buf[32]; snprintf(buf, sizeof(buf), "0x%08X", (unsigned)r); return buf;
}

static bool ngx_init(device* dev)
{
    if (g_ngxInitTried) return g_ngxReady;
    g_ngxInitTried = true;
    g_d3d = reinterpret_cast<ID3D12Device*>(dev->get_native());

    static wchar_t pathBuf[MAX_PATH]; wcscpy_s(pathBuf, g_gameDirW);
    const wchar_t* paths[] = { pathBuf };
    NVSDK_NGX_FeatureCommonInfo info = {};
    info.PathListInfo.Path = paths; info.PathListInfo.Length = 1;
    info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
    info.LoggingInfo.LoggingCallback = [](const char* msg, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature f) { if (msg) { char b[600]; strncpy_s(b, msg, _TRUNCATE); size_t n = strlen(b); while (n && (b[n - 1] == 10 || b[n - 1] == 13)) b[--n] = 0; logmsg("[NGX %d] %s", (int)f, b); } };

    wchar_t appData[MAX_PATH]; swprintf_s(appData, L"%s\\logs", g_gameDirW);
    // NGX is process-wide. When Streamline is active its common plugin has already initialized NGX with the device
    // the game's queue reports (ReShade's proxy); initializing again with the native device gives NGX two device
    // objects for one adapter and the DLSS DLL crashed in D3D12Core at CreateFeature. Use the same object.
    ID3D12Device* ngxDev = fg::sl_device() ? fg::sl_device() : g_d3d;
    NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init_with_ProjectID("7a2f8c3e-5d41-4b9a-9e0c-3f6d2b1a8c47", NVSDK_NGX_ENGINE_TYPE_CUSTOM, "0.1",
                                                            appData, ngxDev, &info, NVSDK_NGX_Version_API);
    logmsg("NGX D3D12 Init_with_ProjectID (device %p%s) -> %s", (void*)ngxDev, ngxDev == g_d3d ? "" : ", Streamline's", ngx_str(r));
    if (NVSDK_NGX_FAILED(r)) return false;

    NVSDK_NGX_Parameter* caps = nullptr;
    r = NVSDK_NGX_D3D12_GetCapabilityParameters(&caps);
    if (NVSDK_NGX_FAILED(r) || !caps) { logmsg("NGX GetCapabilityParameters -> %s", ngx_str(r)); return false; }
    int avail = 0; NVSDK_NGX_Parameter_GetI(caps, NVSDK_NGX_Parameter_SuperSampling_Available, &avail);
    logmsg("NGX SuperSampling.Available=%d", avail);
    if (!avail) return false;
    g_ngxCaps = caps;

    r = NVSDK_NGX_D3D12_AllocateParameters(&g_ngxParams);
    if (NVSDK_NGX_FAILED(r) || !g_ngxParams) { logmsg("NGX AllocateParameters -> %s", ngx_str(r)); return false; }
    g_ngxReady = true;
    return true;
}

// Decide the render resolution for the configured mode (needs NGX + InternalRes). Called once at device creation.
static void setup_scaling()
{
    g_renderW = g_internalW; g_renderH = g_internalH; g_scaling = false;
    if (!g_internalW || !g_ngxReady) return;
    if (g_cfgRenderResW && g_cfgRenderResH) {
        // RenderRes: an explicit source resolution instead of the ratio a mode implies. The target's aspect is kept
        // (the requested height rules, the width follows), a source at or above the target means DLAA, and the NGX
        // mode - which selects the model preset and the size range the driver enforces - is the one whose optimal
        // size is closest among those whose range holds the request (1440p -> Quality, 1080p -> Performance at a 4K
        // target). Ultra Performance is pinned at a third of the target, so a request no range holds (720p) is
        // clamped into the closest range and the clamp is logged.
        uint32_t rh = g_cfgRenderResH & ~1u, rw = (uint32_t)((uint64_t)rh * g_internalW / g_internalH) & ~1u;
        if (rw != g_cfgRenderResW) logmsg("RenderRes %ux%u: the width follows the target's aspect -> %ux%u", g_cfgRenderResW, g_cfgRenderResH, rw, rh);
        if (rw >= g_internalW || rh >= g_internalH) { logmsg("RenderRes %ux%u is not below the target %ux%u: DLAA", rw, rh, g_internalW, g_internalH); g_cfgMode = NVSDK_NGX_PerfQuality_Value_DLAA; strcpy_s(g_cfgModeName, "DLAA"); return; }
        static const NVSDK_NGX_PerfQuality_Value modes[4] = { NVSDK_NGX_PerfQuality_Value_MaxQuality, NVSDK_NGX_PerfQuality_Value_Balanced, NVSDK_NGX_PerfQuality_Value_MaxPerf, NVSDK_NGX_PerfQuality_Value_UltraPerformance };
        static const char* names[4] = { "Quality", "Balanced", "Performance", "UltraPerformance" };
        int pickIn = -1, pickOut = -1; unsigned distIn = ~0u, distOut = ~0u, outW = 0, outH = 0;
        for (int i = 0; i < 4; ++i) {
            unsigned ow = 0, oh = 0, xw = 0, xh = 0, nw = 0, nh = 0; float sh = 0;
            if (NVSDK_NGX_FAILED(NGX_DLSS_GET_OPTIMAL_SETTINGS(g_ngxCaps, g_internalW, g_internalH, modes[i], &ow, &oh, &xw, &xh, &nw, &nh, &sh)) || !ow || !oh) continue;
            const unsigned dOpt = (ow > rw ? ow - rw : rw - ow) + (oh > rh ? oh - rh : rh - oh);
            if (rw >= nw && rw <= xw && rh >= nh && rh <= xh) { if (dOpt < distIn) { distIn = dOpt; pickIn = i; } continue; }
            const unsigned cw = rw < nw ? nw : (rw > xw ? xw : rw), ch = rh < nh ? nh : (rh > xh ? xh : rh);
            const unsigned dClamp = (cw > rw ? cw - rw : rw - cw) + (ch > rh ? ch - rh : rh - ch);
            if (dClamp < distOut) { distOut = dClamp; pickOut = i; outW = cw; outH = ch; }
        }
        if (pickIn < 0 && pickOut < 0) { logmsg("RenderRes %ux%u: NGX reported no mode ranges; staying at DLAA", rw, rh); g_cfgMode = NVSDK_NGX_PerfQuality_Value_DLAA; strcpy_s(g_cfgModeName, "DLAA"); return; }
        const int pick = pickIn >= 0 ? pickIn : pickOut;
        if (pickIn < 0) { logmsg("RenderRes %ux%u is outside every mode's accepted range; the closest, %s, takes %ux%u", rw, rh, names[pick], outW, outH); rw = outW & ~1u; rh = outH & ~1u; }
        g_cfgMode = modes[pick]; strcpy_s(g_cfgModeName, names[pick]);
        g_renderW = rw; g_renderH = rh;
        g_scaling = g_renderW < g_internalW || g_renderH < g_internalH;
        logmsg("RenderRes: internal %ux%u -> render %ux%u under the %s model", g_internalW, g_internalH, g_renderW, g_renderH, g_cfgModeName);
        return;
    }
    if (g_cfgMode == NVSDK_NGX_PerfQuality_Value_DLAA) return;
    unsigned optW = 0, optH = 0, maxW, maxH, minW, minH; float sharp = 0;
    NVSDK_NGX_Result r = NGX_DLSS_GET_OPTIMAL_SETTINGS(g_ngxCaps, g_internalW, g_internalH, g_cfgMode, &optW, &optH, &maxW, &maxH, &minW, &minH, &sharp);
    if (NVSDK_NGX_FAILED(r) || optW == 0 || optH == 0) {
        logmsg("NGX optimal settings for mode %s failed (%s); staying at DLAA", g_cfgModeName, ngx_str(r));
        g_cfgMode = NVSDK_NGX_PerfQuality_Value_DLAA; strcpy_s(g_cfgModeName, "DLAA"); return;
    }
    g_renderW = optW & ~1u; g_renderH = optH & ~1u;
    g_scaling = g_renderW < g_internalW || g_renderH < g_internalH;
    logmsg("mode %s: internal %ux%u -> render %ux%u (NGX optimal %ux%u, range %ux%u..%ux%u)", g_cfgModeName, g_internalW, g_internalH, g_renderW, g_renderH, optW, optH, minW, minH, maxW, maxH);
}

static void release_dlss_resources(device* dev)
{
    if (g_dlss) { NVSDK_NGX_D3D12_ReleaseFeature(g_dlss); g_dlss = nullptr; }
    if (g_mvRtv.handle) { dev->destroy_resource_view(g_mvRtv); g_mvRtv = { 0 }; }
    if (g_mv.handle) { dev->destroy_resource(g_mv); g_mv = { 0 }; }
    if (g_depthFull.handle) { dev->destroy_resource(g_depthFull); g_depthFull = { 0 }; }
    if (g_feedMvRtv.handle) { dev->destroy_resource_view(g_feedMvRtv); g_feedMvRtv = { 0 }; }
    if (g_feedMv.handle) { dev->destroy_resource(g_feedMv); g_feedMv = { 0 }; }
    if (g_feedDepth.handle) { dev->destroy_resource(g_feedDepth); g_feedDepth = { 0 }; }
    if (g_out.handle) { dev->destroy_resource(g_out); g_out = { 0 }; }
    if (g_dynDsv.handle) { dev->destroy_resource_view(g_dynDsv); g_dynDsv = { 0 }; }
    if (g_dynDepth.handle) { dev->destroy_resource(g_dynDepth); g_dynDepth = { 0 }; }
    if (g_mask.handle) { dev->destroy_resource(g_mask); g_mask = { 0 }; }
    if (g_uiRtv.handle) { dev->destroy_resource_view(g_uiRtv); g_uiRtv = { 0 }; }
    if (g_ui.handle) { dev->destroy_resource(g_ui); g_ui = { 0 }; }
    if (g_preHud.handle) { dev->destroy_resource(g_preHud); g_preHud = { 0 }; }
    if (g_keep.handle) { dev->destroy_resource(g_keep); g_keep = { 0 }; g_keepValid = false; }
    if (g_hudless.handle) { dev->destroy_resource(g_hudless); g_hudless = { 0 }; }
    if (g_scratch.handle) { dev->destroy_resource(g_scratch); g_scratch = { 0 }; }
    if (g_fgHudlessBb.handle) { dev->destroy_resource(g_fgHudlessBb); g_fgHudlessBb = { 0 }; }
    if (g_fgUiBb.handle) { dev->destroy_resource(g_fgUiBb); g_fgUiBb = { 0 }; }
    g_fgBbW = g_fgBbH = 0; g_fgScalePending = false;
    if (g_dofCoc.handle) { dev->destroy_resource(g_dofCoc); g_dofCoc = { 0 }; }
    if (g_dofCocOnly.handle) { dev->destroy_resource(g_dofCocOnly); g_dofCocOnly = { 0 }; }
    if (g_dofBlur.handle) { dev->destroy_resource(g_dofBlur); g_dofBlur = { 0 }; }
    if (g_dofMaskRtv.handle) { dev->destroy_resource_view(g_dofMaskRtv); g_dofMaskRtv = { 0 }; }
    if (g_dofMask.handle) { dev->destroy_resource(g_dofMask); g_dofMask = { 0 }; }
    g_dofW = g_dofH = 0; g_dofCocReadyThisFrame = false;
}

// Whether an NGX-hooking add-on (RenoDX's DLSS add-on) has detoured the NGX D3D12 CreateFeature export: Detours rewrites the
// function's first bytes with a jump. Such add-ons capture their "DLSS contract" from CreateFeature, so the first
// create must happen after their hooks exist.
static int ngx_create_hooked()
{
    HMODULE m = GetModuleHandleA("_nvngx.dll");
    if (!m) return -1;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(GetProcAddress(m, "NVSDK_NGX_D3D12_CreateFeature"));
    if (!p) return -1;
    return (p[0] == 0xE9 || (p[0] == 0xFF && p[1] == 0x25) || p[0] == 0xEB) ? 1 : 0;
}
static bool create_feature(command_list* cmd, uint32_t w, uint32_t h, uint32_t outW, uint32_t outH, format fmt)
{
    if (g_cfgMode != NVSDK_NGX_PerfQuality_Value_UltraPerformance) {
        NVSDK_NGX_Parameter_SetUI(g_ngxParams, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, (unsigned)g_cfgPreset);
        NVSDK_NGX_Parameter_SetUI(g_ngxParams, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, (unsigned)g_cfgPreset);
        NVSDK_NGX_Parameter_SetUI(g_ngxParams, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, (unsigned)g_cfgPreset);
        NVSDK_NGX_Parameter_SetUI(g_ngxParams, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, (unsigned)g_cfgPreset);
    }
    NVSDK_NGX_DLSS_Create_Params cp = {};
    cp.Feature.InWidth = w; cp.Feature.InHeight = h;
    cp.Feature.InTargetWidth = outW; cp.Feature.InTargetHeight = outH;
    // With DRS handling, DLAA is created as a Quality feature: same model/preset, but NGX accepts render sub-rects
    // down to its dynamic minimum (about 50 %) instead of the fixed DLAA size.
    cp.Feature.InPerfQualityValue = (g_cfgDRS == 2 && g_cfgMode == NVSDK_NGX_PerfQuality_Value_DLAA) ? NVSDK_NGX_PerfQuality_Value_MaxQuality : g_cfgMode;
    if (g_ngxCaps) { unsigned ow = 0, oh = 0, xw = 0, xh = 0, nw = 0, nh = 0; float sh = 0; if (!NVSDK_NGX_FAILED(NGX_DLSS_GET_OPTIMAL_SETTINGS(g_ngxCaps, outW, outH, cp.Feature.InPerfQualityValue, &ow, &oh, &xw, &xh, &nw, &nh, &sh))) { g_drsMinW = nw; g_drsMinH = nh; } }
    cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
    ID3D12GraphicsCommandList* native = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd->get_native());
    NVSDK_NGX_Handle* handle = nullptr;
    logmsg("NGX CreateFeature DLSS: cmd %p (%s), nvngx_dlss.dll %s, _nvngx %p, DLSS5 add-on %s", (void*)native, fg::inside_streamline() ? "inside SL?" : "game", GetModuleHandleA("nvngx_dlss.dll") ? "loaded" : "not loaded", (void*)GetModuleHandleA("_nvngx.dll"), g_nrAddonLoaded ? "loaded" : "absent");
    const int hookedBefore = ngx_create_hooked();
    NVSDK_NGX_Result r = NGX_D3D12_CREATE_DLSS_EXT(native, 1, 1, &handle, g_ngxParams, &cp);
    const int hookedAfter = ngx_create_hooked();
    if (!g_dlss && !g_recreated && !g_oldFeature) {   // first create of this run
        logmsg("NGX CreateFeature export hooked by an add-on: before %d, after %d%s", hookedBefore, hookedAfter, g_nrAddonLoaded ? "" : " (no NGX post-processing add-on loaded)");
        if (hookedBefore != 1 && hookedAfter == 1 && g_nrAddonLoaded) { g_autoRecreateAt = 4; logmsg("the add-on hooked NGX during our first CreateFeature (it did not see it) - re-creating the feature once at evaluation %u, while the scene fades in", g_autoRecreateAt); }
    }
    logmsg("NGX CreateFeature DLSS (%s %ux%u -> %ux%u, fmt=%u, preset=%d) at frame %u -> %s", g_cfgModeName, w, h, outW, outH, (unsigned)fmt, g_cfgPreset, g_frame, ngx_str(r));
    if (NVSDK_NGX_FAILED(r)) return false;
    g_dlss = handle;
    g_dlssW = w; g_dlssH = h; g_dlssOutW = outW; g_dlssOutH = outH; g_dlssFmt = fmt;
    g_featureCreatedThisFrame = true; g_createdFrame = g_frame;
    return true;
}

static bool ensure_resources(device* dev, command_list* cmd, uint32_t w, uint32_t h, uint32_t outW, uint32_t outH, format fmt)
{
    const bool same = g_dlssW == w && g_dlssH == h && g_dlssOutW == outW && g_dlssOutH == outH && g_dlssFmt == fmt;
    if (g_dlss && same) return true;
    if (!g_dlss && g_mv.handle && g_out.handle && same) return create_feature(cmd, w, h, outW, outH, fmt);
    release_dlss_resources(dev);

    if (!dev->create_resource(resource_desc(w, h, 1, 1, format::r16g16_float, 1, memory_heap::default_, resource_usage::render_target | resource_usage::shader_resource | resource_usage::unordered_access),
                              nullptr, resource_usage::render_target, &g_mv)) { logmsg("create MV texture failed"); return false; }
    if (!dev->create_resource(resource_desc(outW, outH, 1, 1, fmt, 1, memory_heap::default_, resource_usage::unordered_access | resource_usage::copy_source | resource_usage::shader_resource | resource_usage::render_target),
                              nullptr, resource_usage::unordered_access, &g_out)) { logmsg("create output texture failed"); return false; }
    g_outState = resource_usage::unordered_access;
    dev->set_resource_name(g_mv, "MGS4DLSS motion vectors");
    if (dev->create_resource(resource_desc(w, h, 1, 1, format::r32_float, 1, memory_heap::default_, resource_usage::unordered_access | resource_usage::shader_resource), nullptr, resource_usage::unordered_access, &g_depthFull)) { dev->set_resource_name(g_depthFull, "MGS4DLSS depth (full grid)"); g_depthFullState = resource_usage::unordered_access; }
    else logmsg("create full-grid depth texture failed (dynamic resolution will not be corrected)");
    // the video call's caller feed: its depth copy and its objects' vectors (see g_feedLayoutRect)
    if (dev->create_resource(resource_desc(w, h, 1, 1, format::r32_float, 1, memory_heap::default_, resource_usage::unordered_access | resource_usage::shader_resource), nullptr, resource_usage::unordered_access, &g_feedDepth)) { dev->set_resource_name(g_feedDepth, "MGS4DLSS caller feed depth"); g_feedDepthState = resource_usage::unordered_access; }
    else logmsg("create caller-feed depth texture failed");
    if (dev->create_resource(resource_desc(w, h, 1, 1, format::r16g16_float, 1, memory_heap::default_, resource_usage::render_target | resource_usage::shader_resource), nullptr, resource_usage::render_target, &g_feedMv)) {
        dev->set_resource_name(g_feedMv, "MGS4DLSS caller feed motion vectors"); g_feedMvState = resource_usage::render_target;
        if (!dev->create_resource_view(g_feedMv, resource_usage::render_target, resource_view_desc(format::r16g16_float), &g_feedMvRtv)) { logmsg("create caller-feed MV RTV failed"); g_feedMvRtv = { 0 }; }
    } else logmsg("create caller-feed MV texture failed");
    dev->set_resource_name(g_out, "MGS4DLSS output");
    // Phase 2: private depth for replayed dynamic draws + the mask DLSS gets as bias-current-color
    if (dev->create_resource(resource_desc(w, h, 1, 1, format::r24_g8_typeless, 1, memory_heap::default_, resource_usage::depth_stencil | resource_usage::shader_resource),
                             nullptr, resource_usage::depth_stencil_write, &g_dynDepth)) {
        dev->set_resource_name(g_dynDepth, "MGS4DLSS dynamic depth");
        g_dynState = resource_usage::depth_stencil_write;
        if (!dev->create_resource_view(g_dynDepth, resource_usage::depth_stencil, resource_view_desc(format::d24_unorm_s8_uint), &g_dynDsv)) { logmsg("dynamic depth DSV failed"); dev->destroy_resource(g_dynDepth); g_dynDepth = { 0 }; }
        else { const float zero = 0.0f; cmd->clear_depth_stencil_view(g_dynDsv, &zero, nullptr); }
    } else logmsg("dynamic depth texture failed");
    if (dev->create_resource(resource_desc(w, h, 1, 1, format::r8_unorm, 1, memory_heap::default_, resource_usage::unordered_access | resource_usage::shader_resource),
                             nullptr, resource_usage::unordered_access, &g_mask)) { dev->set_resource_name(g_mask, "MGS4DLSS dynamic mask"); g_maskState = resource_usage::unordered_access; }
    else { logmsg("mask texture failed"); return false; }
    if (dev->create_resource(resource_desc(w, h, 1, 1, fmt, 1, memory_heap::default_, resource_usage::render_target | resource_usage::shader_resource | resource_usage::copy_source),
                             nullptr, resource_usage::render_target, &g_ui)) {
        dev->set_resource_name(g_ui, "MGS4DLSS UI layer"); g_uiState = resource_usage::render_target;
        if (!dev->create_resource_view(g_ui, resource_usage::render_target, resource_view_desc(fmt), &g_uiRtv)) { logmsg("UI layer RTV failed"); dev->destroy_resource(g_ui); g_ui = { 0 }; }
        else { const float zero[4] = { 0, 0, 0, 0 }; cmd->clear_render_target_view(g_uiRtv, zero); }
    } else logmsg("UI layer texture failed");
    if (dev->create_resource(resource_desc(w, h, 1, 1, fmt, 1, memory_heap::default_, resource_usage::copy_dest | resource_usage::copy_source | resource_usage::shader_resource), nullptr, resource_usage::copy_dest, &g_preHud)) { dev->set_resource_name(g_preHud, "MGS4DLSS pre-HUD capture"); g_preHudState = resource_usage::copy_dest; }
    else logmsg("pre-HUD capture texture failed");
    if (dev->create_resource(resource_desc(outW, outH, 1, 1, fmt, 1, memory_heap::default_, resource_usage::copy_dest | resource_usage::shader_resource), nullptr, resource_usage::copy_dest, &g_keep)) { dev->set_resource_name(g_keep, "MGS4DLSS frozen frame"); g_keepState = resource_usage::copy_dest; g_keepValid = false; }
    else logmsg("frozen-frame texture failed (the frozen background will be the game's 1080p seed)");
    if (dev->create_resource(resource_desc(w, h, 1, 1, fmt, 1, memory_heap::default_, resource_usage::copy_dest | resource_usage::copy_source | resource_usage::shader_resource | resource_usage::unordered_access), nullptr, resource_usage::copy_dest, &g_hudless)) { dev->set_resource_name(g_hudless, "MGS4DLSS HUD-less color"); g_hudlessState = resource_usage::copy_dest; }
    else logmsg("HUD-less texture failed");
    if (dev->create_resource(resource_desc(outW, outH, 1, 1, fmt, 1, memory_heap::default_, resource_usage::copy_source | resource_usage::shader_resource | resource_usage::unordered_access), nullptr, resource_usage::unordered_access, &g_scratch)) { dev->set_resource_name(g_scratch, "MGS4DLSS DRS resample"); g_scratchState = resource_usage::unordered_access; }
    else logmsg("DRS scratch texture failed");

    if (dev->create_resource_view(g_mv, resource_usage::render_target, resource_view_desc(format::r16g16_float), &g_mvRtv)) {
        const float zero[4] = { 0, 0, 0, 0 };
        cmd->clear_render_target_view(g_mvRtv, zero);
    }
    cmd->barrier(g_mv, resource_usage::render_target, resource_usage::shader_resource_non_pixel); g_mvState = resource_usage::shader_resource_non_pixel;
    g_dlssW = w; g_dlssH = h; g_dlssOutW = outW; g_dlssOutH = outH; g_dlssFmt = fmt;
    return create_feature(cmd, w, h, outW, outH, fmt);
}

static void restore_state(device* dev, command_list* cmd, const cl_state& s)
{
    ID3D12GraphicsCommandList* native = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd->get_native());
    ID3D12DescriptorHeap* heaps[2] = {}; UINT nheaps = 0;
    for (int i : { 1, 0, 4 }) {
        if (!s.table_set[i]) continue;
        descriptor_heap heap = { 0 }; uint32_t off = 0;
        dev->get_descriptor_heap_offset(s.tables[i], 0, 0, &heap, &off);
        ID3D12DescriptorHeap* h = reinterpret_cast<ID3D12DescriptorHeap*>(heap.handle);
        if (!h) continue;
        bool dup = false; for (UINT k = 0; k < nheaps; ++k) dup |= heaps[k] == h;
        if (!dup && nheaps < 2) heaps[nheaps++] = h;
    }
    if (nheaps) native->SetDescriptorHeaps(nheaps, heaps);
    if (s.root_sig) native->SetGraphicsRootSignature(s.root_sig);
    if (s.pso) native->SetPipelineState(s.pso);
    for (int i = 0; i < 5; ++i) {
        if (s.table_set[i]) native->SetGraphicsRootDescriptorTable(i, D3D12_GPU_DESCRIPTOR_HANDLE{ s.tables[i].handle });
        if (s.cbv_set[i])   native->SetGraphicsRootConstantBufferView(i, s.cbv[i]);
    }
    if (s.rtv_count || s.dsv.handle) {
        D3D12_CPU_DESCRIPTOR_HANDLE h[8] = {}; for (uint32_t i = 0; i < s.rtv_count; ++i) h[i].ptr = (SIZE_T)s.rtvs[i].handle;
        D3D12_CPU_DESCRIPTOR_HANDLE dsvh = { (SIZE_T)s.dsv.handle };
        native->OMSetRenderTargets(s.rtv_count, s.rtv_count ? h : nullptr, FALSE, s.dsv.handle ? &dsvh : nullptr);
    }
    const float k = (s.rt_scaled && g_internalW) ? float(g_renderW) / float(g_internalW) : 1.0f, ky = (s.rt_scaled && g_internalH) ? float(g_renderH) / float(g_internalH) : 1.0f;
    if (s.vp_valid) { D3D12_VIEWPORT v = { s.vp.x * k, s.vp.y * ky, s.vp.width * k, s.vp.height * ky, s.vp.min_depth, s.vp.max_depth }; native->RSSetViewports(1, &v); }
    if (s.sc_valid) { D3D12_RECT r = { (LONG)(s.sc.left * k), (LONG)(s.sc.top * ky), (LONG)(s.sc.right * k), (LONG)(s.sc.bottom * ky) }; native->RSSetScissorRects(1, &r); }
    if (s.topology) native->IASetPrimitiveTopology(static_cast<D3D12_PRIMITIVE_TOPOLOGY>(s.topology));
}

static resource pick_depth(device* dev, resource color)
{
    auto itDs = g_dsForRt.find(color.handle);
    resource depth = (itDs != g_dsForRt.end() && is_live(itDs->second)) ? resource{ itDs->second } : resource{ 0 };
    if (!depth.handle) {
        uint32_t best = 0;
        for (auto& kv : g_drawsPerDs) if (kv.second > best && is_live(kv.first)) { best = kv.second; depth = resource{ kv.first }; }
    }
    return depth;
}

static void remember_internal_res(uint32_t w, uint32_t h);

// Runs DLSS on `color` (state as bgfx tracks it) and either copies the result back over it (DLAA) or rewrites the SRV
// descriptor at `srvCpu` so the composite draw samples the full-size output (upscaling modes).
static void run_dlss(command_list* cmd, const cl_state* restore, resource color, resource_usage colorState, uint64_t srvCpu)
{
    device* dev = cmd->get_device();
    if (!ngx_init(dev)) return;
    if (!is_live(color.handle)) { static int n = 0; if (n++ < 3) logmsg("skipped: color %p is not a live texture", (void*)color.handle); return; }
    resource_desc cd = dev->get_resource_desc(color);
    resource depth = pick_depth(dev, color);
    if (!depth.handle) { static bool once = false; if (!once) { once = true; logmsg("no depth buffer found for color %p", (void*)color.handle); } return; }
    resource_desc dd = dev->get_resource_desc(depth);
    if (dd.texture.width != cd.texture.width || dd.texture.height != cd.texture.height) {
        static bool once = false; if (!once) { once = true; logmsg("depth %ux%u does not match color %ux%u; skipping", dd.texture.width, dd.texture.height, cd.texture.width, cd.texture.height); }
        return;
    }
    // game's real render size (changed in-game?) - recorded only for an input the depth buffer vouches for,
    // so a stray small texture picked up by the composite scan cannot write a wrong InternalRes into the ini
    if (!g_scaling || !is_scaled(color)) remember_internal_res(cd.texture.width, cd.texture.height);
    if (g_cfgDebugMode == 2) return;

    const bool upscale = g_scaling && cd.texture.width == g_renderW && cd.texture.height == g_renderH && srvCpu != 0;
    const uint32_t outW = upscale ? g_internalW : cd.texture.width, outH = upscale ? g_internalH : cd.texture.height;
    if (!ensure_resources(dev, cmd, cd.texture.width, cd.texture.height, outW, outH, cd.texture.format)) return;
    if (g_featureCreatedThisFrame) { if (restore) restore_state(dev, cmd, *restore); return; }

    static bool loggedOnce = false;
    if (!loggedOnce) {
        static const NVSDK_NGX_PerfQuality_Value modes[] = { NVSDK_NGX_PerfQuality_Value_DLAA, NVSDK_NGX_PerfQuality_Value_MaxQuality, NVSDK_NGX_PerfQuality_Value_Balanced, NVSDK_NGX_PerfQuality_Value_MaxPerf, NVSDK_NGX_PerfQuality_Value_UltraPerformance };
        static const char* names[] = { "DLAA", "Quality", "Balanced", "Performance", "UltraPerformance" };
        for (int i = 0; i < 5 && g_ngxCaps; ++i) {
            unsigned ow = 0, oh = 0, xw = 0, xh = 0, nw = 0, nh = 0; float sh = 0;
            NVSDK_NGX_Result rr = NGX_DLSS_GET_OPTIMAL_SETTINGS(g_ngxCaps, outW, outH, modes[i], &ow, &oh, &xw, &xh, &nw, &nh, &sh);
            logmsg("DRS ranges for %ux%u target, %s: optimal %ux%u, dynamic min %ux%u max %ux%u (%s)", outW, outH, names[i], ow, oh, nw, nh, xw, xh, ngx_str(rr));
        }
    }
    if (!loggedOnce) { loggedOnce = true; logmsg("injecting: color=%p %ux%u fmt=%u depth=%p fmt=%u -> output %ux%u (%s) frame=%u", (void*)color.handle, cd.texture.width, cd.texture.height, (unsigned)cd.texture.format, (void*)depth.handle, (unsigned)dd.texture.format, outW, outH, upscale ? "SRV redirect" : "copy back", g_frame); }

    {
        const resource res[3] = { color, depth, g_out };
        const resource_usage from[3] = { colorState, resource_usage::depth_stencil_write, g_outState };
        const resource_usage to[3] = { resource_usage::shader_resource_non_pixel, resource_usage::shader_resource_non_pixel, resource_usage::unordered_access };
        cmd->barrier(3, res, from, to); g_outState = resource_usage::unordered_access;
    }
    // Dynamic resolution: the scene occupies a sub-rect of the texture; DLSS evaluates that sub-rect.
    uint32_t subW = cd.texture.width, subH = cd.texture.height; bool drsActive = false;
    // Layout window (mission briefings): the rectangle the scene occupies in the final image, in the texture's pixels;
    // the whole texture otherwise. The scene viewport divided by it is the port's dynamic-resolution scale.
    viewport lay = { 0.0f, 0.0f, float(cd.texture.width), float(cd.texture.height), 0.0f, 1.0f }; bool layoutWin = false;
    float depthOrigin[2] = { 0.0f, 0.0f };   // full-grid pixel p samples the scene depth at p * k + depthOrigin
    {
        const viewport* lw = layout_window();
        if (lw && lw->width > 0 && lw->height > 0 && g_internalW && g_internalH) {
            const float tx = float(cd.texture.width) / float(g_internalW), ty = float(cd.texture.height) / float(g_internalH);
            lay = viewport{ lw->x * tx, lw->y * ty, lw->width * tx, lw->height * ty, 0.0f, 1.0f }; layoutWin = true;
        }
        const viewport& svp = g_sceneVpFrameValid ? g_sceneVpFrame : g_sceneVp;
        if (g_cfgDRS && (g_sceneVpFrameValid || g_sceneVpValid) && g_internalW && svp.width > 0) {
            const float layW = lw ? lw->width : float(g_internalW), layH = lw ? lw->height : float(g_internalH);
            float fx = svp.width / layW, fy = svp.height / layH;
            if (fx > 1.0f) fx = 1.0f; if (fy > 1.0f) fy = 1.0f; if (fx < 0.2f) fx = 0.2f; if (fy < 0.2f) fy = 0.2f;
            if (fx < 0.995f || fy < 0.995f) {
                subW = (uint32_t)(cd.texture.width * fx + 0.5f); subH = (uint32_t)(cd.texture.height * fy + 0.5f);
                if (g_cfgDRS == 2) { if (g_drsMinW && subW < g_drsMinW) subW = g_drsMinW; if (g_drsMinH && subH < g_drsMinH) subH = g_drsMinH; }
                if (subW > cd.texture.width) subW = cd.texture.width; if (subH > cd.texture.height) subH = cd.texture.height;
                drsActive = subW < cd.texture.width || subH < cd.texture.height;
                // the scaled scene keeps the window's position and scales its size (observed on the briefing's camera windows)
                if (lw) { const float lx = lw->x * float(cd.texture.width) / float(g_internalW), ly = lw->y * float(cd.texture.height) / float(g_internalH); depthOrigin[0] = svp.x * float(cd.texture.width) / float(g_internalW) - lx * fx; depthOrigin[1] = svp.y * float(cd.texture.height) / float(g_internalH) - ly * fy; }
                if (drsActive) { g_drsFrames++; static bool once = false; if (!once) { once = true; logmsg("DRS: scene viewport %.0fx%.0f of %ux%u -> DLSS sub-rect %ux%u (min %ux%u)", svp.width, svp.height, cd.texture.width, cd.texture.height, subW, subH, g_drsMinW, g_drsMinH); } }
            }
        }
        if (layoutWin) g_layoutFrames++;
        { static bool was = false; static float lw2 = 0, lh2 = 0; if (layoutWin != was || (layoutWin && (fabsf(lay.width - lw2) > 1.0f || fabsf(lay.height - lh2) > 1.0f))) { was = layoutWin; lw2 = lay.width; lh2 = lay.height; static uint32_t n = 0; if (n++ < 100) logmsg("LAYOUT f%u: 3D scene in a window %s(%.0f,%.0f %.0fx%.0f) of the %ux%u image; scene viewport %.0fx%.0f -> scale %.3f (the upscale pass says %.3f)", g_frame, layoutWin ? "" : "OFF ", lay.x, lay.y, lay.width, lay.height, cd.texture.width, cd.texture.height, svp.width, svp.height, drsActive ? subW / float(cd.texture.width) : 1.0f, g_layoutK); if (g_layoutDumps++ < 40) g_layoutDumpFrames = 3; } }
    }
    g_drsActiveLast = drsActive; g_drsSubW = subW; g_drsSubH = subH;
    // The port renders the 3D scene into the top-left sub-rect and its post chain upscales it to the full final image
    // before the composite (the composite samples the whole texture). DLSS therefore sees a full-size image, and depth
    // and motion vectors must be on that full grid: the depth is stretched into g_depthFull, the camera vectors are
    // computed per full-grid pixel from the sub-res depth, object vectors are rasterized with the full viewport.
    // DRS=2 (legacy) instead evaluates DLSS on the sub-rect and resamples the output back into it.
    const bool fullGrid = g_cfgDRS != 2;
    // Windowed scene (Codec caller / pause-menu model): no full-frame 3D viewport this frame, but a window one. The
    // 3D image sits 1:1 in that rectangle of the frozen screen; vectors are computed relative to it and are zero outside.
    g_windowMode = fullGrid && !g_sceneVpFrameValid && g_winVpFrameValid && !drsActive;
    g_windowVp = g_windowMode ? g_winVpFrame : viewport{ 0, 0, float(cd.texture.width), float(cd.texture.height), 0, 1 };
    if (tracing()) logmsg("f%u DLSS: fullVp valid %d (%.0fx%.0f) winVp valid %d (%.0f,%.0f %.0fx%.0f) window mode %d drs %d", g_frame, (int)g_sceneVpFrameValid, g_sceneVpFrame.width, g_sceneVpFrame.height, (int)g_winVpFrameValid, g_winVpFrame.x, g_winVpFrame.y, g_winVpFrame.width, g_winVpFrame.height, (int)g_windowMode, (int)drsActive);
    { static bool wasWin = false; if (g_windowMode != wasWin) { wasWin = g_windowMode; logmsg("scene window: %s (%.0f,%.0f %.0fx%.0f)", g_windowMode ? "on" : "off", g_windowVp.x, g_windowVp.y, g_windowVp.width, g_windowVp.height);
        // what the frame's depth-tested draws looked like at the flip: the scene-viewport candidates, the window ones, the 3D target
        static uint32_t nd = 0; if (nd++ < 60) { std::string t; char b[96]; const viewport* lw = layout_now();
            for (auto& kv : g_vpHist) { snprintf(b, sizeof b, " scene(%.0f,%.0f %.0fx%.0f)x%u", kv.second.second.x, kv.second.second.y, kv.second.second.width, kv.second.second.height, kv.second.first); t += b; }
            for (auto& kv : g_winVpHist) { snprintf(b, sizeof b, " win(%.0f,%.0f %.0fx%.0f)x%u", kv.second.second.x, kv.second.second.y, kv.second.second.width, kv.second.second.height, kv.second.first); t += b; }
            logmsg("   f%u candidates:%s; 3D target %p (%u draws, last peak %u), layout %s(%.0f,%.0f %.0fx%.0f)", g_frame, t.c_str(), (void*)g_curGeoRt, g_winGeoDraws, g_geoDrawsLast, lw ? "" : "none ", lw ? lw->x : 0.0f, lw ? lw->y : 0.0f, lw ? lw->width : 0.0f, lw ? lw->height : 0.0f); } } }
    const uint32_t mvW = fullGrid ? cd.texture.width : subW, mvH = fullGrid ? cd.texture.height : subH;
    const float kx = (fullGrid && drsActive) ? subW / float(cd.texture.width) : 1.0f, ky = (fullGrid && drsActive) ? subH / float(cd.texture.height) : 1.0f;
    resource dlssDepth = depth; bool depthStretched = false;
    // The camera window's depth was copied into the full-grid depth at the window's first reader (g_winDepthRect): the
    // main view's stretch keeps that region, and runs even at full scale so the velocity pass and DLSS read the copy.
    const bool winDepth = g_winDepthFrame == g_frame && !g_windowMode;
    if (fullGrid && (drsActive || winDepth) && g_depthFull.handle && g_stretchPso) {
        const float layRect[4] = { lay.x, lay.y, lay.width, lay.height };
        depth_stretch_dispatch(cmd, depth, dd.texture.format, cd.texture.width, cd.texture.height, kx, ky, depthOrigin, layRect, true, winDepth ? g_winDepthRect : nullptr);
        dlssDepth = g_depthFull; depthStretched = true;
    }
    // Camera-only motion vectors from this frame's depth (VP = majority block of this frame's scene draws).
    select_frame_vp();
    select_window_vp();
    // Frozen screen: no 3D scene and the final texture holds a recycled image (the seed blit, a copy of the other final
    // texture, or no rewrite at all) that DLSS + NR already produced on a live frame. Pass it through untouched.
    // The world is frozen from the seed capture until the final texture receives a fresh scene write (the geometry
    // target's upscale, a video frame). A frozen frame is never judged by its depth-tested draws: the pause menu's panels
    // are depth-tested quads and its Snake model brings a camera matrix, yet the background is the recycled seed.
    // Without the seed state (e.g. a blocking dialog that stops the world some other way) a frame with no camera / no
    // depth-tested draws and no fresh write is treated the same.
    // Live-scene safety net (scenes that never write the final texture from a geometry target): many depth-tested draws
    // with a camera INTO A GEOMETRY TARGET. The pause menu's panels are hundreds of depth-tested quads too, but they go
    // into the final texture, and clearing the frozen state on them reset the history one frame before the unpause.
    const bool geoIsFinal = !g_curGeoRt || g_curGeoRt == g_finalRt[0] || g_curGeoRt == g_finalRt[1] || g_curGeoRt == color.handle;
    if (g_frozen && (g_freshWrite || (!g_windowMode && g_haveFrameVP && g_depthOnDrawsThisFrame >= 400 && !geoIsFinal))) {
        g_frozen = false;
        static uint32_t nlog = 0; if (nlog++ < 40) logmsg("frozen state cleared at frame %u: %s (scene write from %s, geo target %p, finals %p/%p, color %p, depth-tested %u, window %d)", g_frame, g_freshWrite ? "fresh scene write" : "live scene into a geometry target", desc_str(dev, resource{ g_finalSceneSrc }).c_str(), (void*)g_curGeoRt, (void*)g_finalRt[0], (void*)g_finalRt[1], (void*)color.handle, g_depthOnDrawsThisFrame, (int)g_windowMode);
    }
    // Pass-through only from the seed state, and only from the second consecutive frame without a fresh scene write:
    // a single frame whose camera matrix or scene write was missed inside a live cutscene must never be shown raw
    // (that was a one-frame flicker plus a history reset, dozens of times per scene).
    static uint32_t s_noFreshRun = 0;
    if (g_freshWrite) s_noFreshRun = 0; else s_noFreshRun++;
    const bool frozenPass = g_cfgFrozenBg && !g_windowMode && !upscale && g_cfgDRS != 2 && !g_freshWrite && g_frozen && s_noFreshRun >= 2;
    { static bool was = false; if (frozenPass != was) { was = frozenPass; logmsg("frozen screen pass-through %s at frame %u (frozen %d, fresh write %d, scene write %d from %p, camera %d, depth-tested draws %u, window %d)", frozenPass ? "ON" : "off", g_frame, (int)g_frozen, (int)g_freshWrite, (int)g_finalSceneWritten, (void*)g_finalSceneSrc, (int)g_haveFrameVP, g_depthOnDrawsThisFrame, (int)g_windowMode); } }
    const bool discont = g_forceReset || (g_lastEvalWindow >= 0 && g_lastEvalWindow != (int)g_windowMode);
    bool resumeKept = false;
    if (discont && !frozenPass && !g_windowMode && g_haveLiveVP && g_haveFrameVP && camera_close(g_frameVP, g_liveVP)) {
        resumeKept = true; memcpy(g_prevVP, g_liveVP, 64); g_havePrevVP = true;   // motion relative to the last live frame, not to the pause menu's model camera
        g_resumesKept++; if (g_resumesKept <= 50) logmsg("RESUME f%u: same camera as before the freeze -> DLSS history kept%s", g_frame, g_lastWinRectValid ? " (the 3D window's rectangle excluded for this frame)" : "");
    }
    // the rectangle the camera's NDC maps to: the 3D window (Codec caller), the layout window (briefings) or the frame
    const viewport& mvVp = g_windowMode ? g_windowVp : lay;
    const float mvRect[4] = { mvVp.x, mvVp.y, mvVp.width, mvVp.height };
    const int mvReset = mv_dispatch(cmd, depth, dd.texture.format, mvW, mvH, kx, ky, depthOrigin, mvRect);

    ID3D12GraphicsCommandList* native = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd->get_native());
    // Per-object motion: rasterize the stream-out captures over the camera vectors (depth-tested against the scene depth).
    if (g_cfgObjectMV && objmv::has_captures() && g_mvRtv.handle) {
        auto itv = g_dsvForDs.find(depth.handle);
        const bool haveDsv = itv != g_dsvForDs.end() && itv->second.handle;
        if (haveDsv || depthStretched) {
            if (!depthStretched) cmd->barrier(depth, resource_usage::shader_resource_non_pixel, resource_usage::depth_stencil_write);
            cmd->barrier(g_mv, g_mvState, resource_usage::render_target); g_mvState = resource_usage::render_target;
            const float k = (g_scaling && g_internalW) ? float(g_renderW) / float(g_internalW) : 1.0f, ky2 = (g_scaling && g_internalH) ? float(g_renderH) / float(g_internalH) : 1.0f;
            const viewport& sv = scene_vp_now();
            // full grid: the captured clip positions are viewport-independent, so the full viewport puts each object where
            // the (upscaled) image shows it; legacy sub-rect mode rasterizes into the scene viewport
            D3D12_VIEWPORT svp = (!fullGrid && scene_vp_now_valid()) ? D3D12_VIEWPORT{ sv.x * k, sv.y * ky2, sv.width * k, sv.height * ky2, sv.min_depth, sv.max_depth }
                               : g_windowMode ? D3D12_VIEWPORT{ g_windowVp.x, g_windowVp.y, g_windowVp.width, g_windowVp.height, 0, 1 }
                               : D3D12_VIEWPORT{ lay.x, lay.y, lay.width, lay.height, 0, 1 };   // the layout window (the whole image normally)
            float jitCur[2]; jitter_ndc(jitCur);
            const float prevSz[2] = { svp.Width, svp.Height };
            // the video call: the caller feed's vectors in its rectangle, projected onto the screen through the monitor
            const viewport* flv = feed_layout_now(); float feedRect[4] = {}; bool feedOn = false;
            if (flv && g_feedMv.handle && g_feedMvRtv.handle && g_internalW && g_internalH) {
                const float tx = float(cd.texture.width) / float(g_internalW), ty = float(cd.texture.height) / float(g_internalH);
                feedRect[0] = flv->x * tx; feedRect[1] = flv->y * ty; feedRect[2] = flv->width * tx; feedRect[3] = flv->height * ty; feedOn = true;
                if (g_feedMvState != resource_usage::render_target) { cmd->barrier(g_feedMv, g_feedMvState, resource_usage::render_target); g_feedMvState = resource_usage::render_target; }
                const float zero[4] = { 0, 0, 0, 0 }; cmd->clear_render_target_view(g_feedMvRtv, zero);
                const bool feedDepthOk = g_feedDepthFrame == g_frame && g_feedDepth.handle;
                if (feedDepthOk && g_feedDepthState != resource_usage::shader_resource) { cmd->barrier(g_feedDepth, g_feedDepthState, resource_usage::shader_resource); g_feedDepthState = resource_usage::shader_resource; }
                g_feedFrames++; if (objmv::stats().monitorCaptured) g_monitorFrames++;
            }
            objmv::velocity(native, D3D12_CPU_DESCRIPTOR_HANDLE{ (SIZE_T)g_mvRtv.handle }, haveDsv ? D3D12_CPU_DESCRIPTOR_HANDLE{ (SIZE_T)itv->second.handle } : D3D12_CPU_DESCRIPTOR_HANDLE{ 0 }, cd.texture.width, cd.texture.height, svp, jitCur, g_prevJitNdc, prevSz,
                            depthStretched ? reinterpret_cast<ID3D12Resource*>(g_depthFull.handle) : nullptr,
                            (feedOn && g_feedDepthFrame == g_frame) ? reinterpret_cast<ID3D12Resource*>(g_feedDepth.handle) : nullptr,
                            feedOn ? D3D12_CPU_DESCRIPTOR_HANDLE{ (SIZE_T)g_feedMvRtv.handle } : D3D12_CPU_DESCRIPTOR_HANDLE{ 0 },
                            feedOn ? reinterpret_cast<ID3D12Resource*>(g_feedMv.handle) : nullptr, feedOn ? feedRect : nullptr, g_cfgMonitorFlipV != 0, g_winCut);
            cmd->barrier(g_mv, resource_usage::render_target, resource_usage::shader_resource_non_pixel); g_mvState = resource_usage::shader_resource_non_pixel;
            if (!depthStretched) cmd->barrier(depth, resource_usage::depth_stencil_write, resource_usage::shader_resource_non_pixel);
            g_objMvFrames++;
        }
    }
    // HUD: bias-current-color mask + zero vectors from the replayed UI layer (the HUD is inside the image DLSS sees)
    bool uiMasked = g_cfgUiMask && !g_cfgPrePost && !upscale && g_cfgDRS != 2 && g_ui.handle && g_uiDrawsThisFrame > 0 && g_uimaskPso;
    // No depth-tested draw at all this frame (Codec call: the face is rendered without depth into an HDR buffer over the
    // previous frame's image): nothing to reconstruct temporally, so every pixel is bias-current-color with zero motion -
    // no ghosting or smearing anywhere, while the NR add-on still processes the frame.
    const bool no3d = g_depthOnDrawsThisFrame == 0 && !upscale && g_cfgDRS != 2 && g_ui.handle && g_mask.handle && g_uimaskPso;
    { static bool was = false; if (no3d != was) { was = no3d; logmsg("no 3D scene this frame (%s): %s", no3d ? "e.g. Codec" : "3D scene back", no3d ? "whole frame bias-current-color, zero motion" : "normal reconstruction"); } }
    const bool flashUp = g_flashFrame == g_frame && g_ui.handle && g_mask.handle && g_uimaskPso && !upscale && g_cfgDRS != 2;
    if (flashUp) { g_flashFrames++; uiMasked = true; }
    if (no3d || flashUp) { uimask_dispatch(cmd, cd.texture.width, cd.texture.height, true); uiMasked = true; }
    else if (uiMasked) uimask_dispatch(cmd, cd.texture.width, cd.texture.height, false);
    if (resumeKept && g_lastWinRectValid && g_mask.handle && g_mvHeap && g_lastWinRect.width > 0) {
        // the pause-menu model was evaluated in this rectangle: its history is not the world's - current color there
        const UINT inc = g_d3d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_mvHeap->GetCPUDescriptorHandleForHeapStart(); cpu.ptr += SIZE_T(23) * 4 * inc;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_mvHeap->GetGPUDescriptorHandleForHeapStart(); gpu.ptr += UINT64(23) * 4 * inc;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {}; uav.Format = DXGI_FORMAT_R8_UNORM; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        g_d3d->CreateUnorderedAccessView(reinterpret_cast<ID3D12Resource*>(g_mask.handle), nullptr, &uav, cpu);
        cmd->barrier(g_mask, g_maskState, resource_usage::unordered_access); g_maskState = resource_usage::unordered_access;
        ID3D12GraphicsCommandList* nat = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd->get_native());
        ID3D12DescriptorHeap* heaps[1] = { g_mvHeap }; nat->SetDescriptorHeaps(1, heaps);
        const float one[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        const D3D12_RECT rc = { (LONG)g_lastWinRect.x, (LONG)g_lastWinRect.y, (LONG)(g_lastWinRect.x + g_lastWinRect.width + 0.5f), (LONG)(g_lastWinRect.y + g_lastWinRect.height + 0.5f) };
        nat->ClearUnorderedAccessViewFloat(gpu, cpu, reinterpret_cast<ID3D12Resource*>(g_mask.handle), one, 1, &rc);
        cmd->barrier(g_mask, resource_usage::unordered_access, resource_usage::shader_resource_non_pixel); g_maskState = resource_usage::shader_resource_non_pixel;
        uiMasked = true;
    }
    NVSDK_NGX_D3D12_DLSS_Eval_Params ep = {};
    ep.Feature.pInColor = reinterpret_cast<ID3D12Resource*>(color.handle);
    ep.Feature.pInOutput = reinterpret_cast<ID3D12Resource*>(g_out.handle);
    ep.Feature.InSharpness = g_cfgSharpness100 / 100.0f;
    ep.pInDepth = reinterpret_cast<ID3D12Resource*>(dlssDepth.handle);
    ep.pInMotionVectors = reinterpret_cast<ID3D12Resource*>(g_mv.handle);
    ep.pInBiasCurrentColorMask = ((g_cfgDynMask || uiMasked) && g_mask.handle) ? reinterpret_cast<ID3D12Resource*>(g_mask.handle) : nullptr;
    g_lastDepth = depth.handle;
    ep.InJitterOffsetX = g_cfgJitter ? g_jitterX : 0.0f; ep.InJitterOffsetY = g_cfgJitter ? g_jitterY : 0.0f;
    // DRS=1: DLSS evaluates the whole texture (the sub-rect content at its native scale, garbage outside it that the
    // composite never samples), so NGX post-processing add-ons see a full-size contract; DRS=2 = sub-rect evaluation.
    ep.InRenderSubrectDimensions = (g_cfgDRS == 2) ? NVSDK_NGX_Dimensions{ subW, subH } : NVSDK_NGX_Dimensions{ cd.texture.width, cd.texture.height };
    if (discont && !frozenPass && !resumeKept) { g_discontResets++; if (g_discontResets <= 50) logmsg("RESET f%u: %s -> DLSS history cleared", g_frame, g_forceReset ? "first evaluation after frozen pass-through frames" : "insertion moved between the final texture and a 3D window"); }
    if (frozenPass) g_forceReset = true; else { g_forceReset = false; g_lastEvalWindow = (int)g_windowMode; }
    if (discont && !frozenPass && !resumeKept) g_fgCutFrames = 8;
    ep.InReset = (g_frame <= g_createdFrame + 1 || (mvReset && g_cfgMotionVectors) || (discont && !frozenPass && !resumeKept)) ? 1 : 0;
    if (!frozenPass) { if (g_windowMode) { g_lastWinRect = g_windowVp; g_lastWinRectValid = true; } else if (g_haveFrameVP) { memcpy(g_liveVP, g_frameVP, 64); g_haveLiveVP = true; if (!resumeKept) g_lastWinRectValid = false; } }
    ep.InMVScaleX = 1.0f; ep.InMVScaleY = 1.0f;
    ep.InPreExposure = 1.0f; ep.InExposureScale = 1.0f;
    // The frame-time hint lets the model relate vector magnitudes to speed (the guide asks for it; 0 = unknown). Game
    // frames only: with frame generation the rollover runs on the game's present, not on the generated ones.
    ep.InFrameTimeDeltaInMsec = g_lastFrameDeltaMs;
    NVSDK_NGX_Result r = NVSDK_NGX_Result_Success;
    if (g_cfgProbe && !upscale) probe_dispatch(cmd, color, resource_usage::shader_resource_non_pixel, 0);
    const uint32_t visInW = (g_cfgDRS == 2) ? subW : outW, visInH = (g_cfgDRS == 2) ? subH : outH;   // full grid: the MV texture and the output map 1:1
    if (g_cfgDebugMode == 5) {
        vis_dispatch(cmd, visInW, visInH, outW, outH);   // show the MV field instead of the DLSS result
    } else if (g_cfgDebugMode == 7 && g_hudless.handle && g_preHudCaptured && !upscale) {
        // show the HUD-less color DLSS-G would get: build it here (same steps as the tagging path), then copy it over the output
        r = NGX_D3D12_EVALUATE_DLSS_EXT(native, g_dlss, g_ngxParams, &ep);
        if (!NVSDK_NGX_FAILED(r)) {
            cmd->barrier(g_out, resource_usage::unordered_access, resource_usage::copy_source);
            if (g_hudlessState != resource_usage::copy_dest) { cmd->barrier(g_hudless, g_hudlessState, resource_usage::copy_dest); g_hudlessState = resource_usage::copy_dest; }
            cmd->copy_resource(g_out, g_hudless);
            cmd->barrier(g_out, resource_usage::copy_source, resource_usage::copy_dest);
            cmd->barrier(g_hudless, resource_usage::copy_dest, resource_usage::unordered_access); g_hudlessState = resource_usage::unordered_access;
            if (g_preHudState != resource_usage::shader_resource_non_pixel) { cmd->barrier(g_preHud, g_preHudState, resource_usage::shader_resource_non_pixel); g_preHudState = resource_usage::shader_resource_non_pixel; }
            if (g_uiState != resource_usage::shader_resource_non_pixel) { cmd->barrier(g_ui, g_uiState, resource_usage::shader_resource_non_pixel); g_uiState = resource_usage::shader_resource_non_pixel; }
            hudless_dispatch(cmd, cd.texture.width, cd.texture.height, static_cast<DXGI_FORMAT>(cd.texture.format));
            cmd->barrier(g_hudless, resource_usage::unordered_access, resource_usage::copy_source); g_hudlessState = resource_usage::copy_source;
            cmd->copy_resource(g_hudless, g_out);
            cmd->barrier(g_out, resource_usage::copy_dest, resource_usage::unordered_access);
        }
    } else if (g_cfgDebugMode == 6 && g_ui.handle && !upscale) {
        // show the replayed UI layer instead of the DLSS result (what DLSS-G gets as UI color + alpha)
        cmd->barrier(g_out, resource_usage::unordered_access, resource_usage::copy_dest);
        if (g_uiState != resource_usage::copy_source) { cmd->barrier(g_ui, g_uiState, resource_usage::copy_source); g_uiState = resource_usage::copy_source; }
        cmd->copy_resource(g_ui, g_out);
        cmd->barrier(g_out, resource_usage::copy_dest, resource_usage::unordered_access);
    } else if (g_cfgDebugMode == 9) {
        // DLSS result with the motion-vector field blended over it: the vector silhouette of a character must sit exactly
        // on the rendered character (any offset = the vectors are on a different grid than the image)
        r = NGX_D3D12_EVALUATE_DLSS_EXT(native, g_dlss, g_ngxParams, &ep);
        if (!NVSDK_NGX_FAILED(r)) vis_dispatch(cmd, visInW, visInH, outW, outH, 0.5f);
    } else if (frozenPass) {
        cmd->barrier(color, resource_usage::shader_resource_non_pixel, resource_usage::copy_source);
        cmd->barrier(g_out, resource_usage::unordered_access, resource_usage::copy_dest);
        cmd->copy_resource(color, g_out);
        cmd->barrier(g_out, resource_usage::copy_dest, resource_usage::unordered_access);
        cmd->barrier(color, resource_usage::copy_source, resource_usage::shader_resource_non_pixel);
        g_frozenPassFrames++;
    } else if (g_cfgPostDof && g_cfgDofStepFreeze && g_frameKStep && g_evalCount > 2 && !g_windowMode) {
        // a resolution-step frame: the game's own chain disagrees about the scale and its final image can carry a
        // sub-rect layout; hold the previous, consistent DLSS output for this one frame instead of evaluating it
        cmd->barrier(color, resource_usage::shader_resource_non_pixel, resource_usage::copy_dest);
        cmd->barrier(g_out, resource_usage::unordered_access, resource_usage::copy_source);
        cmd->copy_resource(g_out, color);
        cmd->barrier(g_out, resource_usage::copy_source, resource_usage::unordered_access);
        cmd->barrier(color, resource_usage::copy_dest, resource_usage::shader_resource_non_pixel);
        g_stepFreezes++;
        static uint32_t nf = 0; if (nf++ < 20) logmsg("step-frame hold f%u: previous DLSS output shown (resolution step)", g_frame);
    } else {
        r = NGX_D3D12_EVALUATE_DLSS_EXT(native, g_dlss, g_ngxParams, &ep);
        if (NVSDK_NGX_FAILED(r)) { if (g_failCount++ < 5) logmsg("NGX EvaluateFeature -> %s", ngx_str(r)); }
        else {
            // One shot, the moment the first real scene frame is resolved. Tools that press through the boot
            // prompts need to stop the instant the scene starts: SCENE-STATE is half a second behind by design
            // (it wants 30 frames of the same reading), and half a second of pressing lands inside the scene and
            // can skip the cutscene. "NGX EvaluateFeature ok (#1)" cannot serve - pre-warm has already spent
            // evaluations 1..12 on no-3D frames, so the counter is never 1 here.
            static bool firstReal = false;
            if (!firstReal) { firstReal = true; logmsg("FIRST-3D-FRAME (frame %u)", g_frame); }
            if (++g_evalCount == 1 || (g_cfgLogEveryN && g_evalCount % g_cfgLogEveryN == 0)) logmsg("NGX EvaluateFeature ok (#%u)", g_evalCount);
        }
    }

    if (g_cfgPostDof && g_dofSeenThisFrame && g_dofCbValid && g_dofCbGValid && !upscale && !frozenPass && !g_windowMode && !NVSDK_NGX_FAILED(r)
        && g_cfgDebugMode != 5 && g_cfgDebugMode != 6 && g_cfgDebugMode != 7 && g_cfgDebugMode != 9 && g_cfgDRS != 2) {
        // g_out is always the DLSS output size; the color texture must match it (a smaller texture - e.g. a 1920x1080 seed -
        // would squeeze the blur layer into the top-left quadrant of the output)
        if (outW != g_dlssOutW || outH != g_dlssOutH) { static uint32_t n = 0; if (n++ < 5) logmsg("PostDof: skipped on frame %u - color %ux%u is not the DLSS output size %ux%u", g_frame, outW, outH, g_dlssOutW, g_dlssOutH); g_dofMissed++; }
        else if (dof_apply(cmd, dev, g_dlssOutW, g_dlssOutH, static_cast<DXGI_FORMAT>(cd.texture.format), g_dofKx, g_dofKy)) g_dofFrames++; else g_dofMissed++;
    } else if (g_cfgPostDof && g_dofSeenThisFrame) g_dofMissed++;
    if (g_cfgProbe && !upscale && !NVSDK_NGX_FAILED(r)) probe_dispatch(cmd, g_out, g_outState, 1);
    objmv::mark_frame_end(native);

    if (fg::status().initialized && g_cfgFgMode != 0) {
        // Frame generation inputs: depth + motion vectors (render res) and, before post/HUD, the anti-aliased image as
        // HUD-less color. In composite mode the UI is baked into the image so no HUD-less color is tagged.
        fg::FrameInputs fi = {};
        fi.cmd = native;
        fi.depth = reinterpret_cast<ID3D12Resource*>(dlssDepth.handle); fi.depthFormat = depthStretched ? DXGI_FORMAT_R32_FLOAT : static_cast<DXGI_FORMAT>(dd.texture.format); fi.depthState = depthStretched ? (D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        fi.mv = reinterpret_cast<ID3D12Resource*>(g_mv.handle); fi.mvState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        if (g_windowInjectedThisFrame) {
            // The DLSS output here is the 3D window's own target, not a HUD-less frame, and the Codec's panels are drawn
            // after this point. Without a HUD-less image DLSS-G warps those panels on generated frames whenever they move
            // (the call start collapses them: torn copies of the panel lines flashed across the screen). So the HUD-less
            // image is the final texture right before its first panel draw - the pre-HUD capture taken there - tagged
            // valid until present, seeded now with the frame as it is (in case no panel is drawn), plus the UI layer.
            if (g_preHud.handle && restore && restore->rt.handle && restore->rt_w == cd.texture.width && restore->rt_h == cd.texture.height && is_live(restore->rt.handle)) {
                if (g_preHudState != resource_usage::copy_dest) { cmd->barrier(g_preHud, g_preHudState, resource_usage::copy_dest); g_preHudState = resource_usage::copy_dest; }
                cmd->barrier(restore->rt, resource_usage::render_target, resource_usage::copy_source);
                cmd->copy_resource(restore->rt, g_preHud);
                cmd->barrier(restore->rt, resource_usage::copy_source, resource_usage::render_target);
                fi.hudless = reinterpret_cast<ID3D12Resource*>(g_preHud.handle); fi.hudlessFormat = static_cast<DXGI_FORMAT>(cd.texture.format); fi.hudlessState = D3D12_RESOURCE_STATE_COPY_DEST; fi.hudlessUntilPresent = true;
                static bool once = false; if (!once) { once = true; logmsg("FG (3D window): HUD-less = the final texture before its first panel draw (pre-HUD capture, valid until present); UI layer tagged"); }
            }
            if (g_ui.handle && g_uiRtv.handle && (g_cfgFgMode != 0 || g_cfgUiMask)) {
                if (!g_uiClearedThisFrame) {   // nothing replayed yet this frame: start from an empty layer (the first panel draw clears it again, cheaply)
                    if (g_uiState != resource_usage::render_target) { cmd->barrier(g_ui, g_uiState, resource_usage::render_target); g_uiState = resource_usage::render_target; }
                    const float zero[4] = { 0, 0, 0, 0 }; cmd->clear_render_target_view(g_uiRtv, zero);
                }
                fi.ui = reinterpret_cast<ID3D12Resource*>(g_ui.handle); fi.uiFormat = static_cast<DXGI_FORMAT>(cd.texture.format); fi.uiState = D3D12_RESOURCE_STATE_RENDER_TARGET; fi.uiUntilPresent = true;
            }
        } else if (g_finalPreHudThisFrame && !upscale && g_cfgDebugMode != 5) {
            fi.hudless = reinterpret_cast<ID3D12Resource*>(g_out.handle); fi.hudlessFormat = static_cast<DXGI_FORMAT>(cd.texture.format); fi.hudlessState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            if (g_ui.handle && (g_cfgFgMode != 0 || g_cfgUiMask)) { fi.ui = reinterpret_cast<ID3D12Resource*>(g_ui.handle); fi.uiFormat = static_cast<DXGI_FORMAT>(cd.texture.format); fi.uiState = D3D12_RESOURCE_STATE_RENDER_TARGET; fi.uiUntilPresent = true; }
            static bool once = false; if (!once) { once = true; logmsg("FG: HUD-less = DLSS output (pre-HUD insertion on the final texture); UI layer tagged valid-until-present"); }
        } else if (g_cfgPrePost && !upscale && g_cfgDebugMode != 5) { fi.hudless = reinterpret_cast<ID3D12Resource*>(g_out.handle); fi.hudlessFormat = static_cast<DXGI_FORMAT>(cd.texture.format); fi.hudlessState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; }
        else if (!g_cfgPrePost && !upscale && !drsActive && g_cfgDebugMode != 5 && g_ui.handle && g_uiDrawsThisFrame > 0) {
            // composite mode: the image DLSS-G sees has the HUD baked in; hand it the replayed HUD layer as UI color+alpha
            // so it re-composites the HUD on generated frames instead of warping it with the scene
            if (g_uiState != resource_usage::shader_resource_non_pixel) { cmd->barrier(g_ui, g_uiState, resource_usage::shader_resource_non_pixel); g_uiState = resource_usage::shader_resource_non_pixel; }
            fi.hudless = reinterpret_cast<ID3D12Resource*>(g_out.handle); fi.hudlessFormat = static_cast<DXGI_FORMAT>(cd.texture.format); fi.hudlessState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            if (g_preHudCaptured && g_hudless.handle && g_hudlessPso) {
                // true HUD-less: DLAA output with the pre-HUD capture under the UI layer
                cmd->barrier(g_out, resource_usage::unordered_access, resource_usage::copy_source);
                if (g_hudlessState != resource_usage::copy_dest) { cmd->barrier(g_hudless, g_hudlessState, resource_usage::copy_dest); g_hudlessState = resource_usage::copy_dest; }
                cmd->copy_resource(g_out, g_hudless);
                cmd->barrier(g_out, resource_usage::copy_source, resource_usage::unordered_access);
                cmd->barrier(g_hudless, resource_usage::copy_dest, resource_usage::unordered_access); g_hudlessState = resource_usage::unordered_access;
                if (g_preHudState != resource_usage::shader_resource_non_pixel) { cmd->barrier(g_preHud, g_preHudState, resource_usage::shader_resource_non_pixel); g_preHudState = resource_usage::shader_resource_non_pixel; }
                hudless_dispatch(cmd, cd.texture.width, cd.texture.height, static_cast<DXGI_FORMAT>(cd.texture.format));
                fi.hudless = reinterpret_cast<ID3D12Resource*>(g_hudless.handle);
                g_hudlessFrames++;
            }
            fi.ui = reinterpret_cast<ID3D12Resource*>(g_ui.handle); fi.uiFormat = static_cast<DXGI_FORMAT>(cd.texture.format); fi.uiState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            static bool once = false; if (!once) { once = true; logmsg("FG: UI layer tagged (%u HUD draws replayed this frame, %u post passes skipped); HUD-less %s", g_uiDrawsThisFrame, g_uiPostSkippedThisFrame, g_preHudCaptured ? "built from the pre-HUD capture" : "= DLAA output (no pre-HUD capture)"); }
        }
        fi.renderW = mvW; fi.renderH = mvH; fi.bbW = g_bbW; fi.bbH = g_bbH;
        fi.texW = cd.texture.width; fi.texH = cd.texture.height; fi.hudlessW = outW; fi.hudlessH = outH;
        fi.hudlessSubrect = false;   // full grid: the HUD-less image, depth and vectors all cover the whole texture
        if (g_cfgFgHintRescale && (fi.hudless || fi.ui) && g_bbW && g_bbH && (outW != g_bbW || outH != g_bbH) && !upscale) {
            // backbuffer smaller / larger than the render resolution: the hints are rescaled at the composite draw
            g_fgSrcW = outW; g_fgSrcH = outH;
            if (fg_bb_textures(dev, cd.texture.format)) {
                g_fgSrcHudless = resource{ reinterpret_cast<uint64_t>(fi.hudless) }; g_fgSrcHudlessState = fg_state_of(g_fgSrcHudless.handle);
                g_fgSrcUi = resource{ reinterpret_cast<uint64_t>(fi.ui) }; g_fgSrcUiState = fg_state_of(g_fgSrcUi.handle);
                if (fi.hudless && g_fgSrcHudlessState) { fi.hudless = reinterpret_cast<ID3D12Resource*>(g_fgHudlessBb.handle); fi.hudlessState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE; fi.hudlessUntilPresent = true; } else fi.hudless = nullptr;
                if (fi.ui && g_fgSrcUiState) { fi.ui = reinterpret_cast<ID3D12Resource*>(g_fgUiBb.handle); fi.uiState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE; fi.uiUntilPresent = true; } else fi.ui = nullptr;
                fi.hudlessW = g_bbW; fi.hudlessH = g_bbH;
                g_fgScalePending = fi.hudless != nullptr || fi.ui != nullptr;
            }
        }
        fi.vpX = (int32_t)g_gameVp[0]; fi.vpY = (int32_t)g_gameVp[1]; fi.vpW = (uint32_t)g_gameVp[2]; fi.vpH = (uint32_t)g_gameVp[3];
        // Frozen screens (pass-through frames) and the first frames after a transition are cuts for frame generation:
        // interpolating a still image gains nothing, and interpolating across the Codec's panel collapse / window
        // appearance tears the UI (the NVIDIA app's FG preset override disables DLSS-G's UI recomposition).
        const bool fgCut = ep.InReset != 0 || frozenPass || g_fgCutFrames > 0;
        if (g_fgCutFrames > 0 && !frozenPass) g_fgCutFrames--;
        fg::CameraInput ci = { g_frameVP, g_havePrevVP ? g_prevVP : g_frameVP, ep.InJitterOffsetX, ep.InJitterOffsetY, fgCut, mvW, mvH };
        fg::frame_inputs(g_frame, fi, ci);
    }

    if ((!g_recreated && ((g_cfgRecreateAfter > 0 && g_evalCount >= (uint32_t)g_cfgRecreateAfter) || (g_autoRecreateAt && g_evalCount >= g_autoRecreateAt))) || (g_recreateRequested && !g_oldFeature)) {
        g_recreated = true; g_recreateRequested = false;
        g_oldFeature = g_dlss; g_oldFeatureFrame = g_frame; g_dlss = nullptr;
        logmsg("re-creating DLSS feature (old feature released in a few frames)");
    }
    { ULONGLONG t = GetTickCount64(); g_evalRateN++; if (t - g_evalRateT0 >= 1000) { g_evalRate = g_evalRateN * 1000.0f / float(t - g_evalRateT0); g_evalRateN = 0; g_evalRateT0 = t; } }

    // DRS: the game's composite / post chain samples only the sub-rect, so the full-size output goes back into it.
    const bool resampled = drsActive && g_cfgDRS == 2 && g_scratch.handle && g_resamplePso && !NVSDK_NGX_FAILED(r);
    if (resampled) {
        cmd->barrier(g_out, resource_usage::unordered_access, resource_usage::shader_resource_non_pixel);
        if (g_scratchState != resource_usage::unordered_access) { cmd->barrier(g_scratch, g_scratchState, resource_usage::unordered_access); g_scratchState = resource_usage::unordered_access; }
        resample_dispatch(cmd, g_out, g_scratch, subW, subH, outW, outH, static_cast<DXGI_FORMAT>(cd.texture.format), 16, 8);
        cmd->barrier(g_out, resource_usage::shader_resource_non_pixel, resource_usage::unordered_access);
    }
    if (upscale) {
        // Composite draw samples the full-size output instead of the shrunk texture: rewrite its SRV descriptor in place.
        const resource res[2] = { color, depth };
        const resource_usage from[2] = { resource_usage::shader_resource_non_pixel, resource_usage::shader_resource_non_pixel };
        const resource_usage to[2] = { colorState, resource_usage::depth_stencil_write };
        cmd->barrier(2, res, from, to);
        if (resampled) {
            cmd->barrier(g_scratch, g_scratchState, resource_usage::shader_resource_pixel); g_scratchState = resource_usage::shader_resource_pixel;
            g_d3d->CreateShaderResourceView(reinterpret_cast<ID3D12Resource*>(g_scratch.handle), nullptr, D3D12_CPU_DESCRIPTOR_HANDLE{ (SIZE_T)srvCpu });
        } else {
            cmd->barrier(g_out, resource_usage::unordered_access, resource_usage::shader_resource_pixel); g_outState = resource_usage::shader_resource_pixel;
            if (!NVSDK_NGX_FAILED(r)) g_d3d->CreateShaderResourceView(reinterpret_cast<ID3D12Resource*>(g_out.handle), nullptr, D3D12_CPU_DESCRIPTOR_HANDLE{ (SIZE_T)srvCpu });
        }
    } else if (resampled) {
        const resource res[3] = { color, depth, g_scratch };
        const resource_usage from[3] = { resource_usage::shader_resource_non_pixel, resource_usage::shader_resource_non_pixel, g_scratchState };
        const resource_usage to[3] = { resource_usage::copy_dest, resource_usage::depth_stencil_write, resource_usage::copy_source };
        cmd->barrier(3, res, from, to); g_scratchState = resource_usage::copy_source;
        const subresource_box box = { 0, 0, 0, subW, subH, 1 };
        cmd->copy_texture_region(g_scratch, 0, &box, color, 0, &box, filter_mode::min_mag_mip_point);
        cmd->barrier(color, resource_usage::copy_dest, colorState);
    } else {
        const resource res[3] = { color, depth, g_out };
        const resource_usage from[3] = { resource_usage::shader_resource_non_pixel, resource_usage::shader_resource_non_pixel, resource_usage::unordered_access };
        const resource_usage to[3] = { resource_usage::copy_dest, resource_usage::depth_stencil_write, resource_usage::copy_source };
        cmd->barrier(3, res, from, to);
        if (!NVSDK_NGX_FAILED(r)) {
            if (g_windowMode && g_windowVp.width > 0) {   // only the 3D window belongs to us; the rest of that target is the game's
                const subresource_box box = { (uint32_t)g_windowVp.x, (uint32_t)g_windowVp.y, 0,
                                              (uint32_t)(g_windowVp.x + g_windowVp.width + 0.5f), (uint32_t)(g_windowVp.y + g_windowVp.height + 0.5f), 1 };
                cmd->copy_texture_region(g_out, 0, &box, color, 0, &box, filter_mode::min_mag_mip_point);
            } else cmd->copy_resource(g_out, color);
        }
        const resource res2[2] = { color, g_out };
        const resource_usage from2[2] = { resource_usage::copy_dest, resource_usage::copy_source };
        const resource_usage to2[2] = { colorState, resource_usage::unordered_access };
        cmd->barrier(2, res2, from2, to2); g_outState = resource_usage::unordered_access;
    }
    if (g_cfgDebugMode == 1 && !g_finalPreHudThisFrame) {   // (the magenta test clears the game's bound final texture; skipped at the pre-HUD insertion)
        resource target = upscale ? g_out : color;
        resource_usage tstate = upscale ? g_outState : colorState;
        resource_view rtv = { 0 };
        const resource_desc td = dev->get_resource_desc(target);
        if ((td.usage & resource_usage::render_target) == 0) { static bool once = false; if (!once) { once = true; logmsg("debug: target %p has no render-target usage; magenta test skipped", (void*)target.handle); } }
        else if (dev->create_resource_view(target, resource_usage::render_target, resource_view_desc(cd.texture.format), &rtv)) {
            const float magenta[4] = { 1.0f, 0.0f, 1.0f, 1.0f };
            cmd->barrier(target, tstate, resource_usage::render_target);
            cmd->clear_render_target_view(rtv, magenta);
            cmd->barrier(target, resource_usage::render_target, tstate);
            dev->destroy_resource_view(rtv);
        } else { static bool once = false; if (!once) { once = true; logmsg("debug: could not create RTV on %p", (void*)target.handle); } }
    }
    if (restore) restore_state(dev, cmd, *restore);
}

// ---- event handlers -------------------------------------------------------------------------------------------------
static bool on_create_resource(device* dev, resource_desc& desc, subresource_data*, resource_usage)
{
    if (!g_scaling || desc.type != resource_type::texture_2d) return false;
    if (desc.texture.width != g_internalW || desc.texture.height != g_internalH) return false;
    if (fg::inside_streamline()) { static int n = 0; if (n++ < 3) logmsg("not shrinking a %ux%u texture created by Streamline", desc.texture.width, desc.texture.height); return false; }
    if (g_internalW == g_bbW && g_internalH == g_bbH && (desc.usage & resource_usage::render_target) == 0) return false;
    desc.texture.width = g_renderW; desc.texture.height = g_renderH;
    return true;   // desc modified
}
static void on_init_resource(device* dev, const resource_desc& desc, const subresource_data*, resource_usage, resource res)
{
    if (desc.type == resource_type::texture_2d) { std::lock_guard<std::mutex> lock(g_scaledMutex); g_liveTex.insert(res.handle); }
    if (desc.type == resource_type::texture_2d && g_frame > 200 && desc.texture.width >= 640 && desc.texture.height >= 360 &&
        (desc.usage & (resource_usage::render_target | resource_usage::depth_stencil)) != 0) {
        static int n = 0; if (n++ < 20) logmsg("f%u: game created a render target/depth texture at runtime: %ux%u fmt=%u", g_frame, desc.texture.width, desc.texture.height, (unsigned)desc.texture.format);
    }
    if (!g_scaling || desc.type != resource_type::texture_2d) return;
    if (desc.texture.width == g_renderW && desc.texture.height == g_renderH) {
        std::lock_guard<std::mutex> lock(g_scaledMutex);
        g_scaledTex.insert(res.handle);
        static int n = 0; if (n++ < 12) logmsg("shrunk texture %p to %ux%u (fmt=%u usage=0x%X)", (void*)res.handle, desc.texture.width, desc.texture.height, (unsigned)desc.texture.format, (unsigned)desc.usage);
    }
}
static void on_init_pipeline(device*, pipeline_layout, uint32_t count, const pipeline_subobject* subs, pipeline p)
{
    pso_info info;
    for (uint32_t i = 0; i < count; ++i) {
        if (subs[i].type == pipeline_subobject_type::depth_stencil_state && subs[i].data) info.depth = static_cast<const depth_stencil_desc*>(subs[i].data)->depth_enable;
        if (subs[i].type == pipeline_subobject_type::input_layout && subs[i].data) {
            const input_element* el = static_cast<const input_element*>(subs[i].data);
            for (uint32_t k = 0; k < subs[i].count; ++k)
                if (el[k].semantic && (_stricmp(el[k].semantic, "BLENDWEIGHT") == 0 || _stricmp(el[k].semantic, "BLENDINDICES") == 0 || _stricmp(el[k].semantic, "WEIGHTS") == 0)) info.skinned = true;
        }
    }
    std::lock_guard<std::mutex> lock(g_psoMutex);
    g_psoDepth[p.handle] = info;
}
static void on_destroy_pipeline(device*, pipeline p)
{
    objmv::forget_pso(reinterpret_cast<ID3D12PipelineState*>(p.handle));
    std::lock_guard<std::mutex> lock(g_psoMutex);
    g_psoDepth.erase(p.handle);
}
static void on_destroy_resource(device*, resource res)
{
    forget_mapped(res.handle);
    {
        std::lock_guard<std::mutex> lock(g_scaledMutex);
        g_scaledTex.erase(res.handle);
        g_liveTex.erase(res.handle);
        g_dsvForDs.erase(res.handle);
    }
    // forget every cross-frame reference to it (level transitions destroy and recreate the render targets)
    if (g_prevBusiestRt == res.handle) g_prevBusiestRt = 0;
    if (g_prevBusiestRt2 == res.handle) g_prevBusiestRt2 = 0;
    if (g_finalRt[0] == res.handle) { g_finalRt[0] = 0; if (g_frozen) logmsg("frozen state cleared: final texture %p destroyed", (void*)res.handle); g_frozen = false; }
    if (g_keepSeed == res.handle) { g_keepSeed = 0; g_keepValid = false; }
    if (g_finalRt[1] == res.handle) g_finalRt[1] = 0;
    if (g_geoRt == res.handle) g_geoRt = 0;
    if (g_curGeoRt == res.handle) g_curGeoRt = 0;
    g_depthDrawsPerRt.erase(res.handle); g_seedTex.erase(res.handle);
    if (g_lastDepth == res.handle) g_lastDepth = 0;
    g_drawsPerRt.erase(res.handle); g_drawsPerDs.erase(res.handle);
    for (auto it = g_dsForRt.begin(); it != g_dsForRt.end();) { if (it->first == res.handle || it->second == res.handle) it = g_dsForRt.erase(it); else ++it; }
    {
        std::lock_guard<std::mutex> lock(g_cvMutex);
        for (auto it = g_copiedViews.begin(); it != g_copiedViews.end();) { if (it->second == res.handle) it = g_copiedViews.erase(it); else ++it; }
    }
    std::lock_guard<std::mutex> lock(g_clMutex);
    for (auto& kv : g_cl) { if (kv.second.rt.handle == res.handle) { kv.second.rt = { 0 }; kv.second.rt_w = kv.second.rt_h = 0; } if (kv.second.ds.handle == res.handle) kv.second.ds = { 0 }; }
}
static void on_init_resource_view(device* dev, resource res, resource_usage usage, const resource_view_desc&, resource_view view)
{
    if ((usage & (resource_usage::shader_resource | resource_usage::unordered_access)) == 0) return;
    g_viewEvents++;
    if (tracing() && g_viewSamples < 4 && scene_sized(dev, res)) {
        g_viewSamples++;
        logmsg("   init_resource_view: handle=%llx -> %s", (unsigned long long)view.handle, desc_str(dev, res).c_str());
    }
}
static bool on_copy_descriptor_tables(device* dev, uint32_t count, const descriptor_table_copy* copies)
{
    g_copyEvents += count;
    for (uint32_t c = 0; c < count; ++c) {
        const descriptor_table_copy& cp = copies[c];
        uint64_t src = 0, dst = 0, size = 0; D3D12_DESCRIPTOR_HEAP_TYPE st, dt;
        if (!table_to_cpu(dev, cp.source_table, cp.source_binding, &src, &size, &st) || st != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) continue;
        if (!table_to_cpu(dev, cp.dest_table, cp.dest_binding, &dst, &size, &dt) || dt != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) continue;
        for (uint32_t i = 0; i < cp.count; ++i) {
            resource r = lookup_view(dev, src + i * size);
            if (!r.handle) continue;
            std::lock_guard<std::mutex> lock(g_cvMutex);
            g_copiedViews[dst + i * size] = r.handle;
        }
    }
    return false;
}

static void apply_viewport_scale(command_list* cmd, cl_state& s)
{
    // Re-issue the game's viewport/scissor scaled to the shrunk target (or unscaled when leaving one).
    if (t_reentrant) return;
    t_reentrant = true;
    const float k = s.rt_scaled ? float(g_renderW) / float(g_internalW) : 1.0f;
    const float ky = s.rt_scaled ? float(g_renderH) / float(g_internalH) : 1.0f;
    if (s.vp_valid) {
        viewport v = s.vp; v.x *= k; v.y *= ky; v.width *= k; v.height *= ky;
        cmd->bind_viewports(0, 1, &v);
    }
    if (s.sc_valid) {
        rect r = s.sc;
        r.left = int32_t(r.left * k); r.top = int32_t(r.top * ky); r.right = int32_t(r.right * k + 0.5f); r.bottom = int32_t(r.bottom * ky + 0.5f);
        cmd->bind_scissor_rects(0, 1, &r);
    }
    t_reentrant = false;
}
static void on_bind_rts(command_list* cmd, uint32_t count, const resource_view* rtvs, resource_view dsv)
{
    if (t_reentrant) return;   // our own temporary/restore binds during a replayed draw
    device* dev = cmd->get_device();
    resource rt = (count > 0 && rtvs[0].handle) ? dev->get_resource_from_view(rtvs[0]) : resource{ 0 };
    resource ds = dsv.handle ? dev->get_resource_from_view(dsv) : resource{ 0 };
    std::lock_guard<std::mutex> lock(g_clMutex);
    cl_state& s = g_cl[cmd];
    s.rt = rt; s.ds = ds; s.rt_w = s.rt_h = 0;
    s.rtv_count = count > 8 ? 8 : count; for (uint32_t i = 0; i < s.rtv_count; ++i) s.rtvs[i] = rtvs[i]; s.dsv = dsv;
    if (rt.handle) { resource_desc d = dev->get_resource_desc(rt); s.rt_w = d.texture.width; s.rt_h = d.texture.height; }
    const bool scaled = g_scaling && (is_scaled(rt) || (!rt.handle && is_scaled(ds)));
    if (g_scaling && (scaled || s.rt_scaled)) { s.rt_scaled = scaled; apply_viewport_scale(cmd, s); }
    else s.rt_scaled = scaled;
}
static void on_bind_viewports(command_list* cmd, uint32_t first, uint32_t count, const viewport* vp)
{
    if (t_reentrant || first != 0 || count == 0) return;
    std::lock_guard<std::mutex> lock(g_clMutex);
    cl_state& s = g_cl[cmd];
    s.vp = vp[0]; s.vp_valid = true;
    if (g_scaling && s.rt_scaled) { bool sc = s.sc_valid; s.sc_valid = false; apply_viewport_scale(cmd, s); s.sc_valid = sc; }
}
static void on_bind_scissor_rects(command_list* cmd, uint32_t first, uint32_t count, const rect* rects)
{
    if (t_reentrant || first != 0 || count == 0) return;
    std::lock_guard<std::mutex> lock(g_clMutex);
    cl_state& s = g_cl[cmd];
    s.sc = rects[0]; s.sc_valid = true;
    if (g_scaling && s.rt_scaled) { bool vp = s.vp_valid; s.vp_valid = false; apply_viewport_scale(cmd, s); s.vp_valid = vp; }
}
static void on_bind_descriptor_tables(command_list* cmd, shader_stage stages, pipeline_layout layout, uint32_t first, uint32_t count, const descriptor_table* tables, uint32_t, const uint32_t*)
{
    if ((stages & shader_stage::all_graphics) == 0) return;
    std::lock_guard<std::mutex> lock(g_clMutex);
    cl_state& s = g_cl[cmd];
    s.root_sig = reinterpret_cast<ID3D12RootSignature*>(layout.handle);
    for (uint32_t i = 0; i < count; ++i) { uint32_t idx = first + i; if (idx < 5) { s.tables[idx] = tables[i]; s.table_set[idx] = true; s.cbv_set[idx] = false; } }
}
static void on_push_descriptors(command_list* cmd, shader_stage stages, pipeline_layout layout, uint32_t param, const descriptor_table_update& update)
{
    if ((stages & shader_stage::all_graphics) == 0 || update.type != descriptor_type::constant_buffer || update.count != 1 || param >= 5) return;
    const buffer_range* br = static_cast<const buffer_range*>(update.descriptors);
    ID3D12Resource* buf = reinterpret_cast<ID3D12Resource*>(br->buffer.handle);
    if (!buf) return;
    std::lock_guard<std::mutex> lock(g_clMutex);
    cl_state& s = g_cl[cmd];
    s.root_sig = reinterpret_cast<ID3D12RootSignature*>(layout.handle);
    s.cbv[param] = buf->GetGPUVirtualAddress() + br->offset; s.cbv_set[param] = true; s.table_set[param] = false;
    s.cbv_res[param] = br->buffer; s.cbv_off[param] = br->offset;
}
static void on_bind_vertex_buffers(command_list* cmd, uint32_t first, uint32_t count, const resource* buffers, const uint64_t* offsets, const uint32_t*)
{
    if (first != 0 || count == 0) return;
    std::lock_guard<std::mutex> lock(g_clMutex);
    cl_state& s = g_cl[cmd]; s.vb0 = buffers[0]; s.vb0_off = offsets ? offsets[0] : 0;
}
static void on_bind_index_buffer(command_list* cmd, resource buffer, uint64_t offset, uint32_t)
{
    std::lock_guard<std::mutex> lock(g_clMutex);
    cl_state& s = g_cl[cmd]; s.ib = buffer; s.ib_off = offset;
}
static void on_bind_pipeline_states(command_list* cmd, uint32_t count, const dynamic_state* states, const uint32_t* values)
{
    for (uint32_t i = 0; i < count; ++i) if (states[i] == dynamic_state::primitive_topology) { std::lock_guard<std::mutex> lock(g_clMutex); g_cl[cmd].topology = values[i]; }
}
static void on_bind_pipeline(command_list* cmd, pipeline_stage stages, pipeline p)
{
    if ((stages & pipeline_stage::all_graphics) == 0 && stages != pipeline_stage::all) return;
    std::lock_guard<std::mutex> lock(g_clMutex);
    g_cl[cmd].pso = reinterpret_cast<ID3D12PipelineState*>(p.handle);
}

static bool tracing() { return g_frame < g_traceUntil; }
static std::string desc_str(device* dev, resource r)
{
    if (!r.handle) return "none";
    if (!is_live(r.handle)) { char b[48]; snprintf(b, sizeof(b), "%p (destroyed)", (void*)r.handle); return b; }
    resource_desc d = dev->get_resource_desc(r);
    char b[96]; snprintf(b, sizeof(b), "%p %ux%u f%u", (void*)r.handle, d.texture.width, d.texture.height, (unsigned)d.texture.format);
    return b;
}
static bool is_backbuffer(resource r) { return g_backbuffers.count(r.handle) != 0 || fg::is_app_backbuffer(r.handle); }
static bool scene_sized(device* dev, resource r, resource_desc* out)
{
    if (!r.handle || is_backbuffer(r) || !is_live(r.handle)) return false;
    resource_desc d = dev->get_resource_desc(r);
    if (out) *out = d;
    // The cap is the larger of the backbuffer and the configured InternalRes: in a window smaller than the
    // render resolution (borderless 4K turned into a 94% window) the scene textures exceed the backbuffer.
    const uint32_t maxW = g_internalW > g_bbW ? g_internalW : g_bbW, maxH = g_internalH > g_bbH ? g_internalH : g_bbH;
    return d.type == resource_type::texture_2d && d.texture.width >= 640 && d.texture.width <= maxW && d.texture.height >= 360 && d.texture.height <= maxH;
}

static void remember_internal_res(uint32_t w, uint32_t h)
{
    static bool written = false;
    if ((g_internalW == w && g_internalH == h) || written) return;
    written = true;
    char v[32]; snprintf(v, sizeof(v), "%ux%u", w, h);
    WritePrivateProfileStringA("DLSS", "InternalRes", v, g_iniPath);
    logmsg("game renders at %ux%u (ini had %ux%u); wrote InternalRes=%s - upscaling modes use it after a restart", w, h, g_internalW, g_internalH, v);
    if (!g_scaling) { g_internalW = w; g_internalH = h; }
}


static bool probe_init(device* dev)
{
    if (g_probeInitTried) return g_probeReady;
    g_probeInitTried = true;
    if (!dof_init()) return false;   // root signature (static sampler) + the 256-byte CB ring
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {}; pso.pRootSignature = g_dofRootSig; pso.CS = { g_probe_cs, sizeof(g_probe_cs) };
    if (FAILED(g_d3d->CreateComputePipelineState(&pso, IID_PPV_ARGS(&g_probePso)))) { logmsg("probe: PSO failed"); return false; }
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 128, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
    if (FAILED(g_d3d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_probeHeap)))) { logmsg("probe: heap failed"); return false; }
    for (int i = 0; i < 4; ++i) {
        if (!dev->create_resource(resource_desc(PROBE_W, PROBE_H, 1, 1, format::r8_unorm, 1, memory_heap::default_, resource_usage::unordered_access | resource_usage::shader_resource | resource_usage::copy_source),
                                  nullptr, resource_usage::unordered_access, &g_probeTex[i])) { logmsg("probe: texture failed"); return false; }
        dev->set_resource_name(g_probeTex[i], "MGS4DLSS probe");
    }
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_READBACK, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1 };
    D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = UINT64(4) * 8 * PROBE_SLOT_BYTES; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc = { 1, 0 }; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(g_d3d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_probeRb)))) { logmsg("probe: readback buffer failed"); return false; }
    D3D12_RANGE all = { 0, (SIZE_T)bd.Width }; g_probeRb->Map(0, &all, reinterpret_cast<void**>(&g_probeRbPtr));
    g_probeReady = g_probeRbPtr != nullptr;
    logmsg("probe: ready (%s) - per-frame 240x135 readback of DLSS input / DoF output / composite input", g_probeReady ? "ok" : "map failed");
    return g_probeReady;
}
static DXGI_FORMAT probe_srv_format(format f)
{
    switch (f) {
    case format::r8g8b8a8_typeless: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case format::b8g8r8a8_typeless: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case format::r16g16b16a16_typeless: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case format::r10g10b10a2_typeless: return DXGI_FORMAT_R10G10B10A2_UNORM;
    default: return static_cast<DXGI_FORMAT>(f);
    }
}
static void probe_dispatch(command_list* cmd, resource src, resource_usage srcState, int stage)
{
    device* dev = cmd->get_device();
    if (!probe_init(dev) || !src.handle) return;
    const resource_desc sd = dev->get_resource_desc(src);
    if (sd.type != resource_type::texture_2d) return;
    const DXGI_FORMAT sf = probe_srv_format(sd.texture.format);
    ID3D12GraphicsCommandList* native = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd->get_native());
    const UINT inc = g_d3d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const uint32_t g = (g_probeSlot * 4 + (uint32_t)stage) * 4;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_probeHeap->GetCPUDescriptorHandleForHeapStart(); cpu.ptr += SIZE_T(g) * inc;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_probeHeap->GetGPUDescriptorHandleForHeapStart(); gpu.ptr += UINT64(g) * inc;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {}; srv.Format = sf; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Texture2D.MipLevels = 1;
    g_d3d->CreateShaderResourceView(reinterpret_cast<ID3D12Resource*>(src.handle), &srv, cpu);
    D3D12_CPU_DESCRIPTOR_HANDLE c1 = cpu; c1.ptr += inc; g_d3d->CreateShaderResourceView(reinterpret_cast<ID3D12Resource*>(src.handle), &srv, c1);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {}; uav.Format = DXGI_FORMAT_R8_UNORM; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE c2 = cpu; c2.ptr += 2 * inc; g_d3d->CreateUnorderedAccessView(reinterpret_cast<ID3D12Resource*>(g_probeTex[stage].handle), nullptr, &uav, c2);
    D3D12_CPU_DESCRIPTOR_HANDLE c3 = cpu; c3.ptr += 3 * inc; g_d3d->CreateUnorderedAccessView(reinterpret_cast<ID3D12Resource*>(g_probeTex[stage].handle), nullptr, &uav, c3);
    const bool srcMove = srcState != resource_usage::shader_resource_non_pixel;
    if (srcMove) cmd->barrier(src, srcState, resource_usage::shader_resource_non_pixel);
    if (g_probeTexState[stage] != resource_usage::unordered_access) { cmd->barrier(g_probeTex[stage], g_probeTexState[stage], resource_usage::unordered_access); g_probeTexState[stage] = resource_usage::unordered_access; }
    ID3D12DescriptorHeap* heaps[1] = { g_probeHeap };
    native->SetDescriptorHeaps(1, heaps);
    native->SetComputeRootSignature(g_dofRootSig);
    native->SetPipelineState(g_probePso);
    native->SetComputeRootConstantBufferView(0, g_dofCbRes->GetGPUVirtualAddress());
    native->SetComputeRootDescriptorTable(1, gpu);
    D3D12_GPU_DESCRIPTOR_HANDLE gu = gpu; gu.ptr += 2 * inc; native->SetComputeRootDescriptorTable(2, gu);
    native->Dispatch((PROBE_W + 7) / 8, (PROBE_H + 7) / 8, 1);
    cmd->barrier(g_probeTex[stage], resource_usage::unordered_access, resource_usage::copy_source); g_probeTexState[stage] = resource_usage::copy_source;
    D3D12_TEXTURE_COPY_LOCATION dst = {}; dst.pResource = g_probeRb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = UINT64((uint32_t)stage * 8 + g_probeSlot) * PROBE_SLOT_BYTES;
    dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8_UNORM, PROBE_W, PROBE_H, 1, PROBE_PITCH };
    D3D12_TEXTURE_COPY_LOCATION srcLoc = {}; srcLoc.pResource = reinterpret_cast<ID3D12Resource*>(g_probeTex[stage].handle); srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; srcLoc.SubresourceIndex = 0;
    native->CopyTextureRegion(&dst, 0, 0, 0, &srcLoc, nullptr);
    cmd->barrier(g_probeTex[stage], resource_usage::copy_source, resource_usage::unordered_access); g_probeTexState[stage] = resource_usage::unordered_access;
    if (srcMove) cmd->barrier(src, resource_usage::shader_resource_non_pixel, srcState);
    g_probeMeta[stage][g_probeSlot] = g_frame;
}
// CPU side: scan one read-back luminance image for a sub-rect boundary wall.
static void probe_analyze(int stage, uint32_t slot)
{
    const uint32_t frame = g_probeMeta[stage][slot];
    if (!frame) return;
    g_probeMeta[stage][slot] = 0;
    const uint8_t* img = g_probeRbPtr + (UINT64((uint32_t)stage * 8 + slot) * PROBE_SLOT_BYTES);
    // TRANSIENT walls: analyze the difference against the previous frame of the same stage. Static UI edges (HUD bars,
    // Codec panel borders) cancel out; a layout flash (sub-rect content appearing for a frame) leaves a sharp wall.
    const uint32_t prevSlot = (slot + 7) % 8;
    const uint32_t prevFrame = g_probeMeta[stage][prevSlot] ? g_probeMeta[stage][prevSlot] : frame - 1;
    const uint8_t* prv = g_probeRbPtr + (UINT64((uint32_t)stage * 8 + prevSlot) * PROBE_SLOT_BYTES);
    static float dbuf[4][PROBE_H][PROBE_W];   // per stage: |cur - prev|
    float (*d)[PROBE_W] = dbuf[stage];
    float colGrad[PROBE_W] = {}, rowGrad[PROBE_H] = {};
    double mean = 0, dmean = 0;
    for (uint32_t y = 0; y < PROBE_H; ++y) {
        const uint8_t* r = img + y * PROBE_PITCH; const uint8_t* q = prv + y * PROBE_PITCH;
        for (uint32_t x = 0; x < PROBE_W; ++x) { d[y][x] = fabsf(float(r[x]) - float(q[x])); mean += r[x]; dmean += d[y][x]; }
    }
    mean /= double(PROBE_W) * PROBE_H; dmean /= double(PROBE_W) * PROBE_H;
    if (mean < 2.0) return;      // black / fade frames carry no layout information
    if (dmean > 12.0) return;    // a cut: everything changes, no wall information
    for (uint32_t y = 0; y < PROBE_H; ++y) {
        for (uint32_t x = 0; x + 1 < PROBE_W; ++x) colGrad[x] += fabsf(d[y][x + 1] - d[y][x]);
        if (y + 1 < PROBE_H) for (uint32_t x = 0; x < PROBE_W; ++x) rowGrad[y] += fabsf(d[y + 1][x] - d[y][x]);
    }
    (void)prevFrame;
    float colBase = 0, rowBase = 0;
    for (uint32_t x = 0; x + 1 < PROBE_W; ++x) { colGrad[x] /= PROBE_H; colBase += colGrad[x]; }
    for (uint32_t y = 0; y + 1 < PROBE_H; ++y) { rowGrad[y] /= PROBE_W; rowBase += rowGrad[y]; }
    colBase /= PROBE_W - 1; rowBase /= PROBE_H - 1;
    // strongest wall in the k = 0.40..0.97 range, compared against its own neighborhood
    float bestC = 0; uint32_t bestX = 0;
    for (uint32_t x = PROBE_W * 2 / 5; x < PROBE_W * 97 / 100; ++x) {
        const float nb = 0.25f * (colGrad[x - 2] + colGrad[x - 1] + colGrad[x + 1] + colGrad[x + 2]);
        const float sc = colGrad[x] - nb;
        if (sc > bestC) { bestC = sc; bestX = x; }
    }
    float bestR = 0; uint32_t bestY = 0;
    for (uint32_t y = PROBE_H * 2 / 5; y < PROBE_H * 97 / 100; ++y) {
        const float nb = 0.25f * (rowGrad[y - 2] + rowGrad[y - 1] + rowGrad[y + 1] + rowGrad[y + 2]);
        const float sc = rowGrad[y] - nb;
        if (sc > bestR) { bestR = sc; bestY = y; }
    }
    const float kx = float(bestX + 1) / PROBE_W, kyv = float(bestY + 1) / PROBE_H;
    // a transient LAYOUT wall: a sharp rectangle edge in the frame difference AND the region beyond it is stale
    // (the previous frame shows through), while the region inside updates. A moving object edge updates both sides.
    double dInC = 0, dOutC = 0; uint32_t nInC = 0, nOutC = 0;
    for (uint32_t y = 0; y < PROBE_H; ++y) for (uint32_t x = 0; x < PROBE_W; ++x) { if (x <= bestX) { dInC += d[y][x]; nInC++; } else { dOutC += d[y][x]; nOutC++; } }
    dInC /= nInC ? nInC : 1; dOutC /= nOutC ? nOutC : 1;
    double dInR = 0, dOutR = 0; uint32_t nInR = 0, nOutR = 0;
    for (uint32_t y = 0; y < PROBE_H; ++y) for (uint32_t x = 0; x < PROBE_W; ++x) { if (y <= bestY) { dInR += d[y][x]; nInR++; } else { dOutR += d[y][x]; nOutR++; } }
    dInR /= nInR ? nInR : 1; dOutR /= nOutR ? nOutR : 1;
    const bool hitC = bestC > 4.0f && bestC > colBase * 6.0f && dOutC < 0.8 && dInC > dOutC * 4.0 + 1.0;
    const bool hitR = bestR > 4.0f && bestR > rowBase * 6.0f && dOutR < 0.8 && dInR > dOutR * 4.0 + 1.0;
    const bool hit = hitC || hitR;
    if (hit) {
        g_probeFlags[stage]++;
        if (g_probeLogs++ < 200) logmsg("PROBE f%u stage %d (%s): STALE-OUTSIDE layout wall %s, k = %.3f x %.3f (wall %.1f/%.1f, in/out diff %.2f/%.2f | %.2f/%.2f, mean %.0f, dmean %.1f)", frame, stage,
            stage == 0 ? "DLSS input" : (stage == 1 ? "output after DoF" : (stage == 2 ? "composite input" : "PRESENTED BACKBUFFER")), hitC ? (hitR ? "V+H" : "V") : "H", kx, kyv, bestC, bestR, dInC, dOutC, dInR, dOutR, mean, dmean);
    }
}

// One warm-up step on a frame without a 3D scene: create the resources + the DLSS feature at the size the scene will
// have (DLAA: InternalRes, the game's render size - the swapchain's only when that is not known yet) and run an
// evaluation on our own scratch textures. The game's state is restored afterwards (the draw goes on as usual).
// Not the swapchain's size by default: on a display wider than the game's 16:9 the swapchain is the whole panel
// (7680x2160 on a 32:9), and a feature built at that size is thrown away at the first real frame - a stall for
// nothing, and a 7680-wide "DLSS output" that an NGX-hooking add-on has been seen taking for the one to process.
static void prewarm_step(device* dev, command_list* cmd, const cl_state& s)
{
    if (g_scaling || g_bbW == 0 || g_bbH == 0) { g_warmDone = true; return; }   // only the DLAA layout is known in advance
    const uint32_t pw = g_internalW ? g_internalW : g_bbW, ph = g_internalH ? g_internalH : g_bbH;
    if (!ngx_init(dev)) { logmsg("pre-warm: NGX init failed - giving up"); g_warmDone = true; return; }   // with Streamline, NGX is not initialized at device creation
    LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    const format fmt = g_dlssFmt != format::unknown ? g_dlssFmt : format::r8g8b8a8_unorm;
    if (!g_dlss) {
        if (!ensure_resources(dev, cmd, pw, ph, pw, ph, fmt)) { logmsg("pre-warm: feature creation failed - giving up"); g_warmDone = true; return; }
        QueryPerformanceCounter(&t1);
        logmsg("pre-warm: DLSS feature created on a no-3D frame at %ux%u (f%u, %.0f ms)", pw, ph, g_frame, double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart));
        restore_state(dev, cmd, s);
        return;   // the evaluations start next frame (NGX skips the create frame anyway)
    }
    if (!g_hudless.handle || !g_depthFull.handle || !g_mv.handle || !g_out.handle) { g_warmDone = true; return; }
    ID3D12GraphicsCommandList* native = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd->get_native());
    if (g_hudlessState != resource_usage::shader_resource_non_pixel) { cmd->barrier(g_hudless, g_hudlessState, resource_usage::shader_resource_non_pixel); g_hudlessState = resource_usage::shader_resource_non_pixel; }
    if (g_depthFullState != resource_usage::shader_resource_non_pixel) { cmd->barrier(g_depthFull, g_depthFullState, resource_usage::shader_resource_non_pixel); g_depthFullState = resource_usage::shader_resource_non_pixel; }
    if (g_mvState != resource_usage::shader_resource_non_pixel) { cmd->barrier(g_mv, g_mvState, resource_usage::shader_resource_non_pixel); g_mvState = resource_usage::shader_resource_non_pixel; }
    if (g_outState != resource_usage::unordered_access) { cmd->barrier(g_out, g_outState, resource_usage::unordered_access); g_outState = resource_usage::unordered_access; }
    NVSDK_NGX_D3D12_DLSS_Eval_Params ep = {};
    ep.Feature.pInColor = reinterpret_cast<ID3D12Resource*>(g_hudless.handle);
    ep.Feature.pInOutput = reinterpret_cast<ID3D12Resource*>(g_out.handle);
    ep.Feature.InSharpness = g_cfgSharpness100 / 100.0f;
    ep.pInDepth = reinterpret_cast<ID3D12Resource*>(g_depthFull.handle);
    ep.pInMotionVectors = reinterpret_cast<ID3D12Resource*>(g_mv.handle);
    ep.InRenderSubrectDimensions = NVSDK_NGX_Dimensions{ g_dlssW, g_dlssH };
    ep.InReset = 1; ep.InMVScaleX = 1.0f; ep.InMVScaleY = 1.0f; ep.InPreExposure = 1.0f; ep.InExposureScale = 1.0f;
    const NVSDK_NGX_Result r = NGX_D3D12_EVALUATE_DLSS_EXT(native, g_dlss, g_ngxParams, &ep);
    QueryPerformanceCounter(&t1);
    g_warmEvals++; g_evalCount++;
    logmsg("pre-warm: evaluation %u on a no-3D frame (f%u) -> %s, %.1f ms CPU", g_warmEvals, g_frame, ngx_str(r), double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart));
    // an NGX-hooking add-on that only saw our create after installing its hooks gets the one re-create here, off-screen
    if (!g_recreated && g_autoRecreateAt && g_evalCount >= g_autoRecreateAt) { g_recreated = true; g_oldFeature = g_dlss; g_oldFeatureFrame = g_frame; g_dlss = nullptr; logmsg("pre-warm: re-creating the DLSS feature for the NGX-hooking add-on (off-screen)"); }
    if (NVSDK_NGX_FAILED(r) || g_warmEvals >= 12) { g_warmDone = true; logmsg("pre-warm: done (%u evaluations)", g_warmEvals); }
    restore_state(dev, cmd, s);
}

struct draw_args { bool indexed; uint32_t count, instances, first, first_instance; int32_t vertex_offset; };
static void handle_draw(command_list* cmd, const draw_args& da)
{
    if (t_reentrant) return;   // our replayed draw
    cl_state s;
    {
        std::lock_guard<std::mutex> lock(g_clMutex);
        s = g_cl[cmd];
    }
    if (!s.rt.handle || g_bbW == 0) return;
    device* dev = cmd->get_device();
    if (g_cfgPreWarm && !g_warmDone && g_cfgEnabled && g_noSceneFrames >= 30 && g_sceneDrawsThisFrame == 0 && g_depthOnDrawsThisFrame == 0 && !is_backbuffer(s.rt))
        prewarm_step(dev, cmd, s);
    if (!is_backbuffer(s.rt)) {
        if (s.rt_w >= 640 && s.rt_h >= 360) {
            if (++g_sceneDrawsThisFrame == 1) { fg::frame_begin(g_frame); objmv::mark_frame_begin(reinterpret_cast<ID3D12GraphicsCommandList*>(cmd->get_native())); }
            g_drawsPerRt[s.rt.handle]++;
            const pso_info psoInfo = pso_get(s.pso);   // one lookup per draw (depth test enabled, skinned)
            const bool depthTested = psoInfo.depth;
            // frozen screens (Codec / pause): log the full-frame draw chain so the snapshot the background is built
            // from can be identified (source texture -> blur target -> the blit into the final image)
            if ((g_cfgTraceFreeze || tracing()) && s.rt_w >= 1280 && s.rt_h >= 720) {
                // blits / fullscreen passes (few vertices) and any draw whose first texture is frame-sized; geometry is summarized per frame
                resource src0 = { 0 }; uint32_t sw = 0, sh = 0;
                if (s.table_set[1]) { resource r = resolve_descriptor(dev, s.tables[1], 0); if (r.handle && is_live(r.handle)) { resource_desc d = dev->get_resource_desc(r); if (d.type == resource_type::texture_2d) { src0 = r; sw = d.texture.width; sh = d.texture.height; } } }
                if (da.count <= 8 || sw >= 640) {
                    std::string big;   // every frame-sized texture bound in the first two tables
                    for (int p = 1; p < 3; ++p) if (s.table_set[p]) for (int i = 0; i < 8; ++i) { resource r = resolve_descriptor(dev, s.tables[p], i); if (r.handle && is_live(r.handle)) { resource_desc d = dev->get_resource_desc(r); if (d.type == resource_type::texture_2d && d.texture.width >= 640) { char b[64]; snprintf(b, sizeof(b), " r%d[%d]=%p %ux%u", p, i, (void*)r.handle, d.texture.width, d.texture.height); big += b; } } }
                    frz_record("f%u draw rt=%p %ux%u%s vp=(%.0f,%.0f %.0fx%.0f) n=%u ds=%p depthOn=%d pso=%p ps=%016llx src0=%p %ux%u%s |%s [fullVp %d win %d inj %d]", g_frame, (void*)s.rt.handle, s.rt_w, s.rt_h,
                               s.rt.handle == g_finalRt[0] ? " FINAL0" : (s.rt.handle == g_finalRt[1] ? " FINAL1" : ""), s.vp.x, s.vp.y, s.vp.width, s.vp.height, da.count, (void*)s.ds.handle, (int)depthTested, (void*)s.pso, (unsigned long long)objmv::pso_ps_hash(s.pso),
                               (void*)src0.handle, sw, sh, "", big.c_str(), (int)g_sceneVpFrameValid, (int)g_winVpFrameValid, (int)g_injectedThisFrame);
                }
            }
            if (g_cfgPostDof && !g_cfgPrePost && g_dofPrevOk && da.count <= 8 && s.rt_w >= 1280 && g_cfgEnabled) {
                // The game's DoF: CoC pass (half-res viewport, samples the depth copy) -> spiral gather -> blend over the
                // sharp image. Skip all three; the CoC is evaluated right here from the same depth copy and constants
                // (dof_coc_dispatch), the gather and the blend run on the DLSS output at the insertion (dof_apply).
                const uint64_t ph = objmv::pso_ps_hash(s.pso);
                if (ph == PS_DOF_COC || ph == PS_DOF_GATHER || ph == PS_DOF_COMPOSITE) {
                    if (ph == PS_DOF_COC) {
                        // Dynamic resolution: the game's DoF runs on the scene sub-rect (this pass's viewport is half of it);
                        // that gives this frame's exact scale k for the depth read, the spiral step and the overlay mask.
                        const float kx = s.vp_valid ? s.vp.width * 2.0f / float(s.rt_w) : 1.0f, ky = s.vp_valid ? s.vp.height * 2.0f / float(s.rt_h) : 1.0f;
                        const bool subRect = !s.vp_valid || kx < 0.999f || ky < 0.999f;
                        g_dofKx = (s.vp_valid && kx > 0.05f) ? (kx > 1.0f ? 1.0f : kx) : 1.0f; g_dofKy = (s.vp_valid && ky > 0.05f) ? (ky > 1.0f ? 1.0f : ky) : 1.0f;
                        static float lastKx = 1.0f, lastKy = 1.0f; const bool kStep = kx != lastKx || ky != lastKy; lastKx = kx; lastKy = ky;
                        // A resolution-step frame keeps the game's DoF for that one frame (measured on 4K60 display
                        // captures of the cemetery entry ramp: the fallback flashes at +0.30 relative sharpness on the
                        // step frames, handling them at the new scale flashed at +0.75 - parts of the chain can lag the
                        // step by a frame). A genuine disagreement between the scene's viewport and the post chain's
                        // scale (never observed since the CoC-viewport step check landed, kept as a safety net) also
                        // falls back to the game's DoF.
                        bool mismatch = !kStep && g_sceneVpFrameValid && s.vp_valid && (fabsf(g_sceneVpFrame.width - kx * float(s.rt_w)) > 2.0f || fabsf(g_sceneVpFrame.height - ky * float(s.rt_h)) > 2.0f);
                        if (mismatch) { static uint32_t nm = 0; if (nm++ < 20) logmsg("PostDof: f%u scene viewport %.0fx%.0f disagrees with the post chain (k %.3f -> %.0fx%.0f) - the game's DoF for this frame", g_frame, g_sceneVpFrame.width, g_sceneVpFrame.height, kx, kx * float(s.rt_w), ky * float(s.rt_h)); }
                        if (const viewport* lw = layout_window()) { mismatch = true; static uint32_t nl = 0; if (nl++ < 5) logmsg("PostDof: f%u the scene is a layout window (%.0f,%.0f %.0fx%.0f) - the game's DoF stays", g_frame, lw->x, lw->y, lw->width, lw->height); }
                        g_dofSkipFrame = s.vp_valid && !kStep && !mismatch && (!subRect || g_cfgDofSubRect != 0);
                        g_frameKStep = kStep || mismatch;
                        if (kStep) { static uint32_t nk = 0; if (nk++ < 20) logmsg("PostDof: f%u resolution step (k %.3f x %.3f) - the game's DoF for this frame", g_frame, kx, ky); }
                        if (subRect) {
                            g_dofSubRectFrames++;
                            static uint32_t nlog = 0; if (nlog++ < 5) logmsg("PostDof: f%u dynamic-resolution sub-rect (CoC viewport %.0fx%.0f of %ux%u -> k %.3f x %.3f) - %s", g_frame, s.vp.width, s.vp.height, s.rt_w, s.rt_h, g_dofKx, g_dofKy, g_dofSkipFrame ? "handled at that scale" : "the game's DoF stays");
                        }
                        if (g_dofSkipFrame) {
                            // This frame's inputs, all taken from this draw: the constants, the depth copy it samples, and the CoC
                            // itself, evaluated now (the depth copy is exactly what the game's pass would have read). Anything
                            // missing -> the game's own DoF runs this frame, consistently for all three passes.
                            float cb[18 * 4]; const bool cbOk = read_cbv(s, 2, cb, 18 * 4);
                            const resource dep = s.table_set[1] ? resolve_descriptor(dev, s.tables[1], 1) : resource{ 0 };
                            const bool depOk = dep.handle && is_live(dep.handle);
                            bool cocOk = false, ok = cbOk && depOk;
                            if (ok) {
                                memcpy(g_dofCb, cb + 8 * 4, sizeof(g_dofCb)); g_dofCbValid = true;
                                const resource_desc rd = dev->get_resource_desc(s.rt);
                                g_dofCocUnorm = rd.texture.format == format::r8g8b8a8_unorm || rd.texture.format == format::b8g8r8a8_unorm || rd.texture.format == format::r8g8b8a8_unorm_srgb;
                                {   // diagnostics: the depth copy or its addressing changing is where a mis-scaled blur layer would come from
                                    static uint64_t lastDep = 0; static float lastC16[2] = { 0, 0 }; static uint32_t nlog = 0;
                                    const float* c16 = g_dofCb + 8 * 4; const float* c12 = g_dofCb + 4 * 4;
                                    if ((dep.handle != lastDep || c16[0] != lastC16[0] || c16[1] != lastC16[1]) && nlog++ < 24)
                                        logmsg("PostDof: f%u depth copy %s, c16 %.0fx%.0f, c12 %.3f,%.3f, k %.4f x %.4f (CoC viewport %.0fx%.0f into %ux%u)", g_frame, desc_str(dev, dep).c_str(), c16[0], c16[1], c12[0], c12[1], g_dofKx, g_dofKy, s.vp.width, s.vp.height, s.rt_w, s.rt_h);
                                    lastDep = dep.handle; lastC16[0] = c16[0]; lastC16[1] = c16[1];
                                }
                                g_dofDepth = dep.handle;
                                cocOk = ok = dof_coc_dispatch(cmd, dev, s, dep, s.rt_w, s.rt_h, g_dofKx, g_dofKy);
                            }
                            if (!ok) {
                                g_dofSkipFrame = false; g_dofFallbacks++;
                                static uint32_t nf = 0; if (nf++ < 20) logmsg("PostDof: f%u inputs missing at the CoC draw (constants %d, depth copy %d, CoC pass %d) - the game's DoF for this frame", g_frame, (int)cbOk, (int)depOk, (int)cocOk);
                            }
                        }
                    }
                    if (!g_dofSkipFrame) { if (s.ds.handle && depthTested) g_depthOnDrawsThisFrame++; goto dof_not_skipped; }
                    if (ph == PS_DOF_COC) {
                        g_dofCocSeen = true;
                        static uint32_t nlog = 0;
                        if (nlog++ < 3) {
                            logmsg("PostDof: CoC pass f%u into %s vp=(%.0f,%.0f %.0fx%.0f), depth copy %s, cb rows 8..17:", g_frame, desc_str(dev, s.rt).c_str(), s.vp.x, s.vp.y, s.vp.width, s.vp.height, desc_str(dev, resource{ g_dofDepth }).c_str());
                            for (int r = 0; r < 10 && g_dofCbValid; ++r) logmsg("   c%d = (%.4f %.4f %.4f %.4f)", 8 + r, g_dofCb[r * 4], g_dofCb[r * 4 + 1], g_dofCb[r * 4 + 2], g_dofCb[r * 4 + 3]);
                        }
                    } else if (ph == PS_DOF_GATHER) {
                        float cb[18 * 4];
                        if (read_cbv(s, 2, cb, 18 * 4)) { memcpy(g_dofCbG, cb + 8 * 4, sizeof(g_dofCbG)); g_dofCbGValid = true; }
                        else { static uint32_t n = 0; if (n++ < 5) logmsg("PostDof: f%u gather constants unreadable - the previous frame's are kept", g_frame); }
                        static uint32_t nlog = 0;
                        if (nlog++ < 2 && g_dofCbGValid) { logmsg("PostDof: gather pass f%u into %s vp=(%.0f,%.0f %.0fx%.0f), cb rows 8..17:", g_frame, desc_str(dev, s.rt).c_str(), s.vp.x, s.vp.y, s.vp.width, s.vp.height); for (int r = 0; r < 10; ++r) logmsg("   c%d = (%.5f %.5f %.5f %.5f)", 8 + r, g_dofCbG[r * 4], g_dofCbG[r * 4 + 1], g_dofCbG[r * 4 + 2], g_dofCbG[r * 4 + 3]); }
                    } else if (ph == PS_DOF_COMPOSITE) g_dofSeenThisFrame = true;
                    g_skipThisDraw = true; g_dofSkipped++;
                    return;
                }
            }
            if (s.ds.handle && depthTested) g_depthOnDrawsThisFrame++;
            if (!depthTested && da.count == 6 && s.rt_w == g_dlssW && s.rt_h == g_dlssH && g_dlssW && s.rt.handle != g_finalRt[0] && s.rt.handle != g_finalRt[1]
                && g_depthOnDrawsThisFrame >= 20 && !g_injectedThisFrame && s.table_set[1] && g_preHudLast && g_cfgEnabled && !g_cfgPrePost && !g_scaling && g_cfgDebugMode != 2 && g_flashFrame != g_frame
                && s.vp_valid && (vp_full_frame(s.vp, float(s.rt_w), float(s.rt_h)) || vp_is_layout_scene(s.vp))) {
                resource r0 = resolve_descriptor(dev, s.tables[1], 0);   // the flashback footage pass: the video in slot 0 (see g_flashFrame)
                auto itSeen = r0.handle ? g_rtSeen.find(r0.handle) : g_rtSeen.end();
                if (r0.handle && is_live(r0.handle) && (itSeen == g_rtSeen.end() || g_frame - itSeen->second > 600)) {
                    const resource_desc d0 = dev->get_resource_desc(r0);
                    if (d0.type == resource_type::texture_2d && d0.texture.width == 512 && d0.texture.height == 256 && d0.texture.format == format::r8g8b8a8_unorm) {
                        g_flashFrame = g_frame;
                        static uint32_t nlog = 0; if (nlog++ < 4) logmsg("flashback: f%u footage pass (%ux%u video into %s) - DLSS takes the current frame for the whole picture", g_frame, d0.texture.width, d0.texture.height, desc_str(dev, s.rt).c_str());
                    }
                }
            }
        dof_not_skipped:
            if (g_cfgPostDof && g_dofSkipFrame && !g_dofCombineSeen && da.count <= 4 && s.rt_w == g_dlssW && objmv::pso_ps_hash(s.pso) == PS_DOF_COMBINE) g_dofCombineSeen = true;   // the DoF combine (3-vertex pass): overlays come after it
            if (g_cfgPostDof && g_cfgDofMask && g_dofSkipFrame && g_dofCombineSeen && !g_injectedThisFrame && !depthTested && (s.topology == 4 || s.topology == 5) && da.count >= 5 && da.count <= 8 && s.rt_w == g_dlssW && s.rt_h == g_dlssH
                && s.rt.handle != g_finalRt[0] && s.rt.handle != g_finalRt[1] && g_dlssW && g_dofReady) {
                {
                    // quads / strips drawn into a scene-sized (non-final) texture after the DoF combine whose primary input is
                    // not the scene itself: title cards and captions. Fullscreen post passes (3/4 vertices, scene input) are not.
                    bool slot0scene = false;
                    if (s.table_set[1]) { resource r0 = resolve_descriptor(dev, s.tables[1], 0); if (r0.handle && is_live(r0.handle)) { resource_desc d0 = dev->get_resource_desc(r0); slot0scene = d0.type == resource_type::texture_2d && d0.texture.width * 2 >= g_dlssW && d0.texture.height * 2 >= g_dlssH /* any aspect: the 2048x2048 previous-shot capture drawn by the WIPE transitions is scene content, not a caption */; } }
                    if (!slot0scene) {
                        const resource_desc rd = dev->get_resource_desc(s.rt);
                        if (g_dofMask.handle && (g_dofMaskFmt != rd.texture.format)) { if (g_dofMaskRtv.handle) dev->destroy_resource_view(g_dofMaskRtv); dev->destroy_resource(g_dofMask); g_dofMask = { 0 }; g_dofMaskRtv = { 0 }; }
                        if (!g_dofMask.handle) {
                            if (dev->create_resource(resource_desc(g_dlssW, g_dlssH, 1, 1, rd.texture.format, 1, memory_heap::default_, resource_usage::render_target | resource_usage::shader_resource), nullptr, resource_usage::render_target, &g_dofMask)
                                && dev->create_resource_view(g_dofMask, resource_usage::render_target, resource_view_desc(rd.texture.format), &g_dofMaskRtv)) { dev->set_resource_name(g_dofMask, "MGS4DLSS DoF overlay mask"); g_dofMaskState = resource_usage::render_target; g_dofMaskFmt = rd.texture.format; logmsg("PostDof: overlay mask layer %ux%u fmt %u", g_dlssW, g_dlssH, (unsigned)rd.texture.format); }
                            else { logmsg("PostDof: overlay mask layer failed"); if (g_dofMask.handle) { dev->destroy_resource(g_dofMask); g_dofMask = { 0 }; } }
                        }
                        if (g_dofMask.handle && g_dofMaskRtv.handle) {
                            t_reentrant = true;
                            if (g_dofMaskState != resource_usage::render_target) { cmd->barrier(g_dofMask, g_dofMaskState, resource_usage::render_target); g_dofMaskState = resource_usage::render_target; }
                            if (!g_dofMaskCleared) { const float zero[4] = { 0, 0, 0, 0 }; cmd->clear_render_target_view(g_dofMaskRtv, zero); g_dofMaskCleared = true; }
                            cmd->bind_render_targets_and_depth_stencil(1, &g_dofMaskRtv, resource_view{ 0 });
                            if (da.indexed) cmd->draw_indexed(da.count, da.instances, da.first, da.vertex_offset, da.first_instance);
                            else cmd->draw(da.count, da.instances, da.first, da.first_instance);
                            cmd->bind_render_targets_and_depth_stencil(s.rtv_count, s.rtvs, s.dsv);
                            t_reentrant = false;
                            g_dofOverlaysThisFrame++; g_dofOverlays++;
                            static uint32_t nlog = 0; if (nlog++ < 6) logmsg("PostDof: overlay draw f%u replayed into the mask (%u verts, pso %p, into %s)", g_frame, da.count, (void*)s.pso, desc_str(dev, s.rt).c_str());
                        }
                    }
                }
            }
            if (s.ds.handle) {
                if (tracing()) { static uint32_t nf = 0, lastf = 0; if (lastf != g_frame) { lastf = g_frame; nf = 0; } if (nf++ < 12) logmsg("f%u depth-draw rt=%p %ux%u ds=%p vp=(%.0f,%.0f %.0fx%.0f) count=%u depthOn=%d", g_frame, (void*)s.rt.handle, s.rt_w, s.rt_h, (void*)s.ds.handle, s.vp.x, s.vp.y, s.vp.width, s.vp.height, da.count, (int)depthTested); }
                g_dsForRt[s.rt.handle] = s.ds.handle; g_drawsPerDs[s.ds.handle]++;
                if (s.dsv.handle) g_dsvForDs[s.ds.handle] = s.dsv;
                if (g_layoutDumpFrames && depthTested && s.table_set[1] && s.vp_valid && vp_is_layout_scene(s.vp)) {   // diagnostics: what the main view's draws sample (the monitor showing the caller feed)
                    static uint32_t nm = 0, lfm = 0; if (lfm != g_frame) { lfm = g_frame; nm = 0; }
                    for (int i = 0; i < 8; ++i) { resource r = resolve_descriptor(dev, s.tables[1], i); if (!r.handle || !is_live(r.handle)) continue; resource_desc d = dev->get_resource_desc(r);
                        if (d.type == resource_type::texture_2d && d.texture.width >= 512 && d.texture.height >= 512) { auto& e = g_dumpTexHist[r.handle]; if (e.first++ == 0) { e.second.first = r.handle; e.second.second = d; } }
                        // a texture that is also a render target this frame: the monitor showing the caller feed (or a reflection)
                        if (d.type == resource_type::texture_2d && g_dumpRtSet.count(r.handle) && nm < 20) { nm++; logmsg("   f%u MONITOR? draw into %p vp=(%.0f,%.0f %.0fx%.0f) %u verts pso %p samples r1[%d] = %p %ux%u f%u (a target this frame)", g_frame, (void*)s.rt.handle, s.vp.x, s.vp.y, s.vp.width, s.vp.height, da.count, (void*)s.pso, i, (void*)r.handle, d.texture.width, d.texture.height, (unsigned)d.texture.format); } }
                }
                if (g_layoutDumpFrames && depthTested && s.vp_valid) {   // diagnostics: every target's depth-tested viewports
                    auto& e = g_dumpVpHist[(s.rt.handle * 1000003ull) ^ ((uint64_t)(uint32_t)(s.vp.x + 0.5f) << 48) ^ ((uint64_t)(uint32_t)(s.vp.y + 0.5f) << 32) ^ ((uint32_t)(s.vp.width + 0.5f) << 16) ^ (uint32_t)(s.vp.height + 0.5f)];
                    if (e.first++ == 0) { e.second.first = s.rt.handle; e.second.second = s.vp; }
                }
                if (depthTested && s.vp_valid && s.rt_w >= 640 && s.vp.width <= s.rt_w && (g_dlssW == 0 || s.rt_w == g_dlssW)
                    && (g_curGeoRt == 0 || s.rt.handle == g_curGeoRt)          // only the scene target, not shadow/reflection passes
                    && (vp_full_frame(s.vp, float(s.rt_w), float(s.rt_h))   // a plausible full-frame viewport
                        || vp_is_layout_scene(s.vp))) {                       // or the main view of a layout window (briefings)
                    // the 3D scene's viewport = the one most depth-tested draws use (a few full-size depth-tested quads exist too)
                    auto& e = g_vpHist[(uint64_t)(uint32_t)(s.vp.width + 0.5f) << 32 | (uint32_t)(s.vp.height + 0.5f)];
                    if (e.first++ == 0) e.second = s.vp;
                    if (!g_sceneVpFrameValid || e.first > g_vpHist[(uint64_t)(uint32_t)(g_sceneVpFrame.width + 0.5f) << 32 | (uint32_t)(g_sceneVpFrame.height + 0.5f)].first) { g_sceneVpFrame = e.second; g_sceneVpFrameValid = true; }
                } else if (depthTested && s.vp_valid && s.rt_w >= 640 && (g_dlssW == 0 || s.rt_w == g_dlssW) && s.vp.width >= s.rt_w * 0.2f && s.vp.height >= s.rt_h * 0.2f
                           && s.vp.width < s.rt_w - 1.0f && s.vp.x + s.vp.width <= s.rt_w + 1.0f && s.vp.y + s.vp.height <= s.rt_h + 1.0f) {
                    // a 3D window (Codec caller, pause-menu model): smaller than the frame or at an offset, blitted 1:1 to the final image
                    { uint32_t& n = g_winGeoDrawsPerRt[s.rt.handle]; if (++n > g_winGeoDraws) { g_winGeoDraws = n; g_winGeoRt = s.rt.handle; } }
                    if (const viewport* fl = feed_layout_now()) if (vp_in_layout(s.vp, *fl)) { uint32_t& n = g_feedDrawsPerRt[s.rt.handle]; if (++n > g_feedGeoDraws) { g_feedGeoDraws = n; g_feedGeoRt = s.rt.handle; g_feedVpFrame = s.vp; g_feedVpFrameValid = true; } }
                    if (const viewport* wlf = win_layout_now()) if (vp_in_layout(s.vp, *wlf)) { uint32_t& n = g_winFitDrawsPerRt[s.rt.handle]; if (++n > g_winFitDraws) { g_winFitDraws = n; g_winFitRt = s.rt.handle; g_winFitVp = s.vp; } }
                    auto& e = g_winVpHist[(uint64_t)(uint32_t)(s.vp.x + 0.5f) << 48 | (uint64_t)(uint32_t)(s.vp.y + 0.5f) << 32 | (uint32_t)(s.vp.width + 0.5f) << 16 | (uint32_t)(s.vp.height + 0.5f)];
                    if (e.first++ == 0) e.second = s.vp;
                    if (!g_winVpFrameValid || e.first > g_winVpHist[(uint64_t)(uint32_t)(g_winVpFrame.x + 0.5f) << 48 | (uint64_t)(uint32_t)(g_winVpFrame.y + 0.5f) << 32 | (uint32_t)(g_winVpFrame.width + 0.5f) << 16 | (uint32_t)(g_winVpFrame.height + 0.5f)].first) { g_winVpFrame = e.second; g_winVpFrameValid = true; }
                }
                if (dumping()) analyse_scene_draw(s);
                const bool skinned = psoInfo.skinned;
                int cls = -1;
                if (g_cfgEnabled && g_cfgDebugMode != 2) cls = jitter_scene_draw(s);
                if (skinned) g_skinnedDrawsThisFrame++;
                // Phase 2: dynamic draws = skinned meshes (optionally props with their own model matrix).
                const bool dynamic = skinned || (g_cfgDynMaskProps && cls >= 1);
                // Object motion vectors also for rigid movers with their own model matrix (ObjectMVProps=1): vehicles, the
                // Mk. II, doors - camera-only vectors ghost those under DLSS. Costs one stream-out draw per such prop.
                const bool dynamicMv = dynamic || (g_cfgObjMvProps && cls >= 1);
                // Per-object motion: stream out this draw's clip positions with one extra draw under the game's own state
                // (root signature, root arguments, IA buffers all as bound; only the pipeline and SO targets change).
                if (g_cfgObjectMV && dynamicMv && objmv::ready() && !g_injectedThisFrame && s.ds.handle == g_lastDepth && da.count > 6 && s.pso) {
                    // Identity across frames: the geometry (buffers, index range). Not the PSO: bgfx hands the same draw a
                    // different pipeline object every frame. Instances sharing a mesh (every PMC soldier) are told apart
                    // inside objmv by the head of their vertex constants (see objmv::capture).
                    uint64_t key = 1469598103934665603ull;
                    const uint64_t parts[8] = { s.vb0.handle, s.vb0_off, s.ib.handle, s.ib_off, da.first, da.count, (uint64_t)(int64_t)da.vertex_offset, da.instances };
                    for (uint64_t v : parts) { key ^= v; key *= 1099511628211ull; }
                    const region_info* ri = region_of(s);   // the constants the jitter patch already read for this region
                    const float* anchor = ri ? ri->anchor : nullptr; const uint32_t anchorN = ri ? ri->anchorN : 0;
                    objmv::DrawArgs oda = { da.indexed, da.count, da.instances, da.first, da.vertex_offset, da.first_instance };
                    const bool jittered = g_cfgJitter && (cls == 0 || cls == 1);   // its clip matrix was patched in place this frame
                    // Which view the draw belongs to. The main view (the frame, or a layout window's main view at any
                    // scale) rasterizes with the pass viewport; a 3D window whose place in the image is known (the
                    // briefing's camera window) rasterizes into that rectangle, whatever the port's scale; a window with
                    // no known place (pause-menu model, Codec caller) keeps its own viewport as before; anything else -
                    // the video call's caller, rendered at (0,0 2284x2160) and only seen on the Nomad's monitor - is not
                    // captured, its silhouette would land somewhere in the main view.
                    const bool mainClass = !s.vp_valid || vp_full_frame(s.vp, float(s.rt_w), float(s.rt_h)) || vp_is_layout_scene(s.vp);
                    const viewport* wl = mainClass ? nullptr : win_layout_now();
                    const bool winClass = !mainClass && wl && vp_in_layout(s.vp, *wl);
                    const viewport* fl = (mainClass || winClass) ? nullptr : feed_layout_now();
                    const bool feedClass = fl && vp_in_layout(s.vp, *fl) && g_feedMvRtv.handle && g_cfgMonitorProject;
                    const bool legacyWin = !mainClass && !winClass && !feedClass && !wl;
                    D3D12_VIEWPORT own = { s.vp.x, s.vp.y, s.vp.width, s.vp.height, s.vp.min_depth, s.vp.max_depth };
                    if (winClass) own = D3D12_VIEWPORT{ wl->x, wl->y, wl->width, wl->height, 0.0f, 1.0f };
                    if (feedClass) { const float tx = g_internalW ? float(g_dlssW) / float(g_internalW) : 1.0f, ty = g_internalH ? float(g_dlssH) / float(g_internalH) : 1.0f; own = D3D12_VIEWPORT{ fl->x * tx, fl->y * ty, fl->width * tx, fl->height * ty, 0.0f, 1.0f }; }
                    if (mainClass || winClass || feedClass || legacyWin) objmv::capture(reinterpret_cast<ID3D12GraphicsCommandList*>(cmd->get_native()), key, s.pso, s.topology, oda, jittered, mainClass ? nullptr : &own, anchor, anchorN, feedClass ? 2 : (winClass ? 1 : 0));
                    else g_objMvSkippedOther++;
                }
                // The in-world monitor showing the caller feed: a main-view draw sampling a texture that was copied from the
                // final texture this frame. Captured with its texture coordinates for the projector pass (objmv view 3).
                if (g_cfgMonitorProject && g_cfgObjectMV && objmv::ready() && !g_injectedThisFrame && s.ds.handle == g_lastDepth && s.pso && da.count >= 3 && !g_feedTexSet.empty() && s.table_set[1] && s.vp_valid
                    && (vp_full_frame(s.vp, float(s.rt_w), float(s.rt_h)) || vp_is_layout_scene(s.vp)) && feed_layout_now()) {
                    bool monitor = false;
                    for (int i = 0; i < 8 && !monitor; ++i) { resource r = resolve_descriptor(dev, s.tables[1], i); if (r.handle && g_feedTexSet.count(r.handle)) monitor = true; }
                    if (monitor) {
                        uint64_t key = 0x9E3779B97F4A7C15ull; const uint64_t parts[6] = { s.vb0.handle, s.vb0_off, s.ib.handle, s.ib_off, da.first, da.count }; for (uint64_t v : parts) { key ^= v; key *= 1099511628211ull; }
                        objmv::DrawArgs oda = { da.indexed, da.count, da.instances, da.first, da.vertex_offset, da.first_instance };
                        if (objmv::capture(reinterpret_cast<ID3D12GraphicsCommandList*>(cmd->get_native()), key, s.pso, s.topology, oda, false, nullptr, nullptr, 0, 3)) {
                            static uint32_t nlog = 0; if (nlog++ < 4) logmsg("LAYOUT: monitor draw f%u captured with texture coordinates: %u vertices, pso %p, vp (%.0f,%.0f %.0fx%.0f)", g_frame, da.count, (void*)s.pso, s.vp.x, s.vp.y, s.vp.width, s.vp.height);
                        }
                    }
                }
                if (g_cfgDynMask && dynamic && g_dynDepth.handle && g_dynDsv.handle && s.ds.handle == g_lastDepth && da.count > 6 && !g_injectedThisFrame) {
                    t_reentrant = true;
                    if (g_dynState != resource_usage::depth_stencil_write) { cmd->barrier(g_dynDepth, g_dynState, resource_usage::depth_stencil_write); g_dynState = resource_usage::depth_stencil_write; }
                    cmd->bind_render_targets_and_depth_stencil(0, nullptr, g_dynDsv);
                    if (da.indexed) cmd->draw_indexed(da.count, da.instances, da.first, da.vertex_offset, da.first_instance);
                    else cmd->draw(da.count, da.instances, da.first, da.first_instance);
                    cmd->bind_render_targets_and_depth_stencil(s.rtv_count, s.rtvs, s.dsv);
                    t_reentrant = false;
                    g_dynDrawsThisFrame++;
                }
            }
            // DLAA pre-HUD insertion. The 3D target = the RT that received the most depth-tested draws last frame. Once
            // most of this frame's depth-tested draws are in, DLSS runs on it at whichever comes first:
            //  (a) the first 2D (depth-disabled) draw into it - scenes that draw geometry straight into the final texture,
            //  (b) the first draw that samples it - scenes with a separate geometry target read by the post chain.
            const bool depthOn = depthTested;
            if (s.ds.handle) {
                const uint32_t n = ++g_depthDrawsPerRt[s.rt.handle];
                if (s.rt.handle == g_finalRt[0] || s.rt.handle == g_finalRt[1]) g_depthDrawsIntoFinal++;
                // in-frame detection of the 3D target: the first depth-bound RT that reaches 40% of last frame's peak,
                // drawn with a plausible full-frame viewport - a picture-in-picture pass (the Mk. II's monitor: 1024x1024
                // in a corner of the scene target, hundreds of draws) must not take the slot from the scene itself
                const bool fullFrameVp = !s.vp_valid || vp_full_frame(s.vp, float(s.rt_w), float(s.rt_h)) || vp_is_layout_scene(s.vp);
                // counted per target: the briefing's camera window (hundreds of draws at its own viewport, plus a few
                // full-viewport quads, rendered before the main view) must not take the slot from the main view either
                const uint32_t ns = fullFrameVp ? ++g_sceneClassDrawsPerRt[s.rt.handle] : g_sceneClassDrawsPerRt[s.rt.handle];
                // a final texture qualifies on its scene-class draws alone: the briefings' ~350 depth-bound panel quads
                // per frame (59 at the full viewport) reach 40% of the peak long before the main view is drawn
                const bool isFinal = s.rt.handle == g_finalRt[0] || s.rt.handle == g_finalRt[1];
                if (!g_curGeoRt && ns >= 20 && (isFinal ? ns : n) * 10 >= g_geoDrawsLast * 4) g_curGeoRt = s.rt.handle;
                // The briefings draw hundreds of depth-bound panel quads into the final texture (dozens at the full
                // viewport) before the main view, so the final texture can take the slot first; a target that then
                // gathers clearly more scene-class draws is the scene and takes it over.
                else if (g_curGeoRt && s.rt.handle != g_curGeoRt && fullFrameVp && ns >= 20 && ns > g_sceneClassDrawsPerRt[g_curGeoRt] * 5 / 4) { g_curGeoRt = s.rt.handle; g_geoRepicks++; }
            }
            g_geoRt = g_curGeoRt;
            // Frame generation, composite mode: replay HUD draws into the UI layer. HUD draws = depth-off draws into the
            // final texture once the 3D scene is in; post-process passes into it are told apart by sampling a scene-sized
            // input (half the frame size or more), HUD draws only sample atlases.
            // HUD candidates: depth-off draws into a final texture. The gate is the scene write into that texture
            // (g_finalSceneWritten / g_finalSceneRt below), not the in-frame geometry-target heuristics: those compare
            // against last frame's draw counts and fail on frames where the count swings (rolling, fast turns).
            // Frozen-screen seed (pause menu, Codec): on the last live frame the game downsamples the final texture into a
            // 1920x1080 seed - after its upscale, before the first HUD draw - and the frozen screens blit that seed back
            // over the final texture every frame. The capture comes BEFORE the pre-HUD insertion, so the still image was
            // the raw frame; running DLSS on the final texture right before the capture makes the seed the DLSS (+NR) image.
            // Shape: a few-vertex draw into a smaller, non-final target that samples this frame's final scene texture.
            if (g_keepValid && s.rt.handle == g_keepSeed) g_keepValid = false;   // the game re-captures into the seed without us: the kept frame is stale
            // The seed blit: a full-viewport draw into a final-size target sampling the seed at slot 0 -> sample the kept
            // full-size DLSS frame instead (descriptor rewritten in place, like the upscaling modes' composite redirect).
            if (g_keepValid && g_keep.handle && !depthOn && da.count <= 8 && s.rt_w == g_dlssOutW && s.rt_h == g_dlssOutH && s.table_set[1] && s.vp_valid && s.vp.width >= s.rt_w - 2.0f) {
                if (resolve_descriptor(dev, s.tables[1], 0).handle == g_keepSeed) {
                    uint64_t cpu = 0, size = 0; D3D12_DESCRIPTOR_HEAP_TYPE type;
                    if (table_to_cpu(dev, s.tables[1], 0, &cpu, &size, &type) && type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) {
                        if (g_keepState != resource_usage::shader_resource) { cmd->barrier(g_keep, g_keepState, resource_usage::shader_resource); g_keepState = resource_usage::shader_resource; }
                        g_d3d->CreateShaderResourceView(reinterpret_cast<ID3D12Resource*>(g_keep.handle), nullptr, D3D12_CPU_DESCRIPTOR_HANDLE{ (SIZE_T)cpu });
                        g_keepRedirects++;
                        static bool once = false; if (!once) { once = true; logmsg("frozen frame: the seed blit into %s now samples the kept full-size DLSS frame (seed %p) at frame %u", desc_str(dev, s.rt).c_str(), (void*)g_keepSeed, g_frame); }
                    }
                }
            }
            if (!depthOn && da.count <= 8 && s.table_set[1] && s.rt.handle != g_finalRt[0] && s.rt.handle != g_finalRt[1] && s.rt_w < g_dlssW && feed_layout_now()) {
                resource r0 = resolve_descriptor(dev, s.tables[1], 0);
                if (r0.handle && (r0.handle == g_finalRt[0] || r0.handle == g_finalRt[1] || r0.handle == g_finalSceneRt)) g_feedTexSet.insert(s.rt.handle);
            }
            if (g_layoutDumpFrames && !depthOn && da.count <= 8 && s.table_set[1] && s.rt.handle != g_finalRt[0] && s.rt.handle != g_finalRt[1]) {
                resource r0 = resolve_descriptor(dev, s.tables[1], 0);
                if (r0.handle && is_live(r0.handle)) { resource_desc d0 = dev->get_resource_desc(r0); if (d0.type == resource_type::texture_2d && d0.texture.width * 2 >= g_dlssW && d0.texture.height * 2 >= g_dlssH)
                    logmsg("   f%u copy-like draw into %p %ux%u f%u vp=(%.0f,%.0f %.0fx%.0f) sc=(%d,%d %d,%d) %u verts samples %p %ux%u", g_frame, (void*)s.rt.handle, s.rt_w, s.rt_h, 0u, s.vp.x, s.vp.y, s.vp.width, s.vp.height, s.sc_valid ? (int)s.sc.left : -1, s.sc_valid ? (int)s.sc.top : -1, s.sc_valid ? (int)s.sc.right : -1, s.sc_valid ? (int)s.sc.bottom : -1, da.count, (void*)r0.handle, d0.texture.width, d0.texture.height); }
            }
            if (!depthOn && da.count <= 8 && g_dlssW && s.rt_w < g_dlssW && s.rt.handle != g_finalSceneRt && s.rt.handle != g_finalRt[0] && s.rt.handle != g_finalRt[1]) {
                int idx = -1, param = -1; bool fromFinal = false;
                for (int p = 1; p < 3 && idx < 0; ++p) if (s.table_set[p]) for (int i = 0; i < 4; ++i) {
                    resource r = resolve_descriptor(dev, s.tables[p], i);
                    if (r.handle && (r.handle == g_finalSceneRt || r.handle == g_finalRt[0] || r.handle == g_finalRt[1] || g_seedTex.count(r.handle))) { fromFinal = true; if (r.handle == g_finalSceneRt && g_finalSceneWritten) { idx = i; param = p; break; } }
                }
                if (fromFinal) g_seedTex.insert(s.rt.handle);   // a capture of the frozen image (the pause / Codec seed, the frosted-panel source)
                // the pause / Codec seed is the half-resolution capture (1920x1080 at 4K); cutscenes take other captures of
                // the final texture every few frames (a 2048x2048 one for an effect) and must not move the insertion point
                const bool halfSize = g_dlssW && abs((int)s.rt_w - (int)g_dlssW / 2) <= 2 && abs((int)s.rt_h - (int)g_dlssH / 2) <= 2;
                if (idx >= 0 && halfSize && g_cfgFrozenBg && !g_injectedThisFrame && !g_windowInjectedThisFrame && !g_cfgPrePost && !g_scaling && g_cfgEnabled && g_cfgDebugMode != 2
                    && g_sceneVpFrameValid && is_live(g_finalSceneRt)) {
                    static bool once = false; if (!once) { once = true; logmsg("frozen-screen seed: DLSS on the final texture %s before the game's capture into %s (r%d[%d], count %u) at frame %u", desc_str(dev, resource{ g_finalSceneRt }).c_str(), desc_str(dev, s.rt).c_str(), param, idx, da.count, g_frame); }
                    g_finalPreHudThisFrame = true;   // same role as the pre-HUD insertion: HUD replay and the FG HUD-less image work as there
                    run_dlss(cmd, &s, resource{ g_finalSceneRt }, resource_usage::shader_resource_pixel, 0);
                    g_injectedThisFrame = true; g_finalPreHudInjections++; g_frozenInjections++; g_frozen = true;
                    // keep the full-size DLSS frame for the seed blit (the seed itself is a 1080p downsample of it)
                    if (g_keep.handle && g_out.handle && g_outState == resource_usage::unordered_access && g_cfgDebugMode == 0) {
                        cmd->barrier(g_out, resource_usage::unordered_access, resource_usage::copy_source);
                        if (g_keepState != resource_usage::copy_dest) { cmd->barrier(g_keep, g_keepState, resource_usage::copy_dest); g_keepState = resource_usage::copy_dest; }
                        cmd->copy_resource(g_out, g_keep);
                        cmd->barrier(g_out, resource_usage::copy_source, resource_usage::unordered_access);
                        cmd->barrier(g_keep, resource_usage::copy_dest, resource_usage::shader_resource); g_keepState = resource_usage::shader_resource;
                        g_keepSeed = s.rt.handle; g_keepValid = true;
                    } else g_keepValid = false;
                }
                // Cutscene capture of the final texture (the 2048x2048 one the WIPE transitions slide over the next shot
                // at every cut, and the camera-blur feedback): it samples the final texture BEFORE the pre-HUD insertion,
                // and with PostDof the blur only exists in the DLSS output after that insertion - so every wipe flashed a
                // sharp, DoF-less copy of the previous shot across the screen (~4 frames per cut; the "blur collapses to a
                // corner" flashes). Redirect the capture to the previous frame's DLSS+DoF output: same size, one frame
                // stale - and the wipe shows the previous shot anyway.
                if (idx >= 0 && !halfSize && g_cfgPostDof && g_dofSkipFrame && g_dofSeenThisFrame && !g_injectedThisFrame && !g_windowInjectedThisFrame
                    && !g_cfgPrePost && !g_scaling && g_cfgEnabled && g_cfgDebugMode == 0 && g_out.handle && g_evalCount > 2 && g_dofFrames > 0 && g_dlssOutW && is_live(g_finalSceneRt)) {
                    // g_dofFrames > 0: before the first real re-apply, g_out holds the pre-warm scratch evaluation - never hand that to a capture
                    const resource_desc fd = dev->get_resource_desc(resource{ g_finalSceneRt });
                    if (fd.texture.width == g_dlssOutW && fd.texture.height == g_dlssOutH) {
                        uint64_t cpu = 0, size = 0; D3D12_DESCRIPTOR_HEAP_TYPE type;
                        if (table_to_cpu(dev, s.tables[param], idx, &cpu, &size, &type) && type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) {
                            if (g_outState != resource_usage::shader_resource_pixel) { cmd->barrier(g_out, g_outState, resource_usage::shader_resource_pixel); g_outState = resource_usage::shader_resource_pixel; }
                            g_d3d->CreateShaderResourceView(reinterpret_cast<ID3D12Resource*>(g_out.handle), nullptr, D3D12_CPU_DESCRIPTOR_HANDLE{ (SIZE_T)cpu });
                            g_wipeRedirects++;
                            static uint32_t nlog = 0; if (nlog++ < 4) logmsg("PostDof: capture into %s (r%d[%d]) redirected to the previous DLSS+DoF output at frame %u (the wipe transitions show it)", desc_str(dev, s.rt).c_str(), param, idx, g_frame);
                        }
                    }
                }
            }
            const bool uiCandidate = (!g_injectedThisFrame || g_finalPreHudThisFrame || g_windowInjectedThisFrame) && !depthOn && g_sceneDrawsThisFrame >= 20
                && (s.rt.handle == g_finalRt[0] || s.rt.handle == g_finalRt[1]) && s.rt_w == g_dlssW && s.rt_h == g_dlssH;
            const bool uiReplay = uiCandidate && g_ui.handle && g_uiRtv.handle && (g_cfgFgMode != 0 || g_cfgUiMask) && !g_cfgPrePost;
            if (uiCandidate) {
                auto itd = g_geoRt ? g_depthDrawsPerRt.find(g_geoRt) : g_depthDrawsPerRt.end();
                const uint32_t done = itd != g_depthDrawsPerRt.end() ? itd->second : 0;
                if (tracing()) {
                    std::string texs;
                    for (int p = 1; p < 5; ++p) if (s.table_set[p]) for (int i = 0; i < 8; ++i) { resource r = resolve_descriptor(dev, s.tables[p], i); if (r.handle && is_live(r.handle)) { resource_desc d = dev->get_resource_desc(r); if (d.type == resource_type::texture_2d) { char b[48]; snprintf(b, sizeof(b), " r%d[%d]=%ux%u", p, i, d.texture.width, d.texture.height); texs += b; } } }
                    int winSlot = -1, winParam = -1;
                    for (int p = 1; p < 5 && winSlot < 0; ++p) if (s.table_set[p]) for (int i = 0; i < 8; ++i) { resource r = resolve_descriptor(dev, s.tables[p], i); if (r.handle && (r.handle == g_winGeoRt || r.handle == g_winGeoRtLast)) { winSlot = i; winParam = p; break; } }
                    logmsg("f%u ui-cand rt=%p done=%u/%u count=%u pso=%p vp=(%.0f,%.0f %.0fx%.0f) winRt=%p winSrc=r%d[%d]%s", g_frame, (void*)s.rt.handle, done, g_geoDrawsLast, da.count, (void*)s.pso, s.vp.x, s.vp.y, s.vp.width, s.vp.height, (void*)g_winGeoRt, winParam, winSlot, texs.c_str());
                }
                {
                    // Post-process passes into the final texture are fullscreen triangles/quads (<= 4 vertices) that sample a
                    // scene-sized input or use the scene's (dynamic-resolution) viewport; HUD elements are 6+-vertex quads at
                    // the full viewport. Descriptor slots beyond the ones a HUD shader uses carry stale scene-sized textures,
                    // so a plain "samples something big" test mis-files a third of the HUD draws (and flickers the UI layer).
                    // HUD draws always use the full viewport; anything drawn with the scene's (dynamic-resolution) viewport
                    // is scene-space (tints, vignettes, the upscale) whatever its vertex count.
                    const bool fullVp = s.vp_valid && fabsf(s.vp.width - float(g_dlssW)) < 2.0f && fabsf(s.vp.height - float(g_dlssH)) < 2.0f;
                    // scene-sized input: at least half the frame in both dimensions with the frame's aspect (HUD atlases are
                    // 2048x4096 and the like - large, but not frame-shaped)
                    auto sceneSized = [&](const resource_desc& d) {
                        if (d.type != resource_type::texture_2d || d.texture.width * 2 < g_dlssW || d.texture.height * 2 < g_dlssH) return false;
                        // a depth texture (the port's depth copies / fog passes sample it full-screen) is not scene color: the
                        // pause menu's closing frame samples it into the final texture and looked like a fresh scene write
                        switch (d.texture.format) { case format::r24_g8_typeless: case format::d24_unorm_s8_uint: case format::r32_typeless: case format::d32_float: case format::r16_typeless: case format::d16_unorm: case format::r32_g8_typeless: case format::d32_float_s8_uint: return false; default: break; }
                        const float a = float(d.texture.width) / float(d.texture.height), fa = float(g_dlssW) / float(g_dlssH);
                        return fabsf(a - fa) < 0.2f;
                    };
                    bool big = false, slot0big = false; uint64_t slot0 = 0;
                    for (int p = 1; p < 5 && !big; ++p) if (s.table_set[p]) for (int i = 0; i < 8 && !big; ++i) {
                        resource r = resolve_descriptor(dev, s.tables[p], i);
                        if (r.handle && is_live(r.handle)) { resource_desc d = dev->get_resource_desc(r); if (sceneSized(d)) { big = true; if (p == 1 && i == 0) { slot0big = true; slot0 = r.handle; } } }
                    }
                    // Not HUD: anything with the scene's (dynamic-resolution) viewport; 3/4-vertex fullscreen passes (HUD is
                    // 6-vertex quads and 2-vertex lines); quads whose primary input is a scene-sized texture (the port's
                    // motion feedback quad, drawn into the other final texture).
                    bool post = !fullVp || da.count == 3 || da.count == 4 || (da.count >= 6 && slot0big);
                    if (g_layoutDumpFrames && big && !(post && slot0big && fullVp && da.count <= 4)) logmsg("   f%u draw into %p: %s, scissor (%d,%d %d,%d) vp=(%.0f,%.0f %.0fx%.0f) %u verts, slot0 %p%s", g_frame, (void*)s.rt.handle, post ? "post" : "HUD", s.sc_valid ? (int)s.sc.left : -1, s.sc_valid ? (int)s.sc.top : -1, s.sc_valid ? (int)s.sc.right : -1, s.sc_valid ? (int)s.sc.bottom : -1, s.vp.x, s.vp.y, s.vp.width, s.vp.height, da.count, (void*)slot0, slot0big ? " (scene-sized)" : "");
                    if (post && slot0big && fullVp && da.count <= 4) {
                        // The game's upscale of a scene into the final texture, one per 3D window (the briefing's camera
                        // window has its own, before the main view's): its scissor is the rectangle that window occupies in
                        // the final image (see g_layoutRect), the whole texture unless the scene is a layout window. The
                        // main view's is the one the frame's scene viewport fits - same origin, the window's aspect - and
                        // the tightest if several do.
                        const viewport R = (s.sc_valid && s.sc.right > s.sc.left && s.sc.bottom > s.sc.top) ? viewport{ float(s.sc.left), float(s.sc.top), float(s.sc.right - s.sc.left), float(s.sc.bottom - s.sc.top), 0.0f, 1.0f }
                                                                                                             : viewport{ 0.0f, 0.0f, float(s.rt_w), float(s.rt_h), 0.0f, 1.0f };
                        // this frame's scene viewport only: the camera window's upscale runs before the main view is even
                        // drawn, and matching it against the window viewport of the frame adopted the camera window's rectangle
                        const bool fits = g_sceneVpFrameValid && vp_in_layout(g_sceneVpFrame, R);
                        if (fits && (!g_layoutValid || g_layoutFrame != g_frame || R.width * R.height < g_layoutRect.width * g_layoutRect.height)) {
                            g_layoutRect = R; g_layoutValid = true; g_layoutFrame = g_frame;
                            float c[12]; if (s.cbv_set[2] && read_cbv(s, 2, c, 12)) g_layoutK = c[8];
                        }
                        if (!fits) {
                            // a 3D window's: the scissor fits one of the frame's window viewports (20+ draws) and does not
                            // overlap the main view's rectangle (the video call's caller feed does - it is drawn under it)
                            const viewport* ml = layout_now();
                            if (!(ml && rects_overlap(R, *ml)) && (!g_winLayoutValid || g_winLayoutFrame != g_frame || R.width * R.height < g_winLayoutRect.width * g_winLayoutRect.height))
                                for (auto& kv : g_winVpHist) if (kv.second.first >= 20 && vp_in_layout(kv.second.second, R)) { g_winLayoutRect = R; g_winLayoutVp = kv.second.second; g_winLayoutValid = true; g_winLayoutFrame = g_frame; break; }
                            // the caller feed's: overlaps the main view's rectangle (drawn under it) and fits a window viewport
                            if (ml && layout_is_window(*ml) && rects_overlap(R, *ml) && (!g_feedLayoutValid || g_feedLayoutFrame != g_frame))
                                for (auto& kv : g_winVpHist) if (kv.second.first >= 20 && vp_in_layout(kv.second.second, R)) { g_feedLayoutRect = R; g_feedLayoutVp = kv.second.second; g_feedLayoutValid = true; g_feedLayoutFrame = g_frame; break; }
                        }
                        if (g_layoutDumpFrames) logmsg("   f%u scene write into %p: scissor (%d,%d %d,%d) vp=(%.0f,%.0f %.0fx%.0f) %u verts, samples %p (%s); scene vp %d (%.0f,%.0f %.0fx%.0f) -> %s", g_frame, (void*)s.rt.handle, s.sc_valid ? (int)s.sc.left : -1, s.sc_valid ? (int)s.sc.top : -1, s.sc_valid ? (int)s.sc.right : -1, s.sc_valid ? (int)s.sc.bottom : -1, s.vp.x, s.vp.y, s.vp.width, s.vp.height, da.count, (void*)slot0, desc_str(dev, resource{ slot0 }).c_str(), (int)g_sceneVpFrameValid, g_sceneVpFrame.x, g_sceneVpFrame.y, g_sceneVpFrame.width, g_sceneVpFrame.height, fits ? "fits" : "no");
                    }
                    // The final texture receives the scene (the fullscreen pass at the full viewport that samples a
                    // scene-sized input, i.e. the game's upscale/tonemap) before the HUD; remember which texture, since
                    // the final image is double-buffered and the other one takes scene-space effect draws this frame.
                    if (post && slot0big && fullVp) {
                        g_finalSceneWritten = true; g_finalSceneRt = s.rt.handle; g_finalSceneSrc = slot0;
                        if (slot0 != g_finalRt[0] && slot0 != g_finalRt[1] && g_seedTex.count(slot0) == 0) g_freshWrite = true;   // not the seed, not the other final texture: live content
                    }   // the game's upscale/tonemap, or the frozen-scene blits of the Codec / pause screens
                    const bool preScene = !post && (!g_finalSceneWritten || s.rt.handle != g_finalSceneRt);
                    if (preScene) { post = true; g_uiPreSceneThisFrame++; }
                    if (tracing()) logmsg("f%u    -> %s (fullVp %d, big %d, slot0big %d, sceneRt %p)", g_frame, preScene ? "pre-scene" : (post ? "post" : "HUD"), (int)fullVp, (int)big, (int)slot0big, (void*)g_finalSceneRt);
                    if (post) g_uiPostSkippedThisFrame++;
                    else if (!g_injectedThisFrame && !g_windowInjectedThisFrame && !g_cfgPrePost && !g_scaling && g_cfgEnabled && g_cfgDebugMode != 2) {
                        // Composite mode with a HUD: run DLSS on the final texture now, before its first HUD draw. DLSS (and
                        // any NGX post-processing add-on evaluating inline) then never sees the HUD, the HUD is drawn by the
                        // game on top of the DLSS output, and the DLSS output is the HUD-less color for frame generation.
                        static bool once = false; if (!once) { once = true; logmsg("pre-HUD insertion (final): DLSS on the final texture %s before its first HUD draw (composite skipped)", desc_str(dev, s.rt).c_str()); }
                        t_reentrant = true; cmd->bind_render_targets_and_depth_stencil(0, nullptr, resource_view{ 0 }); t_reentrant = false;
                        g_finalPreHudThisFrame = true;
                        run_dlss(cmd, &s, s.rt, resource_usage::render_target, 0);
                        t_reentrant = true; cmd->bind_render_targets_and_depth_stencil(s.rtv_count, s.rtvs, s.dsv); t_reentrant = false;
                        g_injectedThisFrame = true; g_finalPreHudInjections++;
                    }
                    if (post) {}
                    else if (!uiReplay) g_hudDrawsThisFrame++;   // classification only
                    else {
                        g_hudDrawsThisFrame++;
                        if (g_cfgDebugMode == 7 && g_finalPreHudThisFrame) g_skipThisDraw = true;   // HUD-less view: the HUD stays out of the image
                        else if (g_cfgDebugMode == 7 && g_windowInjectedThisFrame) {
                            // window mode: keep whatever carries the 3D window into the frame (that is the DLSS output), drop the panels
                            bool carriesWindow = false;
                            for (int p = 1; p < 5 && !carriesWindow; ++p) if (s.table_set[p]) for (int i = 0; i < 8 && !carriesWindow; ++i) { resource rr = resolve_descriptor(dev, s.tables[p], i); if (rr.handle && (rr.handle == g_winGeoRt || rr.handle == g_winPostRt)) carriesWindow = true; }
                            if (!carriesWindow) g_skipThisDraw = true;
                        }
                        t_reentrant = true;
                        if (g_uiState != resource_usage::render_target) { cmd->barrier(g_ui, g_uiState, resource_usage::render_target); g_uiState = resource_usage::render_target; }
                        if (!g_uiClearedThisFrame) {
                            const float zero[4] = { 0, 0, 0, 0 }; cmd->clear_render_target_view(g_uiRtv, zero); g_uiClearedThisFrame = true;
                            if (g_preHud.handle && !g_finalPreHudThisFrame) {   // the final texture right before its first HUD draw = post-processed scene without HUD
                                if (g_preHudState != resource_usage::copy_dest) { cmd->barrier(g_preHud, g_preHudState, resource_usage::copy_dest); g_preHudState = resource_usage::copy_dest; }
                                cmd->barrier(s.rt, resource_usage::render_target, resource_usage::copy_source);
                                cmd->copy_resource(s.rt, g_preHud);
                                cmd->barrier(s.rt, resource_usage::copy_source, resource_usage::render_target);
                                g_preHudCaptured = true;
                            }
                        }
                        cmd->bind_render_targets_and_depth_stencil(1, &g_uiRtv, resource_view{ 0 });
                        if (da.indexed) cmd->draw_indexed(da.count, da.instances, da.first, da.vertex_offset, da.first_instance);
                        else cmd->draw(da.count, da.instances, da.first, da.first_instance);
                        cmd->bind_render_targets_and_depth_stencil(s.rtv_count, s.rtvs, s.dsv);
                        t_reentrant = false;
                        g_uiDrawsThisFrame++;
                    }
                }
            }
            if (g_layoutDumpFrames && s.rt.handle) g_dumpRtSet.insert(s.rt.handle);   // diagnostics: every target drawn into this frame
            if (s.rt.handle) g_rtSeen[s.rt.handle] = g_frame;   // every target drawn into: the flashback video is a plain texture, never one of these
            if (tracing()) {   // where does the 3D target go? log RT switches and every draw referencing it
                static uint64_t lastRt = 0; static uint32_t drawsSince = 0;
                std::string refs;
                for (int p = 0; p < 5; ++p) if (s.table_set[p]) for (int i = 0; i < 8; ++i) { resource r = resolve_descriptor(dev, s.tables[p], i); if (r.handle == g_geoRt) refs += " root" + std::to_string(p) + "[" + std::to_string(i) + "]"; }
                if (s.rt.handle != lastRt) {
                    logmsg("f%u RT switch -> %s ds=%p (%u draws into previous) [3D target %p: %u depth draws so far]", g_frame, desc_str(dev, s.rt).c_str(), (void*)s.ds.handle, drawsSince, (void*)g_geoRt, (unsigned)g_depthDrawsPerRt[g_geoRt]);
                    lastRt = s.rt.handle; drawsSince = 0;
                }
                drawsSince++;
                if (!refs.empty()) logmsg("f%u    draw into %p (depth %d, pso %p) samples the 3D target via%s", g_frame, (void*)s.rt.handle, (int)depthOn, (void*)s.pso, refs.c_str());
            }
            // A 3D window (Codec caller, pause-menu model) renders into its own target at a window viewport and is then
            // post-processed (the Codec's CRT/scanline pass) and blitted into the frame. Run DLSS on that target at its
            // first reader: the caller's scene gets DLAA and Neural Rendering, while the CRT overlay and every panel
            // around the window are drawn afterwards and stay out of DLSS entirely.
            // Only on frozen screens: the previous frame must have had no full-frame 3D scene either. A cutscene with a
            // picture-in-picture window (the Mk. II's monitor) reaches this point before its own scene draws have
            // started, and inserting on the window there left the scene itself raw for the frame (flicker + resets).
            // The camera window's depth, at the window's first reader (see g_winDepthRect): the window's rectangle in the
            // image is last frame's (its own scene write comes later in the frame), its viewport this frame's.
            if (g_winFitRt && g_winFitDraws >= 20 && g_winDepthFrame != g_frame && s.rt.handle != g_winFitRt && is_live(g_winFitRt)
                && g_depthFull.handle && g_stretchPso && g_cfgDRS != 2 && g_cfgEnabled && !g_scaling && g_internalW && g_internalH && layout_now()) {
                bool reads = false;
                for (int p = 1; p < 5 && !reads; ++p) if (s.table_set[p]) for (int i = 0; i < 8; ++i) { resource r = resolve_descriptor(dev, s.tables[p], i); if (r.handle == g_winFitRt) { reads = true; break; } }
                if (reads) {
                    const viewport* wvpL = nullptr; const viewport* wl = win_layout_now(&wvpL);
                    auto itDs = g_dsForRt.find(g_winFitRt);
                    const viewport& g_winVpFrame = g_winFitVp;   // the window's own draws' viewport this frame
                    if (wl && itDs != g_dsForRt.end() && is_live(itDs->second) && vp_in_layout(g_winVpFrame, *wl) && wl->width > 0 && wl->height > 0) {
                        const resource ds{ itDs->second }; const resource_desc dd = dev->get_resource_desc(ds);
                        if (dd.type == resource_type::texture_2d && dd.texture.width == g_dlssW && dd.texture.height == g_dlssH) {
                            const float tx = float(g_dlssW) / float(g_internalW), ty = float(g_dlssH) / float(g_internalH);
                            const float wr[4] = { wl->x * tx, wl->y * ty, wl->width * tx, wl->height * ty };
                            const float kwx = g_winVpFrame.width / wl->width, kwy = g_winVpFrame.height / wl->height;
                            const float wo[2] = { g_winVpFrame.x * tx - wr[0] * kwx, g_winVpFrame.y * ty - wr[1] * kwy };
                            cmd->barrier(ds, resource_usage::depth_stencil_write, resource_usage::shader_resource_non_pixel);
                            depth_stretch_dispatch(cmd, ds, dd.texture.format, g_dlssW, g_dlssH, kwx, kwy, wo, wr, false, nullptr);
                            cmd->barrier(ds, resource_usage::shader_resource_non_pixel, resource_usage::depth_stencil_write);
                            restore_state(dev, cmd, s);
                            g_winDepthFrame = g_frame; memcpy(g_winDepthRect, wr, sizeof(wr)); g_winDepthCopies++;
                            static bool once = false; if (!once) { once = true; logmsg("LAYOUT: the camera window (%.0f,%.0f %.0fx%.0f), drawn at (%.0f,%.0f %.0fx%.0f), gets its depth copied to the full grid at its first reader (into %s) and its objects rasterized into it", wl->x, wl->y, wl->width, wl->height, g_winVpFrame.x, g_winVpFrame.y, g_winVpFrame.width, g_winVpFrame.height, desc_str(dev, s.rt).c_str()); }
                        }
                    }
                }
            }
            // The caller feed's depth, at the feed's first reader (see g_feedLayoutRect), into g_feedDepth at the feed's rectangle.
            if (g_feedGeoRt && g_feedGeoDraws >= 20 && g_feedVpFrameValid && g_feedDepthFrame != g_frame && s.rt.handle != g_feedGeoRt && is_live(g_feedGeoRt)
                && g_feedDepth.handle && g_stretchPso && g_cfgDRS != 2 && g_cfgEnabled && !g_scaling && g_internalW && g_internalH && g_cfgObjectMV && g_cfgMonitorProject) {
                bool reads = false;
                for (int p = 1; p < 5 && !reads; ++p) if (s.table_set[p]) for (int i = 0; i < 8; ++i) { resource r = resolve_descriptor(dev, s.tables[p], i); if (r.handle == g_feedGeoRt) { reads = true; break; } }
                if (reads) {
                    const viewport* fl = feed_layout_now();
                    auto itDs = g_dsForRt.find(g_feedGeoRt);
                    if (fl && itDs != g_dsForRt.end() && is_live(itDs->second) && vp_in_layout(g_feedVpFrame, *fl) && fl->width > 0 && fl->height > 0) {
                        const resource ds{ itDs->second }; const resource_desc dd = dev->get_resource_desc(ds);
                        if (dd.type == resource_type::texture_2d && dd.texture.width == g_dlssW && dd.texture.height == g_dlssH) {
                            const float tx = float(g_dlssW) / float(g_internalW), ty = float(g_dlssH) / float(g_internalH);
                            const float fr[4] = { fl->x * tx, fl->y * ty, fl->width * tx, fl->height * ty };
                            const float kfx = g_feedVpFrame.width / fl->width, kfy = g_feedVpFrame.height / fl->height;
                            const float fo[2] = { g_feedVpFrame.x * tx - fr[0] * kfx, g_feedVpFrame.y * ty - fr[1] * kfy };
                            cmd->barrier(ds, resource_usage::depth_stencil_write, resource_usage::shader_resource_non_pixel);
                            depth_stretch_dispatch(cmd, ds, dd.texture.format, g_dlssW, g_dlssH, kfx, kfy, fo, fr, true, nullptr, g_feedDepth, &g_feedDepthState);
                            cmd->barrier(ds, resource_usage::shader_resource_non_pixel, resource_usage::depth_stencil_write);
                            restore_state(dev, cmd, s);
                            g_feedDepthFrame = g_frame; g_feedDepthCopies++;
                            static bool once = false; if (!once) { once = true; logmsg("LAYOUT: the caller feed (%.0f,%.0f %.0fx%.0f), drawn at (%.0f,%.0f %.0fx%.0f) into %p, gets its depth copied at its first reader; its objects' vectors go to the feed texture and reach the screen through the monitor", fl->x, fl->y, fl->width, fl->height, g_feedVpFrame.x, g_feedVpFrame.y, g_feedVpFrame.width, g_feedVpFrame.height, (void*)g_feedGeoRt); }
                        }
                    }
                }
            }
            if (g_cfgWindowScene && !g_injectedThisFrame && g_cfgEnabled && !g_scaling && g_cfgDebugMode != 2
                && g_winGeoRt && g_winGeoDraws >= 20 && g_winVpFrameValid && !g_sceneVpFrameValid && !g_prevFrameHadScene
                && s.rt.handle != g_winGeoRt && is_live(g_winGeoRt)) {
                int idx = -1, param = -1;
                for (int p = 1; p < 5 && idx < 0; ++p) if (s.table_set[p]) for (int i = 0; i < 8; ++i) { resource r = resolve_descriptor(dev, s.tables[p], i); if (r.handle == g_winGeoRt) { idx = i; param = p; break; } }
                if (idx >= 0) {
                    static bool once = false; if (!once) { once = true; logmsg("window insertion: DLSS on the 3D window target %p (%u depth draws, window %.0f,%.0f %.0fx%.0f) at its first reader (r%d[%d], into %s)", (void*)g_winGeoRt, g_winGeoDraws, g_winVpFrame.x, g_winVpFrame.y, g_winVpFrame.width, g_winVpFrame.height, param, idx, desc_str(dev, s.rt).c_str()); }
                    t_reentrant = true; cmd->bind_render_targets_and_depth_stencil(0, nullptr, resource_view{ 0 }); t_reentrant = false;
                    g_windowInjectedThisFrame = true; g_injectedThisFrame = true; g_windowInjections++; g_winPostRt = s.rt.handle;
                    run_dlss(cmd, &s, resource{ g_winGeoRt }, resource_usage::shader_resource_pixel, 0);
                    t_reentrant = true; cmd->bind_render_targets_and_depth_stencil(s.rtv_count, s.rtvs, s.dsv); t_reentrant = false;
                }
            }
            if (g_cfgPrePost && !g_scaling && !g_injectedThisFrame && g_cfgEnabled && g_geoRt && is_live(g_geoRt)) {
                auto itd = g_depthDrawsPerRt.find(g_geoRt);
                const uint32_t done = itd != g_depthDrawsPerRt.end() ? itd->second : 0;
                if (done >= 20 && done * 10 >= g_geoDrawsLast * 8) {
                    if (s.rt.handle == g_geoRt && !depthOn) {
                        g_injectedThisFrame = true; g_prePostInjections++;
                        static bool once = false; if (!once) { once = true; logmsg("pre-HUD insertion (a): 3D target %s after %u depth-tested draws, at a 2D draw into it", desc_str(dev, s.rt).c_str(), done); }
                        t_reentrant = true; cmd->bind_render_targets_and_depth_stencil(0, nullptr, resource_view{ 0 }); t_reentrant = false;
                        run_dlss(cmd, &s, s.rt, resource_usage::render_target, 0);
                        t_reentrant = true; cmd->bind_render_targets_and_depth_stencil(s.rtv_count, s.rtvs, s.dsv); t_reentrant = false;
                    } else if (s.rt.handle != g_geoRt && s.table_set[1]) {
                        int idx = -1;
                        for (int i = 0; i < 8 && idx < 0; ++i) { resource r = resolve_descriptor(dev, s.tables[1], i); if (r.handle == g_geoRt) idx = i; }
                        if (idx >= 0) {
                            g_injectedThisFrame = true; g_prePostInjections++;
                            static bool once = false; if (!once) { once = true; logmsg("pre-HUD insertion (b): 3D target %p (%u depth-tested draws) sampled (slot %d) by a draw into %s", (void*)g_geoRt, done, idx, desc_str(dev, s.rt).c_str()); }
                            run_dlss(cmd, &s, resource{ g_geoRt }, resource_usage::shader_resource_pixel, 0);
                        }
                    }
                }
            }
        }
        return;
    }
    if (g_sceneDrawsThisFrame < 20) return;
    if (!g_traceArmed) {
        g_traceArmed = true; g_traceUntil = g_frame + 3; logmsg("tracing backbuffer draws/copies for frames %u..%u", g_frame, g_traceUntil - 1);
        if (g_dumpPending) { g_dumpPending = false; g_dumpUntil = g_frame + 3; g_dumpDrawsLogged = 0; logmsg("analyzing scene draw constants for frames %u..%u", g_frame + 1, g_dumpUntil - 1); }
    }

    uint32_t drawIdx;
    { std::lock_guard<std::mutex> lock(g_clMutex); drawIdx = g_cl[cmd].bb_draws++; }

    resource color = { 0 }; int colorParam = -1, colorIdx = -1; uint64_t srvCpu = 0;
    for (int p = 0; p < 5 && !color.handle; ++p) {
        if (!s.table_set[p]) continue;
        for (int i = 0; i < 4; ++i) {
            resource r = resolve_descriptor(dev, s.tables[p], i);
            if (scene_sized(dev, r)) {
                color = r; colorParam = p; colorIdx = i;
                uint64_t size; D3D12_DESCRIPTOR_HEAP_TYPE type; table_to_cpu(dev, s.tables[p], i, &srvCpu, &size, &type);
                break;
            }
        }
    }
    if (tracing() && drawIdx < 4)
        logmsg("f%u bb-draw#%u vp=(%.0f,%.0f %.0fx%.0f) -> color=%s (param %d idx %d)", g_frame, drawIdx, s.vp.x, s.vp.y, s.vp.width, s.vp.height, desc_str(dev, color).c_str(), colorParam, colorIdx);
    else if (tracing() && drawIdx < 200) {
        std::string texs;
        for (int p = 0; p < 5; ++p) if (s.table_set[p]) for (int i = 0; i < 4; ++i) { resource r = resolve_descriptor(dev, s.tables[p], i); if (r.handle && is_live(r.handle)) { resource_desc d = dev->get_resource_desc(r); if (d.type == resource_type::texture_2d) { char b[48]; snprintf(b, sizeof(b), " r%d[%d]=%ux%u", p, i, d.texture.width, d.texture.height); texs += b; } } }
        logmsg("f%u bb-draw#%u count=%u depth=%d pso=%p vp=%.0fx%.0f%s", g_frame, drawIdx, da.count, (int)pso_depth_enabled(s.pso), (void*)s.pso, s.vp.width, s.vp.height, texs.c_str());
    }

    if (g_cfgTraceFreeze && drawIdx == 0) frz_record("f%u composite -> backbuffer samples %s (scene draws %u, depth-tested %u, injected %d, HUD draws %u)", g_frame, desc_str(dev, color).c_str(), g_sceneDrawsThisFrame, g_depthOnDrawsThisFrame, (int)g_injectedThisFrame, g_hudDrawsThisFrame);
    if (color.handle && color.handle != g_finalRt[0]) { g_finalRt[1] = g_finalRt[0]; g_finalRt[0] = color.handle; }
    if (color.handle && s.vp.width > 0) { g_gameVp[0] = s.vp.x; g_gameVp[1] = s.vp.y; g_gameVp[2] = s.vp.width; g_gameVp[3] = s.vp.height; }
    if (g_cfgProbe && drawIdx == 0 && color.handle) { probe_dispatch(cmd, color, resource_usage::shader_resource_pixel, 2); restore_state(dev, cmd, s); }
    if (g_fgScalePending && drawIdx == 0) { g_fgScalePending = false; fg_scale_hints(cmd); restore_state(dev, cmd, s); }   // the HUD layer is complete here: rescale the FG hints to the backbuffer size
    if (color.handle && g_sceneVpValid && g_dlssW && (uint32_t)(g_sceneVp.width + 0.5f) < g_dlssW) {   // composite of a DRS sub-rect: where does the scale live?
        static int dumps = 0;
        if (dumps < 3 && (g_frame % 90) == 0) {
            dumps++;
            logmsg("composite draw f%u: scene vp %.0fx%.0f of %ux%u (k=%.4f,%.4f); draw count=%u inst=%u first=%u; vb0=%p+%llu ib=%p+%llu", g_frame, g_sceneVp.width, g_sceneVp.height, g_dlssW, g_dlssH, g_sceneVp.width / g_dlssW, g_sceneVp.height / g_dlssH, da.count, da.instances, da.first, (void*)s.vb0.handle, (unsigned long long)s.vb0_off, (void*)s.ib.handle, (unsigned long long)s.ib_off);
            for (int p = 0; p < 5; ++p) {
                if (!s.cbv_set[p]) continue;
                float c[32] = {}; if (!read_cbv(s, p, c, 32)) continue;
                logmsg("   root[%d] cb: %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f", p,
                       c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8], c[9], c[10], c[11], c[12], c[13], c[14], c[15], c[16], c[17], c[18], c[19], c[20], c[21], c[22], c[23], c[24], c[25], c[26], c[27], c[28], c[29], c[30], c[31]);
            }
            if (s.vb0.handle && is_live(s.vb0.handle)) {   // vertex data of the composite quad (UVs?)
                ID3D12Resource* vb = reinterpret_cast<ID3D12Resource*>(s.vb0.handle);
                D3D12_HEAP_PROPERTIES hp = {}; D3D12_HEAP_FLAGS hf = {};
                if (SUCCEEDED(vb->GetHeapProperties(&hp, &hf)) && hp.Type == D3D12_HEAP_TYPE_UPLOAD) {
                    void* pv = nullptr; D3D12_RANGE rr = { (SIZE_T)s.vb0_off, (SIZE_T)(s.vb0_off + 128) };
                    if (SUCCEEDED(vb->Map(0, &rr, &pv)) && pv) {
                        const float* v = reinterpret_cast<const float*>(static_cast<char*>(pv) + s.vb0_off);
                        logmsg("   vb0 (upload) first 24 floats: %.3f %.3f %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f %.3f %.3f", v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11], v[12], v[13], v[14], v[15], v[16], v[17], v[18], v[19], v[20], v[21], v[22], v[23]);
                        D3D12_RANGE wr = { 0, 0 }; vb->Unmap(0, &wr);
                    }
                } else logmsg("   vb0 heap type %u (not CPU readable)", (unsigned)hp.Type);
            }
        }
    }
    if (g_injectedThisFrame || !g_cfgEnabled || !color.handle) return;
    g_injectedThisFrame = true; g_compositeInjections++;
    run_dlss(cmd, &s, color, resource_usage::shader_resource_pixel, srvCpu);
}
static bool on_draw(command_list* cmd, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi) { g_skipThisDraw = false; handle_draw(cmd, draw_args{ false, vc, ic, fv, fi, 0 }); const bool skip = g_skipThisDraw; g_skipThisDraw = false; return skip; }
static bool on_draw_indexed(command_list* cmd, uint32_t ic, uint32_t inst, uint32_t fi, int32_t vo, uint32_t finst) { g_skipThisDraw = false; handle_draw(cmd, draw_args{ true, ic, inst, fi, finst, vo }); const bool skip = g_skipThisDraw; g_skipThisDraw = false; return skip; }

static uint32_t g_ppMissFrames = 0;   // frames where the geometry target was finished but no draw sampled it (pre-post could not insert)
static void handle_copy(command_list* cmd, resource src, resource dst, const char* what)
{
    if (g_bbW == 0 || fg::inside_streamline()) return;
    device* dev = cmd->get_device();
    if (tracing() && (src.handle == g_geoRt || dst.handle == g_geoRt) && is_live(src.handle) && is_live(dst.handle))
        logmsg("f%u %s %s -> %s (3D target involved)", g_frame, what, desc_str(dev, src).c_str(), desc_str(dev, dst).c_str());
    if (g_cfgTraceFreeze && is_live(src.handle) && is_live(dst.handle)) {
        const resource_desc sd = dev->get_resource_desc(src), ddst = dev->get_resource_desc(dst);
        if ((sd.type == resource_type::texture_2d && sd.texture.width >= 640) || (ddst.type == resource_type::texture_2d && ddst.texture.width >= 640))
            frz_record("f%u %s %s -> %s%s%s", g_frame, what, desc_str(dev, src).c_str(), desc_str(dev, dst).c_str(), "", is_backbuffer(dst) ? " (backbuffer)" : "");
    }
    if (!is_backbuffer(dst)) return;
    if (tracing()) logmsg("f%u %s src=%s -> backbuffer (scene draws so far %u)", g_frame, what, desc_str(dev, src).c_str(), g_sceneDrawsThisFrame);
    if (g_injectedThisFrame || !g_cfgEnabled || g_sceneDrawsThisFrame < 20 || !scene_sized(dev, src) || g_scaling) return;
    g_injectedThisFrame = true;
    cl_state s; { std::lock_guard<std::mutex> lock(g_clMutex); s = g_cl[cmd]; }
    run_dlss(cmd, &s, src, resource_usage::copy_source, 0);
}
// Diagnostics (layout dump frames): clears of depth textures, with their rectangles - whether the views of a briefing
// frame keep or wipe each other's depth in the shared depth texture.
static bool on_clear_dsv(command_list* cmd, resource_view dsv, const float* depth, const uint8_t* stencil, uint32_t rect_count, const rect* rects)
{
    if (g_layoutDumpFrames && cmd) {
        device* dev = cmd->get_device(); const resource r = dev->get_resource_from_view(dsv);
        std::string t; char b[64]; for (uint32_t i = 0; i < rect_count && i < 4; ++i) { snprintf(b, sizeof b, " (%d,%d %d,%d)", rects[i].left, rects[i].top, rects[i].right, rects[i].bottom); t += b; }
        static uint32_t n = 0; if (n++ < 400) logmsg("   f%u clear depth %p%s depth %.3f%s", g_frame, (void*)r.handle, depth ? "" : " (stencil only)", depth ? *depth : -1.0f, rect_count ? t.c_str() : " (whole)");
    }
    return false;
}
static bool on_copy_resource(command_list* cmd, resource src, resource dst) { handle_copy(cmd, src, dst, "copy_resource"); return false; }
static bool on_copy_texture_region(command_list* cmd, resource src, uint32_t, const subresource_box*, resource dst, uint32_t, const subresource_box*, filter_mode) { handle_copy(cmd, src, dst, "copy_texture_region"); return false; }

// The DebugKey setting as a virtual key: F1..F12 by name, a single letter or digit, a hex code (0x79), or nothing
// ("", "none", "0").
static unsigned parse_key(const char* k)
{
    if (!k || !*k || _stricmp(k, "none") == 0 || strcmp(k, "0") == 0) return 0;
    if ((k[0] == 'F' || k[0] == 'f') && k[1] >= '0' && k[1] <= '9') { int n = atoi(k + 1); return (n >= 1 && n <= 24) ? VK_F1 + (n - 1) : 0; }
    if (k[0] == '0' && (k[1] == 'x' || k[1] == 'X')) { unsigned v = (unsigned)strtoul(k, nullptr, 16); return v < 256 ? v : 0; }
    if (!k[1]) { unsigned char c = (unsigned char)toupper((unsigned char)k[0]); return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ? c : 0; }
    static const struct { const char* name; unsigned vk; } named[] = {
        { "pause", VK_PAUSE }, { "scroll", VK_SCROLL }, { "insert", VK_INSERT }, { "home", VK_HOME }, { "end", VK_END },
        { "pgup", VK_PRIOR }, { "pgdn", VK_NEXT }, { "delete", VK_DELETE }, { "backspace", VK_BACK },
    };
    for (const auto& n : named) if (_stricmp(k, n.name) == 0) return n.vk;
    return 0;
}

// The pause: the focus-loss messages to the game's window, or the activation ones back, off this thread. A blocked
// SendMessage would hold whichever thread pressed the key (the present thread, or DLSS-G's) against the window's
// thread, which may be waiting on it; a thread of its own with a timeout never does.
struct PauseJob { HWND hwnd; bool on; int mask; };
static DWORD WINAPI world_pause_thread(LPVOID p)
{
    PauseJob job = *reinterpret_cast<PauseJob*>(p); delete reinterpret_cast<PauseJob*>(p);
    DWORD_PTR res = 0;
    if (job.mask & 1) SendMessageTimeoutW(job.hwnd, WM_ACTIVATEAPP, job.on ? FALSE : TRUE, 0, SMTO_ABORTIFHUNG | SMTO_NORMAL, 3000, &res);
    if (job.mask & 2) SendMessageTimeoutW(job.hwnd, WM_ACTIVATE, job.on ? WA_INACTIVE : WA_ACTIVE, 0, SMTO_ABORTIFHUNG | SMTO_NORMAL, 3000, &res);
    if (job.mask & 4) SendMessageTimeoutW(job.hwnd, job.on ? WM_KILLFOCUS : WM_SETFOCUS, 0, 0, SMTO_ABORTIFHUNG | SMTO_NORMAL, 3000, &res);
    return 0;
}
static void world_pause(bool on)
{
    if (!g_hwnd) { logmsg("pause: no game window known yet"); return; }
    g_worldPaused = on; ++g_pauseToggles;
    logmsg("world %s at frame %u: %s to window %p (PauseMessages=%d)", on ? "PAUSED" : "resumed", g_frame,
           on ? "focus-loss messages" : "activation messages", (void*)g_hwnd, g_cfgPauseMessages);
    PauseJob* job = new PauseJob{ g_hwnd, on, g_cfgPauseMessages };
    HANDLE t = CreateThread(nullptr, 0, world_pause_thread, job, 0, nullptr);
    if (t) CloseHandle(t); else { delete job; g_worldPaused = !on; logmsg("pause: could not start the thread"); }
}

// Which of the CompositeIfLoaded modules (RenoDX's DLSS add-on by default) is in the process, if any.
static void nr_detect()
{
    char list[512] = ""; GetPrivateProfileStringA("DLSS", "CompositeIfLoaded", "renodx-dlss.addon64,renodx-dlss5.addon64", list, sizeof(list), g_iniPath);
    g_nrAddonLoaded = false; g_nrAddonName[0] = 0;
    for (char* tok = strtok(list, ";,"); tok; tok = strtok(nullptr, ";,")) {
        while (*tok == ' ') ++tok;
        if (*tok && GetModuleHandleA(tok) != nullptr) { g_nrAddonLoaded = true; strncpy_s(g_nrAddonName, tok, _TRUNCATE); break; }
    }
}
// The NrKick (see g_cfgNrKick): one WARP device, created through ReShade's D3D12CreateDevice hook once the game's
// device and swapchain exist, so that init_device reaches RenoDX with its settings loaded. Runs from on_present
// until it has happened, before the pre-warm evaluations, so the first NR pass is on the first scene frame.
static void nr_kick()
{
    if (!g_cfgNrKick || g_nrKickDone || !g_bbW || !g_d3d) return;
    if (!g_nrAddonLoaded) {
        if (g_frame < 600) { nr_detect(); if (g_nrAddonLoaded) reload_config(); }   // seen now: the insertion point and NrPreload follow at once
        else g_nrKickDone = true;   // no NGX add-on in the process this run: stop looking
        if (!g_nrAddonLoaded) return;
    }
    g_nrKickDone = true;
    IDXGIFactory4* factory = nullptr; IDXGIAdapter* warp = nullptr; ID3D12Device* dev = nullptr;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr = factory->EnumWarpAdapter(IID_PPV_ARGS(&warp));
    if (SUCCEEDED(hr)) { g_nrKickInProgress = true; hr = D3D12CreateDevice(warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)); g_nrKickInProgress = false; }
    if (SUCCEEDED(hr)) { g_nrKickDevice = dev; logmsg("NrKick: WARP D3D12 device created (%p) at frame %u so ReShade raises init_device again and %s attaches its NR runtime (NrKick=0 in the ini turns this off)", (void*)dev, g_frame, g_nrAddonName); }
    else logmsg("NrKick: WARP device creation failed (0x%08X) - %s attaches its NR runtime only from its settings tab", (unsigned)hr, g_nrAddonName);
    if (warp) warp->Release();
    if (factory) factory->Release();
}
static void reload_config()
{
    g_cfgEnabled = GetPrivateProfileIntA("DLSS", "Enabled", 1, g_iniPath);
    g_cfgSharpness100 = GetPrivateProfileIntA("DLSS", "Sharpness", 0, g_iniPath);
    g_cfgDebugMode = GetPrivateProfileIntA("DLSS", "DebugMode", 0, g_iniPath);
    { char k[32] = ""; GetPrivateProfileStringA("DLSS", "DebugKey", "", k, sizeof(k), g_iniPath); g_cfgDebugKey = parse_key(k); }
    { char k[32] = ""; GetPrivateProfileStringA("DLSS", "PauseKey", "Pause", k, sizeof(k), g_iniPath); g_cfgPauseKey = parse_key(k); }
    g_cfgPauseMessages = GetPrivateProfileIntA("DLSS", "PauseMessages", 7, g_iniPath);
    g_cfgDebugKeyMode = GetPrivateProfileIntA("DLSS", "DebugKeyMode", 9, g_iniPath);
    g_cfgJitter = GetPrivateProfileIntA("DLSS", "Jitter", 1, g_iniPath);
    g_cfgDynMask = GetPrivateProfileIntA("DLSS", "DynamicMask", 0, g_iniPath);
    g_cfgUiMask = GetPrivateProfileIntA("DLSS", "UIMask", 1, g_iniPath);
    g_cfgWindowScene = GetPrivateProfileIntA("DLSS", "WindowScene", 1, g_iniPath);
    g_cfgMonitorFlipV = GetPrivateProfileIntA("DLSS", "MonitorFlipV", 0, g_iniPath);
    g_cfgMonitorProject = GetPrivateProfileIntA("DLSS", "MonitorProject", 0, g_iniPath);
    g_cfgFrozenBg = GetPrivateProfileIntA("DLSS", "FrozenBackground", 1, g_iniPath);
    g_cfgTraceFreeze = GetPrivateProfileIntA("DLSS", "TraceFreeze", 0, g_iniPath);
    g_cfgFileTrace = GetPrivateProfileIntA("DLSS", "FileTrace", 0, g_iniPath);
    g_cfgAssetTrace = GetPrivateProfileIntA("DLSS", "AssetTrace", 0, g_iniPath);
    g_cfgDynMaskProps = GetPrivateProfileIntA("DLSS", "DynamicMaskProps", 0, g_iniPath);
    g_cfgDynZeroMV = GetPrivateProfileIntA("DLSS", "DynamicZeroMV", 0, g_iniPath);
    g_cfgMotionVectors = GetPrivateProfileIntA("DLSS", "MotionVectors", 1, g_iniPath);
    {
        char pp[16] = "auto"; GetPrivateProfileStringA("DLSS", "PrePost", "auto", pp, sizeof(pp), g_iniPath);
        g_cfgPrePostMode = (_stricmp(pp, "auto") == 0 || strcmp(pp, "-1") == 0) ? -1 : (atoi(pp) != 0 ? 1 : 0);
        // Add-ons that post-process DLSS's output (RenoDX's DLSS 5 Neural Rendering add-on is the known one: shipped
        // as renodx-dlss.addon64 since its September 2026 builds, renodx-dlss5.addon64 before) need the final image,
        // so when one of the listed modules is loaded we insert at the composite. Without any, pre-post gives the
        // cleanest AA (vignette/HUD outside DLSS). Extend the list in the ini for other tools.
        nr_detect();
        g_cfgNrPreload = GetPrivateProfileIntA("DLSS", "NrPreload", 1, g_iniPath);
        if (g_nrAddonLoaded && g_cfgNrPreload && !g_nrPreloadDone) {
            g_nrPreloadDone = true;
            wchar_t path[MAX_PATH]; swprintf_s(path, L"%s\\nvngx_dlssnr.dll", g_gameDirW);
            if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) logmsg("NrPreload: no nvngx_dlssnr.dll next to mgs4.exe - nothing to load for %s", g_nrAddonName);
            else if (GetModuleHandleW(L"nvngx_dlssnr.dll")) logmsg("NrPreload: nvngx_dlssnr.dll is already in the process");
            else { HMODULE h = LoadLibraryW(path); logmsg("NrPreload: nvngx_dlssnr.dll %s for %s (NrPreload=0 in the ini turns this off)", h ? "loaded" : "failed to load", g_nrAddonName); }
        }
        g_cfgNrKick = GetPrivateProfileIntA("DLSS", "NrKick", 1, g_iniPath);
        const int eff = g_cfgPrePostMode < 0 ? (g_nrAddonLoaded ? 0 : 1) : g_cfgPrePostMode;
        if (eff != g_cfgPrePost) logmsg("insertion: %s (PrePost=%s, DLSS post-processing add-on %s)", eff ? "pre-post (before post-process/HUD)" : "composite (final image)", g_cfgPrePostMode < 0 ? "auto" : (g_cfgPrePostMode ? "1" : "0"), g_nrAddonLoaded ? g_nrAddonName : "not loaded");
        g_cfgPrePost = eff;
    }
    g_cfgDynMask = GetPrivateProfileIntA("DLSS", "DynamicMask", 0, g_iniPath);
    g_cfgUiMask = GetPrivateProfileIntA("DLSS", "UIMask", 1, g_iniPath);
    g_cfgWindowScene = GetPrivateProfileIntA("DLSS", "WindowScene", 1, g_iniPath);
    g_cfgMonitorFlipV = GetPrivateProfileIntA("DLSS", "MonitorFlipV", 0, g_iniPath);
    g_cfgMonitorProject = GetPrivateProfileIntA("DLSS", "MonitorProject", 0, g_iniPath);
    g_cfgDynMaskProps = GetPrivateProfileIntA("DLSS", "DynamicMaskProps", 0, g_iniPath);
    g_cfgDynZeroMV = GetPrivateProfileIntA("DLSS", "DynamicZeroMV", 0, g_iniPath);
    {
        char fps[32] = "0"; GetPrivateProfileStringA("DLSS", "FGTargetFps", "0", fps, sizeof(fps), g_iniPath);
        const int mode = GetPrivateProfileIntA("DLSS", "FrameGen", 0, g_iniPath), reflex = GetPrivateProfileIntA("DLSS", "Reflex", 1, g_iniPath);
        const float target = (float)atof(fps);
        static bool applied = false;
        if (!applied || mode != g_cfgFgMode || reflex != g_cfgReflex || target != g_cfgFgTargetFps) {
            applied = true;
            g_cfgFgMode = mode < 0 ? 0 : (mode > 4 ? 4 : mode); g_cfgReflex = reflex < 0 ? 0 : (reflex > 2 ? 2 : reflex); g_cfgFgTargetFps = target < 0 ? 0 : target;
            fg::Settings fs; fs.mode = g_cfgFgMode; fs.targetFps = g_cfgFgTargetFps; fs.reflex = g_cfgReflex; fg::set_settings(fs);
            logmsg("frame generation: %s, target fps %.0f, Reflex %d", g_cfgFgMode == 0 ? "off" : (g_cfgFgMode == 4 ? "dynamic" : (g_cfgFgMode == 1 ? "2x" : (g_cfgFgMode == 2 ? "3x" : "4x"))), g_cfgFgTargetFps, g_cfgReflex);
        }
    }
    g_cfgObjectMV = GetPrivateProfileIntA("DLSS", "ObjectMV", 1, g_iniPath);
    g_cfgObjMvProps = GetPrivateProfileIntA("DLSS", "ObjectMVProps", 0, g_iniPath);
    { const int lim = GetPrivateProfileIntA("DLSS", "CutPosLimit", 6000, g_iniPath); g_cfgCutPosLimit = lim > 0 ? float(lim) : 6000.0f; }
    g_cfgObjMvMaxPixels = GetPrivateProfileIntA("DLSS", "ObjectMVMaxPixels", 200, g_iniPath); g_cfgObjMvMaxGradient = GetPrivateProfileIntA("DLSS", "ObjectMVMaxGradient", 4, g_iniPath);
    objmv::set_limits(float(g_cfgObjMvMaxPixels), float(g_cfgObjMvMaxGradient));
    g_cfgFgHintRescale = GetPrivateProfileIntA("DLSS", "FGHintRescale", 0, g_iniPath);
    g_cfgDRS = GetPrivateProfileIntA("DLSS", "DRS", 1, g_iniPath);
    g_cfgSceneLog = GetPrivateProfileIntA("DLSS", "SceneLog", 1, g_iniPath);
    {
        const int pd = GetPrivateProfileIntA("DLSS", "PostDof", 0, g_iniPath);
        if (pd != g_cfgPostDof) { g_cfgPostDof = pd; logmsg("PostDof -> %d (%s)", pd, pd ? "the game's DoF draws are skipped and the DoF re-applied on the DLSS output" : "the game's DoF stays in its post chain"); }
        char v[32]; GetPrivateProfileStringA("DLSS", "DofStep", "2.0", v, sizeof(v), g_iniPath); g_cfgDofStep = (float)atof(v);
        GetPrivateProfileStringA("DLSS", "DofRadius", "1.0", v, sizeof(v), g_iniPath); g_cfgDofRadius = (float)atof(v);
        g_cfgDofMask = GetPrivateProfileIntA("DLSS", "DofMask", 1, g_iniPath);
        g_cfgDofJitterSign = GetPrivateProfileIntA("DLSS", "DofJitterSign", 1, g_iniPath);
        g_cfgDofSubRect = GetPrivateProfileIntA("DLSS", "DofSubRect", 1, g_iniPath);
        g_cfgPreWarm = GetPrivateProfileIntA("DLSS", "PreWarm", 1, g_iniPath);
        g_cfgProbe = GetPrivateProfileIntA("DLSS", "Probe", 0, g_iniPath);
        g_cfgDofStepFreeze = GetPrivateProfileIntA("DLSS", "DofStepFreeze", 1, g_iniPath);
        {   // TraceFrames=N (live): trace every full-frame draw for the next N frames (with TraceFreeze=1 the lines go to the log)
            static int lastTf = 0; const int tf = GetPrivateProfileIntA("DLSS", "TraceFrames", 0, g_iniPath);
            if (tf != lastTf) { lastTf = tf; if (tf > 0) { g_traceUntil = g_frame + (uint32_t)tf; logmsg("tracing the next %d frames (%u..%u)", tf, g_frame, g_traceUntil - 1); } }
        }
    }
    {   // DumpShaders=1: write every pipeline's VS/PS bytecode to logs\shaders\<hash>.{vs,ps}.dxbc (post-pass identification)
        static int last = -1; const int dump = GetPrivateProfileIntA("DLSS", "DumpShaders", 0, g_iniPath);
        if (dump != last) { last = dump; char dir[MAX_PATH]; snprintf(dir, MAX_PATH, "%s\\logs\\shaders", g_gameDir); objmv::set_shader_dump_dir(dump ? dir : ""); logmsg("shader dump %s", dump ? dir : "off"); }
    }
    g_cfgHudMin = GetPrivateProfileIntA("DLSS", "HudMinDraws", 25, g_iniPath);
    g_cfgJitterSignX = GetPrivateProfileIntA("DLSS", "JitterSignX", 1, g_iniPath) < 0 ? -1.0f : 1.0f;
    g_cfgJitterSignY = GetPrivateProfileIntA("DLSS", "JitterSignY", -1, g_iniPath) < 0 ? -1.0f : 1.0f;
    if (g_cfgDebugMode != g_cfgLastDebugMode) {
        logmsg("DebugMode -> %d", g_cfgDebugMode); g_cfgLastDebugMode = g_cfgDebugMode;
        if (g_cfgDebugMode == 3) { g_traceUntil = g_frame + 3; logmsg("tracing backbuffer draws/copies for frames %u..%u", g_frame, g_traceUntil - 1); }
        if (g_cfgDebugMode == 4) { if (g_traceArmed) { g_dumpUntil = g_frame + 2; g_dumpDrawsLogged = 0; logmsg("analyzing scene draw constants for frames %u..%u", g_frame, g_dumpUntil - 1); } else g_dumpPending = true; }
    }
}

// End-of-frame bookkeeping. Runs on the game's Present: from ReShade's present event normally, or from the
// Streamline proxy swapchain's Present hook when frame generation is set up (then ReShade's present event fires on
// Streamline's present thread, for generated frames too, and must not touch the per-frame state).
static std::vector<float> g_frameMs;   // present-to-present intervals of the last stats window (pacing diagnostics)
static void frame_rollover()
{
    fg::poll();
    g_frame++;
    {
        static LARGE_INTEGER freq = {}, last = {}; LARGE_INTEGER now; QueryPerformanceCounter(&now);
        if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
        if (last.QuadPart) { const float ms = float(double(now.QuadPart - last.QuadPart) * 1000.0 / double(freq.QuadPart)); g_frameMs.push_back(ms); g_lastFrameDeltaMs = (ms > 0.0f && ms < 500.0f) ? ms : 0.0f; }
        last = now;
        if (g_frameMs.size() >= 600) {
            std::vector<float> v = g_frameMs; std::sort(v.begin(), v.end());
            const float med = v[v.size() / 2], p5 = v[v.size() / 20], p95 = v[v.size() * 19 / 20];
            uint32_t out = 0; for (float x : g_frameMs) if (x > med * 1.35f || x < med * 0.65f) out++;
            logmsg("frame pacing: %zu frames, median %.2f ms, p5 %.2f, p95 %.2f, min %.2f, max %.2f, outliers (>35%% off) %u", g_frameMs.size(), med, p5, p95, v.front(), v.back(), out);
            g_frameMs.clear();
        }
    }
    if (g_cfgTraceFreeze) {
        frz_record("f%u end: scene draws %u, depth-tested %u, fullVp %d (%.0f,%.0f %.0fx%.0f), winVp %d (%.0f,%.0f %.0fx%.0f), HUD %u, insertion preHUD %d window %d any %d, finals %p/%p, 3D target %p (%u)",
                   g_frame - 1, g_sceneDrawsThisFrame, g_depthOnDrawsThisFrame, (int)g_sceneVpFrameValid, g_sceneVpFrame.x, g_sceneVpFrame.y, g_sceneVpFrame.width, g_sceneVpFrame.height, (int)g_winVpFrameValid, g_winVpFrame.x, g_winVpFrame.y, g_winVpFrame.width, g_winVpFrame.height,
                   g_hudDrawsThisFrame, (int)g_finalPreHudThisFrame, (int)g_windowInjectedThisFrame, (int)g_injectedThisFrame, (void*)g_finalRt[0], (void*)g_finalRt[1], (void*)g_curGeoRt, g_geoDrawsLast);
        if (g_prevFrameHadScene && !g_sceneVpFrameValid && g_frame > g_freezeTracedAt + 300 && !g_frzDumping) {
            g_freezeTracedAt = g_frame; g_frzFreezes++;
            std::deque<frz_line> ring; { std::lock_guard<std::mutex> lock(g_frzMutex); ring.swap(g_frzRing); }
            logmsg("=== world stopped rendering at frame %u (freeze #%u): last frames' full-size chain follows (%zu lines), then the next 6 non-empty frames ===", g_frame - 1, g_frzFreezes, ring.size());
            for (const frz_line& l : ring) logmsg("FRZ %s", l.text.c_str());
            g_frzDumping = true; g_frzTraceFrames = 6;
        } else if (g_frzDumping) {
            if (g_sceneDrawsThisFrame >= 20 && --g_frzTraceFrames == 0) { g_frzDumping = false; logmsg("=== freeze trace #%u done ===", g_frzFreezes); }
        }
        std::lock_guard<std::mutex> lock(g_frzMutex);
        while (!g_frzRing.empty() && g_frzRing.front().frame + 3 < g_frame) g_frzRing.pop_front();   // keep the last 3 frames
    }
    if (g_oldFeature && g_frame > g_oldFeatureFrame + 6) {
        NVSDK_NGX_Result r = NVSDK_NGX_D3D12_ReleaseFeature(g_oldFeature); g_oldFeature = nullptr;
        logmsg("released previous DLSS feature -> %s", ngx_str(r));
    }
    if (tracing()) { logmsg("f%u: %u shader-view creations, %u descriptor copies this frame", g_frame - 1, g_viewEvents, g_copyEvents); g_viewSamples = 0; }
    if (dumping() && !g_blockHist.empty()) report_blocks();
    g_viewEvents = 0; g_copyEvents = 0;
    if (g_cfgPrePost && !g_scaling && g_injectedThisFrame && g_prevBusiestRt) { static uint32_t lastPP = 0; if (g_prePostInjections == lastPP) g_ppMissFrames++; lastPP = g_prePostInjections; }
    g_dofPrevOk = g_finalPreHudThisFrame && !g_windowInjectedThisFrame && g_cfgDebugMode != 5 && g_cfgDebugMode != 6 && g_cfgDebugMode != 7 && g_cfgDebugMode != 9;
    g_preHudLast = g_finalPreHudThisFrame;   // (before the reset: the flashback rule needs to know the insertion runs)
    g_injectedThisFrame = false; g_finalPreHudThisFrame = false; g_windowInjectedThisFrame = false; g_winPostRt = 0; g_featureCreatedThisFrame = false; g_fgScalePending = false;
    g_sceneDrawsLast = g_sceneDrawsThisFrame; g_sceneDrawsThisFrame = 0;
    g_dynDrawsLastFrame = g_dynDrawsThisFrame; g_dynDrawsThisFrame = 0; g_dynClearedThisFrame = false;
    if (g_probeReady) { const uint32_t nxt = (g_probeSlot + 1) % 8; for (int st = 0; st < 4; ++st) probe_analyze(st, nxt); g_probeSlot = nxt; g_probeFrames++; }
    g_frameKStep = false; g_noSceneFrames = g_depthOnDrawsThisFrame < 20 ? g_noSceneFrames + 1 : 0; g_dofSeenThisFrame = false; g_dofCocSeen = false; g_dofSkipFrame = false; g_dofCocReadyThisFrame = false; g_dofCombineSeen = false; g_dofMaskCleared = false; g_dofOverlaysThisFrame = 0; g_uiDrawsLast = g_uiDrawsThisFrame; g_uiDrawsThisFrame = 0; g_uiPostSkippedLast = g_uiPostSkippedThisFrame; g_uiPostSkippedThisFrame = 0; g_uiClearedThisFrame = false; g_finalSceneWritten = false; g_finalSceneRt = 0; g_finalSceneSrc = 0; g_freshWrite = false; g_uiPreSceneThisFrame = 0; g_preHudCaptured = false;
    // scene state: 0 = in-game cutscene (3D, no HUD), 1 = gameplay (3D + HUD), 2 = no 3D scene (menu / loading / video)
    if (g_cfgSceneLog) {
        const int raw = (g_sceneDrawsLast < 20) ? 2 : (g_hudDrawsLast >= (uint32_t)g_cfgHudMin ? 1 : 0);
        if (raw != g_sceneStateRaw) { g_sceneStateRaw = raw; g_sceneStateFrames = 0; }
        // periodic heartbeat so the classifier can be diagnosed from the log even when it never changes state
        if ((g_frame % 600) == 0)
            logmsg("SCENE-STATE-TICK %s (frame %u, scene draws %u, HUD draws %u, post-skipped %u, geoRt %p, finalRt %p, rt %ux%u vs dlss %ux%u)",
                   raw == 2 ? "no-3d" : (raw == 1 ? "gameplay" : "cutscene"), g_frame, g_sceneDrawsLast, g_hudDrawsLast, g_uiPostSkippedLast,
                   (void*)g_geoRt, (void*)g_finalRt[0], g_internalW, g_internalH, g_dlssW, g_dlssH);
        if (++g_sceneStateFrames == 30 && raw != g_sceneState) {   // ~0.5 s of the same reading
            static const char* names[3] = { "cutscene", "gameplay", "no-3d" };
            logmsg("SCENE-STATE %s (frame %u, scene draws %u, HUD draws %u, viewport %.0fx%.0f)", names[raw], g_frame, g_sceneDrawsLast, g_hudDrawsLast, g_sceneVp.width, g_sceneVp.height);
            g_sceneState = raw;
        }
    }
    g_hudDrawsLast = g_hudDrawsThisFrame; g_hudDrawsThisFrame = 0; g_depthOnDrawsLast = g_depthOnDrawsThisFrame; g_depthOnDrawsThisFrame = 0;
    objmv::new_frame(g_frame);
    if (g_sceneVpFrameValid) {
        // hysteresis: a new sub-rect is only adopted after the same reading has held for several frames, so a
        // one-off odd viewport can never shrink the region DLSS (and anything layered on it) processes
        static uint32_t stable = 0; static viewport pending = {};
        if (fabsf(pending.width - g_sceneVpFrame.width) < 2.0f && fabsf(pending.height - g_sceneVpFrame.height) < 2.0f) ++stable;
        else { pending = g_sceneVpFrame; stable = 0; }
        if (stable >= 20 && (fabsf(g_sceneVp.width - pending.width) >= 2.0f || fabsf(g_sceneVp.height - pending.height) >= 2.0f)) {
            if (g_sceneVpValid) logmsg("scene viewport changed: %.0fx%.0f -> %.0fx%.0f (stable for %u frames)", g_sceneVp.width, g_sceneVp.height, pending.width, pending.height, stable);
            g_sceneVp = pending; g_sceneVpValid = true;
        } else if (!g_sceneVpValid && stable >= 20) { g_sceneVp = pending; g_sceneVpValid = true; }
    }
    // the layout rectangle seen at this frame's scene write serves the next frame's scene draws (jitter) until its own
    if (g_layoutValid && g_layoutFrame == g_frame - 1) { g_layoutLast = g_layoutRect; g_layoutLastValid = true; }
    if (g_winLayoutValid && g_winLayoutFrame == g_frame - 1) { g_winLayoutLast = g_winLayoutRect; g_winLayoutLastVp = g_winLayoutVp; g_winLayoutLastValid = true; g_winLayoutLastFrame = g_frame - 1; }
    if (g_feedLayoutValid && g_feedLayoutFrame == g_frame - 1) { g_feedLayoutLast = g_feedLayoutRect; g_feedLayoutLastVp = g_feedLayoutVp; g_feedLayoutLastValid = true; g_feedLayoutLastFrame = g_frame - 1; }
    g_feedDrawsPerRt.clear(); g_feedGeoRt = 0; g_feedGeoDraws = 0; g_feedVpFrameValid = false; g_feedTexSet.clear();
    g_winFitDrawsPerRt.clear(); g_winFitRt = 0; g_winFitDraws = 0;
    if (g_layoutDumpFrames) {   // diagnostics after a layout change: the frame's viewport candidates and targets
        g_layoutDumpFrames--; std::string t; char b[128];
        for (auto& kv : g_vpHist) { snprintf(b, sizeof b, " scene(%.0f,%.0f %.0fx%.0f)x%u", kv.second.second.x, kv.second.second.y, kv.second.second.width, kv.second.second.height, kv.second.first); t += b; }
        for (auto& kv : g_winVpHist) { snprintf(b, sizeof b, " win(%.0f,%.0f %.0fx%.0f)x%u", kv.second.second.x, kv.second.second.y, kv.second.second.width, kv.second.second.height, kv.second.first); t += b; }
        for (auto& kv : g_sceneClassDrawsPerRt) { snprintf(b, sizeof b, " rt %p: %u scene-class", (void*)kv.first, kv.second); t += b; }
        logmsg("   f%u viewports:%s; 3D target %p, window target %p (%u), layout %d (%.0f,%.0f %.0fx%.0f)", g_frame - 1, t.c_str(), (void*)g_curGeoRt, (void*)g_winGeoRt, g_winGeoDraws, (int)(g_layoutValid && g_layoutFrame == g_frame - 1), g_layoutRect.x, g_layoutRect.y, g_layoutRect.width, g_layoutRect.height);
        t.clear();
        for (auto& kv : g_dumpVpHist) if (kv.second.first >= 3) { snprintf(b, sizeof b, " %p@(%.0f,%.0f %.0fx%.0f)x%u", (void*)kv.second.second.first, kv.second.second.second.x, kv.second.second.second.y, kv.second.second.second.width, kv.second.second.second.height, kv.second.first); t += b; }
        logmsg("   f%u depth-tested draws per target and viewport:%s", g_frame - 1, t.c_str());
        t.clear();
        for (auto& kv : g_dumpTexHist) { snprintf(b, sizeof b, " %p %ux%u f%u x%u", (void*)kv.second.second.first, kv.second.second.second.texture.width, kv.second.second.second.texture.height, (unsigned)kv.second.second.second.texture.format, kv.second.first); t += b; }
        logmsg("   f%u textures (512+) sampled by the main view's draws:%s", g_frame - 1, t.c_str());
    }
    g_dumpTexHist.clear(); if (!g_layoutDumpFrames) g_dumpRtSet.clear();
    if (g_rtSeen.size() > 4096) { for (auto it = g_rtSeen.begin(); it != g_rtSeen.end();) { if (g_frame - it->second > 600) it = g_rtSeen.erase(it); else ++it; } }
    g_dumpVpHist.clear();
    if (layout_window() && g_frame % 900 == 0 && g_layoutDumps < 40) { g_layoutDumps++; g_layoutDumpFrames = 1; }   // a periodic dump while a layout window is up
    g_sceneClassDrawsPerRt.clear();
    g_prevFrameHadScene = g_sceneVpFrameValid;
    g_sceneVpFrameValid = false; g_vpHist.clear(); g_winVpFrameValid = false; g_winVpHist.clear();
    g_winGeoRtLast = g_winGeoRt; g_winGeoRt = 0; g_winGeoDraws = 0; g_winGeoDrawsPerRt.clear();
    g_skinnedDrawsLast = g_skinnedDrawsThisFrame; g_skinnedDrawsThisFrame = 0;
    g_depthDrawsIntoFinalLast = g_depthDrawsIntoFinal; g_depthDrawsIntoFinal = 0;
    { uint32_t best = 0; for (auto& kv : g_depthDrawsPerRt) if (kv.second > best) best = kv.second; g_geoDrawsLast = best; g_depthDrawsPerRt.clear(); g_curGeoRt = 0; g_geoRt = 0; }
    { uint32_t best = 0; uint64_t bestRt = 0; for (auto& kv : g_drawsPerRt) if (kv.second > best) { best = kv.second; bestRt = kv.first; }
      if (bestRt != g_prevBusiestRt) g_prevBusiestRt2 = g_prevBusiestRt;
      g_prevBusiestDraws = best; g_prevBusiestRt = bestRt; }
    g_drawsPerRt.clear(); g_drawsPerDs.clear();
    // jitter bookkeeping for the next frame
    static uint32_t lastLogged = 0;
    if (g_frame > 600 && (g_frame - lastLogged) >= 600) {
        lastLogged = g_frame; g_missLogBudget = 3;
        logmsg("jitter (this frame): calls %u, no-cbv %u, dup-region %u, map-fail %u, patched %u (%u at a learned deep offset), no-matrix %u; VP found=%d (votes %zu); MV dispatches %u, resets %u, cam delta now rot %.3f pos %.1f, max in window rot %.3f pos %.1f, VP changed in %u frames; insertion pre-post %u / final-pre-HUD %u / composite %u",
               g_jitStat[0], g_jitStat[1], g_jitStat[2], g_jitStat[3], g_jitStat[4], g_jitStat[6], g_jitStat[5], (int)g_haveFrameVP, g_vpVotes.size(), g_mvDispatches, g_mvResets, g_camDeltaRot, g_camDeltaPos, g_camDeltaRotMax, g_camDeltaPosMax, g_vpChanges, g_prePostInjections, g_finalPreHudInjections, g_compositeInjections);
        logmsg("   dynamic draws replayed last frame: %u (skinned %u; mask %s, zero-MV %s); pre-HUD missed frames %u; final RTs %p/%p; 3D target %p (%u depth-bound draws); PSOs known %zu", g_dynDrawsLastFrame, g_skinnedDrawsLast, g_cfgDynMask ? "on" : "off", g_cfgDynZeroMV ? "on" : "off", g_ppMissFrames, (void*)g_finalRt[0], (void*)g_finalRt[1], (void*)g_geoRt, g_geoDrawsLast, g_psoDepth.size());
        logmsg("   scene viewport (last dynamic draw): %s (%.0f,%.0f %.0fx%.0f) in %ux%u targets", g_sceneVpValid ? "" : "unknown", g_sceneVp.x, g_sceneVp.y, g_sceneVp.width, g_sceneVp.height, g_dlssW, g_dlssH);
        if (const viewport* lw = layout_window()) logmsg("   layout window: the scene occupies (%.0f,%.0f %.0fx%.0f) of the image (mission briefing); %u evaluations in a window so far, 3D target re-picked in %u frames, object captures skipped (other views) %u", lw->x, lw->y, lw->width, lw->height, g_layoutFrames, g_geoRepicks, g_objMvSkippedOther);
        { const viewport* wvp = nullptr; const viewport* wl = win_layout_now(&wvp); if (wl && wvp) logmsg("   camera window: (%.0f,%.0f %.0fx%.0f) of the image, drawn at (%.0f,%.0f %.0fx%.0f); depth copied at its first reader in %u frames; camera cuts %u", wl->x, wl->y, wl->width, wl->height, wvp->x, wvp->y, wvp->width, wvp->height, g_winDepthCopies, g_winCuts); }
        { const viewport* fvp = nullptr; const viewport* fl = feed_layout_now(&fvp); const objmv::Stats& os = objmv::stats(); if (fl && fvp) logmsg("   caller feed: (%.0f,%.0f %.0fx%.0f) of the image, drawn at (%.0f,%.0f %.0fx%.0f) into %p; depth copies %u, feed frames %u, frames with a monitor capture %u, monitor draws projected %u (last frame: feed captures %u, monitor captures %u); %s", fl->x, fl->y, fl->width, fl->height, fvp->x, fvp->y, fvp->width, fvp->height, (void*)g_feedGeoRt, g_feedDepthCopies, g_feedFrames, g_monitorFrames, os.monitorDrawn, os.feedCaptured, os.monitorCaptured, os.monitorInfo); }
        { const objmv::Stats& os = objmv::stats(); logmsg("   frozen-background insertions %u (DLSS run before the game's screen capture for the pause menu / Codec); frozen pass-through frames %u; seed textures %zu; seed-blit redirects to the kept frame %u", g_frozenInjections, g_frozenPassFrames, g_seedTex.size(), g_keepRedirects);
        logmsg("   window-scene insertions %u (3D window rendered into its own target: Codec caller / pause model); flashback frames (footage pass up: DLSS took the current frame) %u", g_windowInjections, g_flashFrames);
        if (g_cfgProbe && g_probeReady) logmsg("   probe: %u frames read back; sub-rect layout flags: DLSS input %u / DoF output %u / composite input %u / presented %u", g_probeFrames, g_probeFlags[0], g_probeFlags[1], g_probeFlags[2], g_probeFlags[3]);
        if (g_cfgPostDof) logmsg("   PostDof: frames re-applied %u, draws skipped %u, skipped without re-apply %u, input fallbacks to the game's DoF %u, sub-rect frames handled %u, overlay draws masked %u, pre-warm evaluations %u, LATE scene writes %u, step-frame holds %u, wipe captures redirected %u, CoC constants %s", g_dofFrames, g_dofSkipped, g_dofMissed, g_dofFallbacks, g_dofSubRectFrames, g_dofOverlays, g_warmEvals, g_lateSceneWrites, g_stepFreezes, g_wipeRedirects, g_dofCbValid ? "captured" : "none");
        logmsg("   object motion: ready %d, last frame captured %u (with history %u, re-paired by signature %u, skipped %u, overflow %u); SO PSOs %u (%u failed), velocity PSOs %u, root sigs %u (%u SO-enabled), PSOs seen %u, slots %u, velocity passes %u | GPU ms: scene %.2f, stream-out %.2f, velocity %.2f; CPU %.2f ms/frame", (int)os.ready, os.capturedLast, os.withPrevLast, os.reorderedLast, os.skippedLast, os.overflowLast, os.soPsos, os.soPsoFailures, os.velPsos, os.rootSigsSeen, os.rootSigsSoEnabled, os.psosSeen, os.slotsUsed, os.velocityFrames, os.frameGpuMs, os.soGpuMs, os.velGpuMs, os.cpuMs); }
        for (int i = 0; i < 3; ++i) if (g_topVotes[i].count)
            logmsg("   vote #%d: %u regions, w-row (%.3f %.3f %.3f | %.1f), x-row (%.3f %.3f %.3f | %.1f), near %.2f", i, g_topVotes[i].count, g_topVotes[i].m[12], g_topVotes[i].m[13], g_topVotes[i].m[14], g_topVotes[i].m[15], g_topVotes[i].m[0], g_topVotes[i].m[1], g_topVotes[i].m[2], g_topVotes[i].m[3], g_topVotes[i].m[11]);
        g_camDeltaRotMax = g_camDeltaPosMax = 0; g_vpChanges = 0;
    }
    memset(g_jitStat, 0, sizeof(g_jitStat));
    if (!g_haveFrameVP) select_frame_vp();
    if (g_haveFrameVP) { memcpy(g_prevVP, g_frameVP, 64); g_havePrevVP = true; memcpy(g_lastGoodVP, g_frameVP, 64); g_haveLastGoodVP = true; }
    if (g_haveWinVP) { memcpy(g_winVPPrev, g_winVP, 64); g_haveWinVPPrev = true; } else g_haveWinVPPrev = false;
    g_haveFrameVP = false; g_vpVotes.clear(); g_patchedRegions.clear(); g_patchedDraws = 0; g_matrixMisses = 0;
    jitter_ndc(g_prevJitNdc);   // this frame's offsets become 'previous' for the next velocity pass
    advance_jitter();
    { std::lock_guard<std::mutex> lock(g_clMutex); for (auto& kv : g_cl) kv.second.bb_draws = 0; }
    if (g_frame % 120 == 0) reload_config();
}
static void on_present(command_queue* queue, swapchain* sc, const rect*, const rect*, uint32_t, const rect*)
{
    nr_kick();
    if (g_cfgProbe && g_probeReady && queue && sc) {   // every presented frame, generated ones included
        command_list* icl = queue->get_immediate_command_list();
        if (icl) { resource bb = sc->get_current_back_buffer(); if (bb.handle) probe_dispatch(icl, bb, resource_usage::present, 3); }
    }
    { static bool once = false; if (!once && queue) { once = true; ID3D12CommandQueue* q = reinterpret_cast<ID3D12CommandQueue*>(queue->get_native()); UINT64 hz = 0; if (q && SUCCEEDED(q->GetTimestampFrequency(&hz))) objmv::set_timestamp_frequency(hz); } }
    if (fg::status().swapchainProxied) return;
    frame_rollover();
}
static void on_init_swapchain(swapchain* sc, bool resize)
{
    device* dev = sc->get_device();
    g_backbuffers.clear();
    for (uint32_t i = 0; i < sc->get_back_buffer_count(); ++i) g_backbuffers.insert(sc->get_back_buffer(i).handle);
    resource_desc d = dev->get_resource_desc(sc->get_back_buffer(0));
    g_bbW = d.texture.width; g_bbH = d.texture.height;
    g_hwnd = reinterpret_cast<HWND>(sc->get_hwnd());
    logmsg("swapchain %s: %ux%u fmt=%u (%u buffers), window %p", resize ? "resized" : "created", g_bbW, g_bbH, (unsigned)d.texture.format, sc->get_back_buffer_count(), (void*)g_hwnd);
}
static void on_destroy_swapchain(swapchain*, bool) { g_backbuffers.clear(); }
static void on_init_device(device* dev)
{
    if (g_nrKickInProgress) { logmsg("device created: the NrKick WARP device - ignored"); return; }
    logmsg("device created: api=%u (d3d12=%u)", (unsigned)dev->get_api(), (unsigned)device_api::d3d12);
    if (dev->get_api() != device_api::d3d12) { logmsg("not D3D12 - add-on inactive (set Options -> Graphics -> API to DirectX 12)"); g_cfgEnabled = 0; return; }
    g_d3d = reinterpret_cast<ID3D12Device*>(dev->get_native());
    objmv::init(g_d3d, logmsg);   // hooks root signature / PSO creation: must precede the game's pipelines
    // Streamline (frame generation) is only loaded when FrameGen is enabled at startup: it takes over the swapchain,
    // so everything else (ReShade, the DLAA path, NGX add-ons) must be known to work with it before it is on by default.
    if (g_cfgFgMode != 0) { fg::init(g_d3d, g_gameDirW, logmsg); fg::set_frame_callback(frame_rollover); }   // before the game creates its swapchain
    else {
        logmsg("frame generation off at startup: Streamline not loaded (set FrameGen in the ini / overlay and restart to use it)");
        // Without Streamline nothing loads NGX before our first CreateFeature, and NGX-hooking add-ons (RenoDX's)
        // install their hooks when _nvngx.dll loads: initialize NGX now so the first create is already hooked.
        if (g_cfgEnabled && ngx_init(dev)) logmsg("NGX initialized at device creation (no Streamline): NGX-hooking add-ons can hook before the first CreateFeature");
    }
    if (g_cfgEnabled && (g_cfgMode != NVSDK_NGX_PerfQuality_Value_DLAA || g_cfgRenderResW)) {
        if (!g_internalW) logmsg("%s needs InternalRes; it will be detected and written to the ini this run - restart afterwards", g_cfgRenderResW ? "RenderRes" : g_cfgModeName);
        else if (ngx_init(dev)) setup_scaling();
    }
}
static void on_destroy_device(device* dev)
{
    if (g_nrKickDevice && dev->get_native() == reinterpret_cast<uint64_t>(g_nrKickDevice)) return;   // the NrKick WARP device, not the game's
    release_dlss_resources(dev);
    if (g_mvCb) { g_mvCb->Unmap(0, nullptr); g_mvCb->Release(); g_mvCb = nullptr; g_mvCbPtr = nullptr; }
    if (g_dummyUav) { g_dummyUav->Release(); g_dummyUav = nullptr; }
    if (g_mvHeap) { g_mvHeap->Release(); g_mvHeap = nullptr; }
    if (g_mvPso) { g_mvPso->Release(); g_mvPso = nullptr; }
    if (g_visPso) { g_visPso->Release(); g_visPso = nullptr; }
    if (g_mvRootSig) { g_mvRootSig->Release(); g_mvRootSig = nullptr; }
    g_mvReady = false; g_mvInitTried = false;
    if (g_dofCbRes) { g_dofCbRes->Unmap(0, nullptr); g_dofCbRes->Release(); g_dofCbRes = nullptr; g_dofCbPtr = nullptr; }
    for (ID3D12PipelineState** p : { &g_dofCocPso, &g_dofPackPso, &g_dofGatherPso, &g_dofCompositePso, &g_probePso }) if (*p) { (*p)->Release(); *p = nullptr; }
    if (g_probeHeap) { g_probeHeap->Release(); g_probeHeap = nullptr; }
    if (g_dofRootSig) { g_dofRootSig->Release(); g_dofRootSig = nullptr; }
    g_dofReady = false; g_dofInitTried = false; g_probeReady = false; g_probeInitTried = false;
    { std::lock_guard<std::mutex> lock(g_mapMutex); g_mapped.clear(); }
    if (g_ngxParams) { NVSDK_NGX_D3D12_DestroyParameters(g_ngxParams); g_ngxParams = nullptr; }
    if (g_ngxReady) { NVSDK_NGX_D3D12_Shutdown1(g_d3d); g_ngxReady = false; }
    fg::shutdown();
    objmv::shutdown();
}

static void load_config()
{
    snprintf(g_iniPath, MAX_PATH, "%s\\mgs4_dlss.ini", g_gameDir);
    g_cfgPreset = GetPrivateProfileIntA("DLSS", "Preset", 11, g_iniPath);
    g_cfgLogEveryN = GetPrivateProfileIntA("DLSS", "LogEveryN", 600, g_iniPath);
    g_cfgRecreateAfter = GetPrivateProfileIntA("DLSS", "RecreateAfter", 0, g_iniPath);
    char mode[32] = "DLAA"; GetPrivateProfileStringA("DLSS", "Mode", "DLAA", mode, sizeof(mode), g_iniPath);
    for (char* p = mode; *p; ++p) *p = (char)tolower((unsigned char)*p);
    struct { const char* n; NVSDK_NGX_PerfQuality_Value v; const char* pretty; } modes[] = {
        { "dlaa", NVSDK_NGX_PerfQuality_Value_DLAA, "DLAA" }, { "native", NVSDK_NGX_PerfQuality_Value_DLAA, "DLAA" },
        { "quality", NVSDK_NGX_PerfQuality_Value_MaxQuality, "Quality" }, { "balanced", NVSDK_NGX_PerfQuality_Value_Balanced, "Balanced" },
        { "performance", NVSDK_NGX_PerfQuality_Value_MaxPerf, "Performance" }, { "ultraperformance", NVSDK_NGX_PerfQuality_Value_UltraPerformance, "UltraPerformance" },
        { "ultra performance", NVSDK_NGX_PerfQuality_Value_UltraPerformance, "UltraPerformance" }, { "ultra", NVSDK_NGX_PerfQuality_Value_UltraPerformance, "UltraPerformance" } };
    g_cfgMode = NVSDK_NGX_PerfQuality_Value_DLAA;
    for (auto& m : modes) if (strcmp(mode, m.n) == 0) { g_cfgMode = m.v; strcpy_s(g_cfgModeName, m.pretty); }
    char res[32] = ""; GetPrivateProfileStringA("DLSS", "InternalRes", "", res, sizeof(res), g_iniPath);
    unsigned w = 0, h = 0; if (sscanf_s(res, "%ux%u", &w, &h) == 2 && w >= 640 && h >= 360) { g_internalW = w; g_internalH = h; }
    char rr[32] = ""; GetPrivateProfileStringA("DLSS", "RenderRes", "", rr, sizeof(rr), g_iniPath);
    unsigned rw = 0, rh = 0; if (sscanf_s(rr, "%ux%u", &rw, &rh) == 2 && rw >= 320 && rh >= 180) { g_cfgRenderResW = rw; g_cfgRenderResH = rh; }
    g_cfgLastDebugMode = -1;
    reload_config();
    logmsg("config: Enabled=%d Mode=%s RenderRes=%ux%u InternalRes=%ux%u Preset=%d Sharpness=%d%% DebugMode=%d", g_cfgEnabled, g_cfgModeName, g_cfgRenderResW, g_cfgRenderResH, g_internalW, g_internalH, g_cfgPreset, g_cfgSharpness100, g_cfgDebugMode);
}

// ---- overlay: the add-on's own "MGS4 DLSS" tab in ReShade's window, and the same under its entry on the Add-ons page ----
static const char* kModeNames[5] = { "DLAA (native)", "Quality", "Balanced", "Performance", "Ultra Performance" };
static const char* kModeIni[5] = { "DLAA", "Quality", "Balanced", "Performance", "UltraPerformance" };
static const NVSDK_NGX_PerfQuality_Value kModeVals[5] = { NVSDK_NGX_PerfQuality_Value_DLAA, NVSDK_NGX_PerfQuality_Value_MaxQuality, NVSDK_NGX_PerfQuality_Value_Balanced, NVSDK_NGX_PerfQuality_Value_MaxPerf, NVSDK_NGX_PerfQuality_Value_UltraPerformance };
static int g_uiMode = -1;
static int mode_index(NVSDK_NGX_PerfQuality_Value v) { for (int i = 0; i < 5; ++i) if (kModeVals[i] == v) return i; return 0; }
static void write_ini(const char* key, const char* val) { WritePrivateProfileStringA("DLSS", key, val, g_iniPath); }
static void write_ini_int(const char* key, int val) { char b[16]; snprintf(b, sizeof(b), "%d", val); write_ini(key, b); }

static void draw_overlay(effect_runtime*)
{
    if (g_uiMode < 0) g_uiMode = mode_index(g_cfgMode);
    bool en = g_cfgEnabled != 0;
    if (ImGui::Checkbox("Enable DLSS", &en)) { g_cfgEnabled = en ? 1 : 0; write_ini_int("Enabled", g_cfgEnabled); }
    if (ImGui::Combo("DLSS mode", &g_uiMode, kModeNames, 5)) write_ini("Mode", kModeIni[g_uiMode]);
    const int active = mode_index(g_cfgMode);
    if (g_uiMode != active && !g_cfgRenderResW)
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Restart the game to switch to %s (active now: %s)", kModeNames[g_uiMode], kModeNames[active]);
    if (g_cfgRenderResW) ImGui::TextColored(ImVec4(0.6f, 0.9f, 0.6f, 1.0f), "RenderRes=%ux%u overrides the mode: rendering %ux%u under the %s model (restart to change; set from the launcher's Settings tab)", g_cfgRenderResW, g_cfgRenderResH, g_scaling ? g_renderW : g_internalW, g_scaling ? g_renderH : g_internalH, g_cfgModeName);
    int preset = g_cfgPreset == 10 ? 0 : 1; const char* presets[] = { "J", "K (transformer, default)" };
    if (ImGui::Combo("DLSS preset", &preset, presets, 2)) { g_cfgPreset = preset == 0 ? 10 : 11; write_ini_int("Preset", g_cfgPreset); g_recreateRequested = true; }
    if (ImGui::SliderInt("Sharpness", &g_cfgSharpness100, 0, 100, "%d%%")) write_ini_int("Sharpness", g_cfgSharpness100);
    static const int dbgModes[] = { 0, 1, 2, 3, 4, 5, 6, 7, 9, 10, 11, 12 };
    const char* dbg[] = { "Off", "Magenta path test", "Bypass DLSS (A/B)", "Trace 3 frames", "Analyze draw constants", "Motion vectors (field only)", "UI layer (frame generation)", "HUD-less color (frame generation)",
                          "Motion vectors blended over the image (a character's vector silhouette must sit on the character)", "DoF: blurred layer only", "DoF: blur coverage", "DoF: overlay mask" };
    int d = 0; for (int i = 0; i < 12; ++i) if (dbgModes[i] == g_cfgDebugMode) d = i;
    if (ImGui::Combo("Debug", &d, dbg, 12)) { write_ini_int("DebugMode", dbgModes[d]); reload_config(); }
    if (g_cfgDebugKey) {
        int kd = 0; for (int i = 0; i < 12; ++i) if (dbgModes[i] == g_cfgDebugKeyMode) kd = i;
        ImGui::TextDisabled("DebugKey (0x%02X) switches Off <-> %s in the game, with this overlay closed", g_cfgDebugKey, dbg[kd]);
    } else ImGui::TextDisabled("DebugKey=none in mgs4_dlss.ini: set a key (F1..F12) there, or in the launcher's Settings, to flip a debug view in the game");
    ImGui::Separator();
    // The world stopped, the picture still drawn every frame: the one way to set NR, DLAA and the native image
    // side by side on the same frame. The game's own pause, brought on without the window losing focus.
    bool paused = g_worldPaused;
    if (ImGui::Checkbox("Pause the world (the game's focus-loss pause, with this window kept in front)", &paused)) world_pause(paused);
    if (g_cfgPauseKey) ImGui::TextDisabled("PauseKey (0x%02X) does the same in the game, overlay open or not. The world stays still while you change NR here. If you alt-tab it resumes: press the key twice.", g_cfgPauseKey);
    else ImGui::TextDisabled("PauseKey=none in mgs4_dlss.ini: name a key (Pause, F1..F12) there or in the launcher's Settings");
    ImGui::Separator();
    ImGui::Text("NGX: %s", g_ngxReady ? "ready" : (g_ngxInitTried ? "FAILED" : "not initialized yet"));
    if (g_dlss) ImGui::Text("Feature: %s  %ux%u -> %ux%u, preset %s", g_cfgModeName, g_dlssW, g_dlssH, g_dlssOutW, g_dlssOutH, g_cfgPreset == 10 ? "J" : "K");
    else ImGui::Text("Feature: none yet");
    ImGui::Text("Evaluations: %u  (%.0f/s)", g_evalCount, g_evalRate);
    ImGui::Text("Internal res %ux%u, render %ux%u%s", g_internalW, g_internalH, g_scaling ? g_renderW : g_internalW, g_scaling ? g_renderH : g_internalH, g_scaling ? " (textures shrunk)" : "");
    bool drs = g_cfgDRS != 0;
    if (ImGui::Checkbox("Handle the game's dynamic resolution (DLSS on the scene sub-rect; restart to change)", &drs)) { g_cfgDRS = drs ? 1 : 0; write_ini_int("DRS", g_cfgDRS); }
    if (g_sceneVpValid && g_internalW) {
        const float fx = g_sceneVp.width / float(g_internalW);
        if (fx < 0.995f || g_drsActiveLast) {
            // the game's own load-driven dynamic resolution: it renders the 3D scene into this sub-rect and upscales it
            // itself before DLSS sees the image, so the detail DLAA works from is capped by the game's scale
            if (g_cfgDRS == 2) ImGui::TextColored(ImVec4(0.6f, 0.9f, 0.6f, 1.0f), "Game dynamic resolution: the 3D scene is rendered at %.0fx%.0f (%.0f%%) -> DLSS evaluates that sub-rect, output resampled back into it (%u frames so far)", g_sceneVp.width, g_sceneVp.height, fx * 100.0f, g_drsFrames);
            else if (g_cfgDRS) ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Game dynamic resolution: the GAME renders its 3D scene at %.0fx%.0f (%.0f%%) and upscales it to %ux%u itself; DLAA runs on that full image, so detail is capped at the game's scale. The game picks it from its GPU load (DLSS + NR count) - Mode=Quality or less load keeps it at 100%% (%u frames so far)", g_sceneVp.width, g_sceneVp.height, fx * 100.0f, g_internalW, g_internalH, g_drsFrames);
            else ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "Game dynamic resolution: scene viewport %.0fx%.0f (%.0f%%) -> NOT handled (DRS=0): expect smearing while the game changes resolution (%u frames so far)", g_sceneVp.width, g_sceneVp.height, fx * 100.0f, g_drsFrames);
        }
        else ImGui::Text("Game dynamic resolution: scene viewport at full size (%.0fx%.0f)", g_sceneVp.width, g_sceneVp.height);
        if (const viewport* lw = layout_window()) ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f), "Layout window: the 3D scene occupies (%.0f,%.0f %.0fx%.0f) of the image (mission briefing); depth, vectors and jitter are expressed in that window", lw->x, lw->y, lw->width, lw->height);
    }
    bool jit = g_cfgJitter != 0;
    if (ImGui::Checkbox("Camera jitter (Halton, patched into draw constants)", &jit)) { g_cfgJitter = jit ? 1 : 0; write_ini_int("Jitter", g_cfgJitter); }
    bool pdof = g_cfgPostDof != 0;
    if (ImGui::Checkbox("Depth of field after DLSS / NR (PostDof: the game's DoF draws are skipped and re-applied on the DLSS output)", &pdof)) { g_cfgPostDof = pdof ? 1 : 0; write_ini_int("PostDof", g_cfgPostDof); }
    if (g_cfgPostDof) ImGui::Text("   DoF frames re-applied %u | draws skipped %u | skipped without re-apply %u | dynamic-resolution frames left to the game %u", g_dofFrames, g_dofSkipped, g_dofMissed, g_dofSubRectFrames);
    bool mvs = g_cfgMotionVectors != 0;
    if (ImGui::Checkbox("Camera motion vectors (reprojected from depth)", &mvs)) { g_cfgMotionVectors = mvs ? 1 : 0; write_ini_int("MotionVectors", g_cfgMotionVectors); }
    const char* ppNames[] = { "Auto (composite when a DLSS post-processing add-on such as DLSS 5 NR is loaded, else pre-post)", "Pre-post: DLAA before post-process/HUD", "Composite: DLAA on the final image" };
    int ppSel = g_cfgPrePostMode < 0 ? 0 : (g_cfgPrePostMode ? 1 : 2);
    if (ImGui::Combo("Insertion point (DLAA)", &ppSel, ppNames, 3)) { write_ini("PrePost", ppSel == 0 ? "auto" : (ppSel == 1 ? "1" : "0")); reload_config(); }
    ImGui::Text("Active: %s | DLSS post-processing add-on: %s | pre-post %u frames, composite %u frames", g_cfgPrePost ? "pre-post" : "composite", g_nrAddonLoaded ? g_nrAddonName : "none (CompositeIfLoaded list in ini)", g_prePostInjections, g_compositeInjections);
    bool dm = g_cfgDynMask != 0;
    if (ImGui::Checkbox("Character mask (skinned meshes -> bias current color)", &dm)) { g_cfgDynMask = dm ? 1 : 0; write_ini_int("DynamicMask", g_cfgDynMask); }
    bool dp2 = g_cfgDynMaskProps != 0;
    if (ImGui::Checkbox("Also mask props with their own transform", &dp2)) { g_cfgDynMaskProps = dp2 ? 1 : 0; write_ini_int("DynamicMaskProps", g_cfgDynMaskProps); }
    bool dz = g_cfgDynZeroMV != 0;
    if (ImGui::Checkbox("Zero motion on masked objects (third-person camera turns)", &dz)) { g_cfgDynZeroMV = dz ? 1 : 0; write_ini_int("DynamicZeroMV", g_cfgDynZeroMV); }
    bool fb = g_cfgFrozenBg != 0;
    if (ImGui::Checkbox("Frozen screens (pause menu / Codec): DLSS before the game captures its background", &fb)) { g_cfgFrozenBg = fb ? 1 : 0; write_ini_int("FrozenBackground", g_cfgFrozenBg); }
    ImGui::Text("Frozen-screen insertions: %u | pass-through frames (no second NR on a frozen image): %u", g_frozenInjections, g_frozenPassFrames);
    bool om = g_cfgObjectMV != 0;
    if (ImGui::Checkbox("Per-object motion vectors (stream-out of the game's vertex shaders)", &om)) { g_cfgObjectMV = om ? 1 : 0; write_ini_int("ObjectMV", g_cfgObjectMV); }
    {
        const objmv::Stats& os = objmv::stats();
        if (!os.ready) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "Object motion: not available (%s)", os.lastError[0] ? os.lastError : "init failed");
        else ImGui::Text("Object motion: %u captured (%u with history, %u skipped, %u overflow) | SO PSOs %u (%u failed) | GPU: scene %.2f ms, stream-out %.2f ms, velocity %.2f ms | CPU %.2f ms/frame", os.capturedLast, os.withPrevLast, os.skippedLast, os.overflowLast, os.soPsos, os.soPsoFailures, os.frameGpuMs, os.soGpuMs, os.velGpuMs, os.cpuMs);
        if (os.lastError[0] && os.ready) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "%s", os.lastError);
    }
    ImGui::Text("Dynamic draws replayed last frame: %u (skinned %u)", g_dynDrawsLastFrame, g_skinnedDrawsLast);
    { static const char* names[3] = { "in-game cutscene", "gameplay (HUD visible)", "no 3D scene (menu / loading / video)" };
      ImGui::Text("Scene state: %s | HUD draws %u, scene draws %u", g_sceneState >= 0 && g_sceneState < 3 ? names[g_sceneState] : "?", g_hudDrawsLast, g_sceneDrawsLast); }
    ImGui::Separator();
    {
        const fg::Status& st = fg::status();
        const char* fgNames[] = { "Off", "2x", "3x", "4x", "Dynamic (target frame rate)" };
        int fm = g_cfgFgMode;
        if (ImGui::Combo("Frame generation", &fm, fgNames, 5)) { write_ini_int("FrameGen", fm); reload_config(); }
        if (g_cfgFgMode != 0 && !st.loaded) ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Restart the game to load Streamline (frame generation was off at startup)");
        if (g_cfgFgMode == 4) {
            float t = g_cfgFgTargetFps;
            if (ImGui::InputFloat("Target fps (0 = monitor refresh)", &t, 10.0f, 30.0f, "%.0f")) { char b[32]; snprintf(b, sizeof(b), "%.0f", t < 0 ? 0.0f : t); write_ini("FGTargetFps", b); reload_config(); }
            if (!st.dynamicSupported && st.initialized) ImGui::TextWrapped("Driver-side dynamic multi-frame generation is not reported as available; the add-on picks 2x/3x/4x itself from the measured game frame rate (now %ux).", st.adaptiveFrames + 1);
        }
        const char* rfNames[] = { "Off", "On", "On + Boost" };
        int rf = g_cfgReflex;
        if (ImGui::Combo("NVIDIA Reflex", &rf, rfNames, 3)) { write_ini_int("Reflex", rf); reload_config(); }
        if (!st.loaded) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "Streamline runtime (sl.interposer.dll, sl.dlss_g.dll, ...) not found next to mgs4.exe");
        else if (!st.initialized) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "Streamline failed to initialize: %s", st.lastError);
        else if (!st.supported) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "DLSS Frame Generation not supported: %s", st.lastError[0] ? st.lastError : "adapter/driver");
        else if (!st.swapchainProxied) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "Swapchain was not created through Streamline (restart the game)");
        else {
            ImGui::Text("%s | status 0x%X | max %ux | dynamic MFG %s | vsync %s | VRAM %.0f MB", st.slVersion, st.statusFlags, st.maxFrames + 1, st.dynamicSupported ? "yes" : "no", st.vsyncSupported ? "ok" : "off required", st.vramBytes / 1048576.0);
            ImGui::Text("Presented %u frames | HUD-less color: %s", st.framesPresented, g_cfgPrePost ? "yes (pre-post)" : "composite: DLAA output + replayed UI layer");
            if (!g_cfgPrePost) ImGui::Text("UI layer: %u HUD draws replayed last frame, %u post passes skipped; HUD-less frames built %u", g_uiDrawsLast, g_uiPostSkippedLast, g_hudlessFrames);
            if (st.lastError[0]) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "%s", st.lastError);
        }
    }
    ImGui::Text("Jitter (%.3f, %.3f) px | VP: %s | MV pass: %s, %u resets | cam delta rot %.3f pos %.0f", g_jitterX, g_jitterY, g_havePrevVP ? "found" : "missing",
                g_mvReady ? "ok" : (g_mvInitTried ? "FAILED" : "idle"), g_mvResets, g_camDeltaRot, g_camDeltaPos);
}

// The debug key, read the way ReShade reads its own keys - so it is the game's window that has to be in front, and
// the overlay's key handling is respected. The switch goes through the ini, exactly as a change from the overlay
// does, so the overlay, the launcher's Settings tab and the next reload_config all see the same DebugMode.
static void on_reshade_present(effect_runtime* rt)
{
    if (!rt) return;
    if (g_cfgPauseKey && rt->is_key_pressed(g_cfgPauseKey)) { world_pause(!g_worldPaused); logmsg("PauseKey pressed"); }
    if (!g_cfgDebugKey || !rt->is_key_pressed(g_cfgDebugKey)) return;
    const int to = g_cfgDebugMode == 0 ? g_cfgDebugKeyMode : 0;
    write_ini_int("DebugMode", to);
    reload_config();
    logmsg("DebugKey pressed: DebugMode %s", to == 0 ? "off" : "on");
}

// The block under the add-on's entry on ReShade's Add-ons page: the switch for the whole add-on, what it is
// and what it is doing, and a pointer to the tab - the way the DLSS 5 NR add-on's reads. Everything else is on
// the MGS4 DLSS tab, so a person opening the Add-ons page to turn things on and off is not handed the lot twice.
static void draw_settings(effect_runtime*)
{
    bool en = g_cfgEnabled != 0;
    if (ImGui::Checkbox("Enable MGS4 DLSS", &en)) { g_cfgEnabled = en ? 1 : 0; write_ini_int("Enabled", g_cfgEnabled); }
    ImGui::TextDisabled("mgs4_dlss v" MGS4_DLSS_VERSION "  -  DLSS / DLAA, per-object motion vectors, frame generation, DoF after DLSS");
    if (g_dlss) ImGui::Text("%s  %ux%u -> %ux%u, preset %s%s", g_cfgModeName, g_dlssW, g_dlssH, g_dlssOutW, g_dlssOutH, g_cfgPreset == 10 ? "J" : "K",
                            g_nrAddonLoaded ? ", DLSS 5 NR add-on loaded" : "");
    else ImGui::Text("NGX: %s", g_ngxReady ? "ready, no feature yet" : (g_ngxInitTried ? "FAILED" : "not initialized yet"));
    ImGui::TextDisabled("Every setting is on the MGS4 DLSS tab, and in the launcher's Settings.");
}

extern "C" __declspec(dllexport) const char* NAME = "MGS4 DLSS";
extern "C" __declspec(dllexport) const char* DESCRIPTION = "Injects NGX DLSS (DLAA / Quality / Balanced / Performance / Ultra Performance) into Metal Gear Solid 4 (bgfx/D3D12).";

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    switch (reason) {
    case DLL_PROCESS_ATTACH: {
        GetModuleFileNameA(NULL, g_gameDir, MAX_PATH);
        if (char* p = strrchr(g_gameDir, '\\')) *p = 0;
        MultiByteToWideChar(CP_ACP, 0, g_gameDir, -1, g_gameDirW, MAX_PATH);
        char path[MAX_PATH]; snprintf(path, MAX_PATH, "%s\\logs\\mgs4_dlss.log", g_gameDir);
        g_log = fopen(path, "w");
        if (!reshade::register_addon(hModule)) { logmsg("register_addon failed (ReShade API mismatch?)"); return FALSE; }
        logmsg("mgs4_dlss v" MGS4_DLSS_VERSION " registered (header API %u)", RESHADE_API_VERSION);
        load_config();
        install_file_trace();   // before everything else: the stage load runs ahead of device creation
        reshade::register_event<reshade::addon_event::init_device>(on_init_device);
        reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
        reshade::register_event<reshade::addon_event::init_swapchain>(on_init_swapchain);
        reshade::register_event<reshade::addon_event::destroy_swapchain>(on_destroy_swapchain);
        reshade::register_event<reshade::addon_event::create_resource>(on_create_resource);
        reshade::register_event<reshade::addon_event::init_resource>(on_init_resource);
        reshade::register_event<reshade::addon_event::destroy_resource>(on_destroy_resource);
        reshade::register_event<reshade::addon_event::init_pipeline>(on_init_pipeline);
        reshade::register_event<reshade::addon_event::destroy_pipeline>(on_destroy_pipeline);
        reshade::register_event<reshade::addon_event::init_resource_view>(on_init_resource_view);
        reshade::register_event<reshade::addon_event::copy_descriptor_tables>(on_copy_descriptor_tables);
        reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind_rts);
        reshade::register_event<reshade::addon_event::bind_viewports>(on_bind_viewports);
        reshade::register_event<reshade::addon_event::bind_scissor_rects>(on_bind_scissor_rects);
        reshade::register_event<reshade::addon_event::bind_descriptor_tables>(on_bind_descriptor_tables);
        reshade::register_event<reshade::addon_event::push_descriptors>(on_push_descriptors);
        reshade::register_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
        reshade::register_event<reshade::addon_event::bind_vertex_buffers>(on_bind_vertex_buffers);
        reshade::register_event<reshade::addon_event::bind_index_buffer>(on_bind_index_buffer);
        reshade::register_event<reshade::addon_event::bind_pipeline_states>(on_bind_pipeline_states);
        reshade::register_event<reshade::addon_event::draw>(on_draw);
        reshade::register_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
        reshade::register_event<reshade::addon_event::clear_depth_stencil_view>(on_clear_dsv);
        reshade::register_event<reshade::addon_event::copy_resource>(on_copy_resource);
        reshade::register_event<reshade::addon_event::copy_texture_region>(on_copy_texture_region);
        reshade::register_event<reshade::addon_event::present>(on_present);
        reshade::register_event<reshade::addon_event::reshade_present>(on_reshade_present);
        // A titled overlay is a tab of its own in ReShade's window, the way RenoDX's is, and carries every
        // control; the untitled one is the short block under the add-on's entry on the Add-ons page.
        reshade::register_overlay("MGS4 DLSS", draw_overlay);
        reshade::register_overlay(nullptr, draw_settings);
        break; }
    case DLL_PROCESS_DETACH:
        reshade::unregister_event<reshade::addon_event::reshade_present>(on_reshade_present);
        reshade::unregister_overlay("MGS4 DLSS", draw_overlay);
        reshade::unregister_overlay(nullptr, draw_settings);
        reshade::unregister_addon(hModule);
        if (g_log) fclose(g_log);
        break;
    }
    return TRUE;
}
