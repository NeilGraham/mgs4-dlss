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
//  * NGX-hooking add-ons (renodx-dlss5) install hooks when nvngx_dlss.dll loads (inside our first CreateFeature), so the
//    feature is re-created once after a few frames to let them capture CreateFeature.
//  * Still zero motion vectors and zero jitter (camera jitter + camera-only MVs are the next step).
// Log: <game>\logs\mgs4_dlss.log. Config: <game>\mgs4_dlss.ini (Enabled/Sharpness/DebugMode live-reloaded).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <imgui.h>
#include <reshade.hpp>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#include "mv_cs.h"   // g_mv_cs[]: compiled src/mv_cs.hlsl (camera-only motion vectors from depth)
#include "mv_vis.h"  // g_mv_vis[]: compiled src/mv_vis.hlsl (debug visualisation of the motion vectors)
#include "hudless_cs.h"  // g_hudless_cs[]: compiled src/hudless_cs.hlsl (HUD-less colour for frame generation)
#include "resample_cs.h"  // g_resample_cs[]: compiled src/resample_cs.hlsl (DLSS output -> the game's dynamic-resolution sub-rect)
#include "fg.h"      // DLSS Frame Generation via Streamline (fg.cpp)
#include "objmv.h"   // per-object motion vectors via stream output (objmv.cpp)
#include <cstdio>
#include <cstdarg>
#include <cstring>
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
static int g_cfgDebugMode = 0;        // 0 normal, 1 = paint the displayed texture magenta, 2 = bypass DLSS, 3 = trace 3 frames again
static int g_cfgLastDebugMode = 0;
static NVSDK_NGX_PerfQuality_Value g_cfgMode = NVSDK_NGX_PerfQuality_Value_DLAA;
static char g_cfgModeName[32] = "DLAA";
static uint32_t g_internalW = 0, g_internalH = 0;   // from ini InternalRes (size of the game's render targets)
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
    const D3D12_DESCRIPTOR_HEAP_DESC hd = h->GetDesc();
    *type = hd.Type;
    *size = g_d3d->GetDescriptorHandleIncrementSize(hd.Type);
    *cpu = h->GetCPUDescriptorHandleForHeapStart().ptr + (uint64_t(off) + binding) * (*size);
    if (diag) snprintf(diag, diagLen, "heap=%p type=%u n=%u off=%u cpu=%llx", (void*)h, (unsigned)hd.Type, (unsigned)hd.NumDescriptors, off, (unsigned long long)*cpu);
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
static uint32_t g_bbW = 0, g_bbH = 0;
static std::unordered_set<uint64_t> g_backbuffers;
static uint32_t g_sceneDrawsThisFrame = 0;
static bool g_injectedThisFrame = false;
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
static resource_usage g_outState = resource_usage::unordered_access;
static uint32_t g_evalCount = 0, g_failCount = 0;
static bool g_featureCreatedThisFrame = false;
static uint32_t g_createdFrame = 0;
static bool g_recreated = false;
static bool g_recreateRequested = false;   // overlay changed the preset
static NVSDK_NGX_Handle* g_oldFeature = nullptr;
static uint32_t g_oldFeatureFrame = 0;
static ULONGLONG g_evalRateT0 = 0; static uint32_t g_evalRateN = 0; static float g_evalRate = 0;

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
// Phase 2: dynamic-object mask. Draws whose constants do not carry the camera VP at c[0] (characters, props) are replayed
// into a private depth buffer; the MV pass turns that into DLSS's bias-current-colour mask (and optionally zero motion).
static int g_cfgDynMask = 0;
static int g_cfgDynZeroMV = 1;
static resource g_dynDepth = { 0 }; static resource_view g_dynDsv = { 0 };
static resource_usage g_dynState = resource_usage::depth_stencil_write;
// HUD layer for frame generation in composite mode: the game's HUD draws are replayed into this RGBA target (cleared to
// zero each frame) and handed to DLSS-G as UI colour + alpha, so generated frames get the HUD re-composited unwarped.
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
static viewport g_sceneVp = {};              // viewport of the last dynamic scene draw (the port can render into a sub-viewport of its targets)
static std::unordered_map<uint64_t, uint32_t> g_drawOccurrence;   // this frame: geometry key -> times drawn so far (pass / instance index)
static bool g_sceneVpValid = false;
static viewport g_sceneVpFrame = {}; static bool g_sceneVpFrameValid = false;   // most common viewport of this frame's depth draws into the scene target
static std::unordered_map<uint64_t, std::pair<uint32_t, viewport>> g_vpHist;   // this frame: (w,h) -> draw count, viewport
// Dynamic resolution: the port renders the scene into a variable sub-viewport of its targets. With DRS=1 the DLSS
// feature is created in a scalable mode, evaluated on the sub-rect and its full-size output is resampled back into it.
static int g_cfgDRS = 0;
static resource g_scratch = { 0 }; static resource_usage g_scratchState = resource_usage::unordered_access;
static uint32_t g_drsMinW = 0, g_drsMinH = 0;    // DLSS dynamic minimum for the feature's mode
static uint32_t g_drsFrames = 0, g_drsSubW = 0, g_drsSubH = 0; static bool g_drsActiveLast = false;
static float drs_factor_x() { return (g_cfgDRS && g_sceneVpValid && g_internalW && g_sceneVp.width > 0 && g_sceneVp.width < g_internalW) ? g_sceneVp.width / float(g_internalW) : 1.0f; }
static float drs_factor_y() { return (g_cfgDRS && g_sceneVpValid && g_internalH && g_sceneVp.height > 0 && g_sceneVp.height < g_internalH) ? g_sceneVp.height / float(g_internalH) : 1.0f; }
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
static std::unordered_map<uint64_t, int> g_patchedRegions;   // per frame: region -> draw class (0 static world, 1 dynamic, 2 unknown)
static uint32_t g_patchedDraws = 0, g_matrixMisses = 0;
static float g_frameVP[16] = {}; static bool g_haveFrameVP = false;   // unjittered VP of this frame (majority of c[0] blocks)
static float g_prevVP[16] = {};  static bool g_havePrevVP = false;
struct vp_vote { uint32_t count = 0; float m[16]; };
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
static void jitter_ndc(float* o)
{
    const uint32_t w = g_scaling ? g_renderW : g_internalW, h = g_scaling ? g_renderH : g_internalH;
    o[0] = (g_cfgJitter && w) ? g_cfgJitterSignX * 2.0f * g_jitterX / (float(w) * drs_factor_x()) : 0.0f;
    o[1] = (g_cfgJitter && h) ? g_cfgJitterSignY * 2.0f * g_jitterY / (float(h) * drs_factor_y()) : 0.0f;
}
static uint32_t g_jitStat[6] = {};   // 0 calls, 1 no cbv, 2 already patched, 3 map failed, 4 patched, 5 no matrix
// Jitters the draw's clip matrix in place and classifies the draw: 0 = static world (camera VP at c[0]),
// 1 = dynamic (matrix elsewhere: prop/character with its own transform), 2 = no clip matrix found, -1 = not evaluated.
static int jitter_scene_draw(const cl_state& s)
{
    g_jitStat[0]++;
    if (!s.cbv_set[2] || !s.cbv_res[2].handle || (g_renderW == 0 && g_internalW == 0)) { g_jitStat[1]++; return -1; }
    const uint64_t key = s.cbv_res[2].handle ^ (s.cbv_off[2] * 0x9E3779B97F4A7C15ull);
    auto ins = g_patchedRegions.emplace(key, -1);
    if (!ins.second) { g_jitStat[2]++; return ins.first->second; }   // this constant region was already handled
    ID3D12Resource* r = reinterpret_cast<ID3D12Resource*>(s.cbv_res[2].handle);
    const UINT64 size = r->GetDesc().Width;
    const size_t nread = (s.cbv_off[2] + 36 * 4 <= size) ? 36 : 20;
    if (s.cbv_off[2] + nread * 4 > size) { g_jitStat[3]++; return -1; }
    void* p = nullptr; D3D12_RANGE rr = { (SIZE_T)s.cbv_off[2], (SIZE_T)(s.cbv_off[2] + nread * 4) };
    if (FAILED(r->Map(0, &rr, &p)) || !p) { g_jitStat[3]++; return -1; }
    float* c = reinterpret_cast<float*>(static_cast<char*>(p) + s.cbv_off[2]);
    float m[36] = {}; memcpy(m, c, nread * 4);     // read once (write-combined memory)
    int k = -1;
    for (int cand = 0; cand + 16 <= (int)nread; cand += 4) if (looks_like_clip_matrix(m + cand)) { k = cand; break; }
    int cls = 2;
    if (k >= 0) {
        cls = (k == 0) ? 0 : 1;
        if (k == 0) {   // vote: the block shared by most draws is the view-projection (identity model matrix)
            uint64_t hsh = 1469598103934665603ull; const uint32_t* u = reinterpret_cast<const uint32_t*>(m);
            for (int i = 0; i < 16; ++i) { hsh ^= u[i]; hsh *= 1099511628211ull; }
            vp_vote& v = g_vpVotes[hsh]; if (v.count++ == 0) memcpy(v.m, m, 64);
        }
        if (g_cfgJitter && !g_injectedThisFrame) {   // draws after DLSS ran (transparents, particles, HUD) stay unjittered
            const uint32_t w = g_scaling ? g_renderW : g_internalW, h = g_scaling ? g_renderH : g_internalH;
            const float ox = g_cfgJitterSignX * 2.0f * g_jitterX / (float(w) * drs_factor_x()), oy = g_cfgJitterSignY * 2.0f * g_jitterY / (float(h) * drs_factor_y());
            float row0[4], row1[4];
            for (int i = 0; i < 4; ++i) { row0[i] = m[k + i] + ox * m[k + 12 + i]; row1[i] = m[k + 4 + i] + oy * m[k + 12 + i]; }
            memcpy(c + k, row0, 16); memcpy(c + k + 4, row1, 16);
            static bool once = false;
            if (!once) { once = true; logmsg("first jittered matrix at c[%d]: row0 %.4f %.4f %.4f %.2f -> %.4f %.4f %.4f %.2f (jitter %.3f,%.3f px)", k / 4, m[k], m[k + 1], m[k + 2], m[k + 3], row0[0], row0[1], row0[2], row0[3], g_jitterX, g_jitterY); }
        }
        g_patchedDraws++; g_jitStat[4]++;
    } else {
        g_matrixMisses++; g_jitStat[5]++;
        if (g_missLogBudget > 0) { g_missLogBudget--; logmsg("no clip matrix (pso=%p rt=%p): c0=(%.3f %.3f %.3f %.2f) c1=(%.3f %.3f %.3f %.2f) c2=(%.3f %.3f %.3f %.2f) c3=(%.3f %.3f %.3f %.2f) c4=(%.3f %.3f %.3f %.2f)", (void*)s.pso, (void*)s.rt.handle, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15], m[16], m[17], m[18], m[19]); }
    }
    D3D12_RANGE wr = { (SIZE_T)s.cbv_off[2], (SIZE_T)(s.cbv_off[2] + nread * 4) }; r->Unmap(0, &wr);
    ins.first->second = cls;
    return cls;
}

static vp_vote g_topVotes[3] = {};
static void select_frame_vp()
{
    g_haveFrameVP = false;
    vp_vote top[3] = {};
    for (auto& kv : g_vpVotes) {
        const vp_vote& v = kv.second;
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
static ID3D12DescriptorHeap* g_mvHeap = nullptr;    // shader visible: 4 slots x (SRV depth, UAV mv) + 4 slots x (SRV mv, UAV out)
static ID3D12Resource* g_mvCb = nullptr; static uint8_t* g_mvCbPtr = nullptr;   // 8 x 256 B upload ring
static ID3D12Resource* g_dummyUav = nullptr;   // 8x8 R8 texture bound where a shader declares a UAV it never writes
static uint32_t g_mvSlot = 0;
static bool g_mvReady = false, g_mvInitTried = false;
static resource_usage g_mvState = resource_usage::shader_resource_non_pixel;
static uint32_t g_mvDispatches = 0, g_mvResets = 0;
static float g_camDeltaRot = 0, g_camDeltaPos = 0, g_camDeltaRotMax = 0, g_camDeltaPosMax = 0;
static uint32_t g_vpChanges = 0;   // frames (in the logging window) whose VP differed from the previous frame's
struct VisCB { float inSize[2]; float outSize[2]; float scale; float pad[3]; };
struct MvCB { float invVP[16]; float prevVP[16]; float size[2]; float nearZ; float reset; float dynZeroMV; float pad[3]; };

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
    if (FAILED(hr)) { logmsg("MV: visualisation PSO failed 0x%08lX", (unsigned long)hr); return false; }
    pso.CS = { g_hudless_cs, sizeof(g_hudless_cs) };
    hr = g_d3d->CreateComputePipelineState(&pso, IID_PPV_ARGS(&g_hudlessPso));
    if (FAILED(hr)) { logmsg("MV: HUD-less PSO failed 0x%08lX", (unsigned long)hr); return false; }
    pso.CS = { g_resample_cs, sizeof(g_resample_cs) };
    hr = g_d3d->CreateComputePipelineState(&pso, IID_PPV_ARGS(&g_resamplePso));
    if (FAILED(hr)) { logmsg("MV: resample PSO failed 0x%08lX", (unsigned long)hr); return false; }
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 96, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };   // 24 slots x [srv0, srv1, uav0, uav1]: mv 0-3, vis 8-11, hudless 12-15, resample 16-19
    hr = g_d3d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_mvHeap));
    if (FAILED(hr)) { logmsg("MV: CreateDescriptorHeap failed 0x%08lX", (unsigned long)hr); return false; }
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_UPLOAD, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1 };
    D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = 16 * 256; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc = { 1, 0 }; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
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
static float g_lastGoodVP[16] = {}; static bool g_haveLastGoodVP = false;   // most recent frame whose camera matrix was found
static uint32_t g_vpMissStreak = 0, g_vpMissesCovered = 0;

static int mv_dispatch(command_list* cmd, resource depth, format depthFmt, uint32_t w, uint32_t h)
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
        if (dr > 0.06f || dp > 1500.0f) {
            reset = 1; g_mvResets++;
            if (g_mvResets <= 200) logmsg("RESET f%u: camera delta rot %.4f (limit 0.06) pos %.1f (limit 1500) -> DLSS history cleared", g_frame, dr, dp);
        }
    }
    MvCB cb = {};
    if (!reset) {
        if (!invert4x4(curVP, cb.invVP)) { reset = 1; if (g_mvResets++ <= 200) logmsg("RESET f%u: view-projection not invertible", g_frame); }
        memcpy(cb.prevVP, prevVP, 64);
    }
    cb.size[0] = float(w); cb.size[1] = float(h); cb.nearZ = curVP[11] != 0 ? curVP[11] : 1.0f; cb.reset = reset ? 1.0f : 0.0f;
    cb.dynZeroMV = (g_cfgDynMask && g_cfgDynZeroMV) ? 1.0f : 0.0f;
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

// Debug: paint the motion-vector field into g_out (must be in unordered_access state).
// HUD-less colour: g_hudless (a copy of the DLAA output, in UAV state) gets the pre-HUD capture wherever the UI layer has content.
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
static void resample_dispatch(command_list* cmd, uint32_t subW, uint32_t subH, uint32_t fullW, uint32_t fullH, DXGI_FORMAT fmt)
{
    if (!g_mvReady || !g_resamplePso) return;
    const uint32_t slot = 16 + (g_mvSlot % 4);
    float cb[4] = { float(subW), float(subH), float(fullW), float(fullH) };
    memcpy(g_mvCbPtr + (8 + slot % 4) * 256, cb, sizeof(cb));
    const UINT inc = g_d3d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_mvHeap->GetCPUDescriptorHandleForHeapStart(); cpu.ptr += SIZE_T(slot) * 4 * inc;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_mvHeap->GetGPUDescriptorHandleForHeapStart(); gpu.ptr += UINT64(slot) * 4 * inc;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {}; srv.Format = fmt; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Texture2D.MipLevels = 1;
    g_d3d->CreateShaderResourceView(reinterpret_cast<ID3D12Resource*>(g_out.handle), &srv, cpu);
    D3D12_CPU_DESCRIPTOR_HANDLE h1 = cpu; h1.ptr += inc;
    g_d3d->CreateShaderResourceView(reinterpret_cast<ID3D12Resource*>(g_out.handle), &srv, h1);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {}; uav.Format = fmt; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE h2 = cpu; h2.ptr += 2 * inc;
    g_d3d->CreateUnorderedAccessView(reinterpret_cast<ID3D12Resource*>(g_scratch.handle), nullptr, &uav, h2);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDummy = {}; uavDummy.Format = DXGI_FORMAT_R8_UNORM; uavDummy.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE h3 = cpu; h3.ptr += 3 * inc;
    g_d3d->CreateUnorderedAccessView(g_dummyUav, nullptr, &uavDummy, h3);
    ID3D12GraphicsCommandList* native = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd->get_native());
    ID3D12DescriptorHeap* heaps[1] = { g_mvHeap };
    native->SetDescriptorHeaps(1, heaps);
    native->SetComputeRootSignature(g_mvRootSig);
    native->SetPipelineState(g_resamplePso);
    native->SetComputeRootConstantBufferView(0, g_mvCb->GetGPUVirtualAddress() + (8 + slot % 4) * 256);
    native->SetComputeRootDescriptorTable(1, gpu);
    D3D12_GPU_DESCRIPTOR_HANDLE gpuUav = gpu; gpuUav.ptr += 2 * inc;
    native->SetComputeRootDescriptorTable(2, gpuUav);
    native->Dispatch((subW + 7) / 8, (subH + 7) / 8, 1);
}
static void vis_dispatch(command_list* cmd, uint32_t inW, uint32_t inH, uint32_t outW, uint32_t outH)
{
    if (!g_mvReady || !g_visPso) return;
    const uint32_t slot = 8 + (g_mvSlot % 4);
    VisCB cb = {}; cb.inSize[0] = float(inW); cb.inSize[1] = float(inH); cb.outSize[0] = float(outW); cb.outSize[1] = float(outH); cb.scale = 0.05f;   // 10 px of motion = full swing
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
static uint32_t g_dumpUntil = 0;           // frame index until which scene draw constants are analysed
static uint32_t g_dumpDrawsLogged = 0;
struct block_stat { uint32_t count = 0; float row0[4] = {}; };
static std::unordered_map<uint64_t, block_stat> g_blockHist;   // (offset << 48) ^ hash(64 bytes) -> stats
static bool dumping() { return g_frame < g_dumpUntil; }

static bool read_cbv(const cl_state& s, int param, float* out, size_t nfloats)
{
    static int fails = 0;
    if (!s.cbv_set[param] || !s.cbv_res[param].handle) { if (fails++ < 3) logmsg("read_cbv: root[%d] not a CBV / no buffer (set=%d res=%p)", param, (int)s.cbv_set[param], (void*)s.cbv_res[param].handle); return false; }
    ID3D12Resource* r = reinterpret_cast<ID3D12Resource*>(s.cbv_res[param].handle);
    const D3D12_RESOURCE_DESC d = r->GetDesc();
    if (s.cbv_off[param] + nfloats * 4 > d.Width) { if (fails++ < 3) logmsg("read_cbv: offset %llu + %zu > buffer size %llu", (unsigned long long)s.cbv_off[param], nfloats * 4, (unsigned long long)d.Width); return false; }
    D3D12_HEAP_PROPERTIES hp = {}; D3D12_HEAP_FLAGS hf = {};
    HRESULT hr = r->GetHeapProperties(&hp, &hf);
    if (FAILED(hr) || (hp.Type != D3D12_HEAP_TYPE_UPLOAD && !(hp.Type == D3D12_HEAP_TYPE_CUSTOM && hp.CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE))) {
        if (fails++ < 3) logmsg("read_cbv: buffer %p heap type %u (hr=0x%08lX) is not CPU-readable", (void*)r, (unsigned)hp.Type, (unsigned long)hr); return false;
    }
    void* p = nullptr; D3D12_RANGE rr = { (SIZE_T)s.cbv_off[param], (SIZE_T)(s.cbv_off[param] + nfloats * 4) };
    hr = r->Map(0, &rr, &p);
    if (FAILED(hr) || !p) { if (fails++ < 3) logmsg("read_cbv: Map failed 0x%08lX", (unsigned long)hr); return false; }
    memcpy(out, static_cast<char*>(p) + s.cbv_off[param], nfloats * 4);
    D3D12_RANGE wr = { 0, 0 }; r->Unmap(0, &wr);
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
    // NGX is process-wide. When Streamline is active its common plugin has already initialised NGX with the device
    // the game's queue reports (ReShade's proxy); initialising again with the native device gives NGX two device
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
    if (g_cfgMode == NVSDK_NGX_PerfQuality_Value_DLAA || !g_internalW || !g_ngxReady) return;
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
    if (g_out.handle) { dev->destroy_resource(g_out); g_out = { 0 }; }
    if (g_dynDsv.handle) { dev->destroy_resource_view(g_dynDsv); g_dynDsv = { 0 }; }
    if (g_dynDepth.handle) { dev->destroy_resource(g_dynDepth); g_dynDepth = { 0 }; }
    if (g_mask.handle) { dev->destroy_resource(g_mask); g_mask = { 0 }; }
    if (g_uiRtv.handle) { dev->destroy_resource_view(g_uiRtv); g_uiRtv = { 0 }; }
    if (g_ui.handle) { dev->destroy_resource(g_ui); g_ui = { 0 }; }
    if (g_preHud.handle) { dev->destroy_resource(g_preHud); g_preHud = { 0 }; }
    if (g_hudless.handle) { dev->destroy_resource(g_hudless); g_hudless = { 0 }; }
    if (g_scratch.handle) { dev->destroy_resource(g_scratch); g_scratch = { 0 }; }
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
    cp.Feature.InPerfQualityValue = (g_cfgDRS && g_cfgMode == NVSDK_NGX_PerfQuality_Value_DLAA) ? NVSDK_NGX_PerfQuality_Value_MaxQuality : g_cfgMode;
    if (g_ngxCaps) { unsigned ow = 0, oh = 0, xw = 0, xh = 0, nw = 0, nh = 0; float sh = 0; if (!NVSDK_NGX_FAILED(NGX_DLSS_GET_OPTIMAL_SETTINGS(g_ngxCaps, outW, outH, cp.Feature.InPerfQualityValue, &ow, &oh, &xw, &xh, &nw, &nh, &sh))) { g_drsMinW = nw; g_drsMinH = nh; } }
    cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
    ID3D12GraphicsCommandList* native = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd->get_native());
    NVSDK_NGX_Handle* handle = nullptr;
    logmsg("NGX CreateFeature DLSS: cmd %p (%s), nvngx_dlss.dll %s, _nvngx %p, DLSS5 add-on %s", (void*)native, fg::inside_streamline() ? "inside SL?" : "game", GetModuleHandleA("nvngx_dlss.dll") ? "loaded" : "not loaded", (void*)GetModuleHandleA("_nvngx.dll"), GetModuleHandleA("renodx-dlss5.addon64") ? "loaded" : "absent");
    NVSDK_NGX_Result r = NGX_D3D12_CREATE_DLSS_EXT(native, 1, 1, &handle, g_ngxParams, &cp);
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
    dev->set_resource_name(g_out, "MGS4DLSS output");
    // Phase 2: private depth for replayed dynamic draws + the mask DLSS gets as bias-current-colour
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
    if (dev->create_resource(resource_desc(w, h, 1, 1, fmt, 1, memory_heap::default_, resource_usage::copy_dest | resource_usage::copy_source | resource_usage::shader_resource | resource_usage::unordered_access), nullptr, resource_usage::copy_dest, &g_hudless)) { dev->set_resource_name(g_hudless, "MGS4DLSS HUD-less colour"); g_hudlessState = resource_usage::copy_dest; }
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
    {
        const viewport& svp = g_sceneVpFrameValid ? g_sceneVpFrame : g_sceneVp;
        if (g_cfgDRS && (g_sceneVpFrameValid || g_sceneVpValid) && g_internalW && svp.width > 0 && (svp.width < g_internalW - 1 || svp.height < g_internalH - 1)) {
            float fx = svp.width / float(g_internalW), fy = svp.height / float(g_internalH);
            if (fx > 1.0f) fx = 1.0f; if (fy > 1.0f) fy = 1.0f; if (fx < 0.2f) fx = 0.2f; if (fy < 0.2f) fy = 0.2f;
            subW = (uint32_t)(cd.texture.width * fx + 0.5f); subH = (uint32_t)(cd.texture.height * fy + 0.5f);
            if (g_drsMinW && subW < g_drsMinW) subW = g_drsMinW; if (g_drsMinH && subH < g_drsMinH) subH = g_drsMinH;
            if (subW > cd.texture.width) subW = cd.texture.width; if (subH > cd.texture.height) subH = cd.texture.height;
            drsActive = subW < cd.texture.width || subH < cd.texture.height;
            if (drsActive) { g_drsFrames++; static bool once = false; if (!once) { once = true; logmsg("DRS: scene viewport %.0fx%.0f of %ux%u -> DLSS sub-rect %ux%u (min %ux%u)", svp.width, svp.height, cd.texture.width, cd.texture.height, subW, subH, g_drsMinW, g_drsMinH); } }
        }
    }
    g_drsActiveLast = drsActive; g_drsSubW = subW; g_drsSubH = subH;
    // Camera-only motion vectors from this frame's depth (VP = majority block of this frame's scene draws).
    select_frame_vp();
    const int mvReset = mv_dispatch(cmd, depth, dd.texture.format, subW, subH);

    ID3D12GraphicsCommandList* native = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd->get_native());
    // Per-object motion: rasterise the stream-out captures over the camera vectors (depth-tested against the scene depth).
    if (g_cfgObjectMV && objmv::has_captures() && g_mvRtv.handle) {
        auto itv = g_dsvForDs.find(depth.handle);
        if (itv != g_dsvForDs.end() && itv->second.handle) {
            cmd->barrier(depth, resource_usage::shader_resource_non_pixel, resource_usage::depth_stencil_write);
            cmd->barrier(g_mv, g_mvState, resource_usage::render_target); g_mvState = resource_usage::render_target;
            const float k = (g_scaling && g_internalW) ? float(g_renderW) / float(g_internalW) : 1.0f, ky = (g_scaling && g_internalH) ? float(g_renderH) / float(g_internalH) : 1.0f;
            D3D12_VIEWPORT svp = g_sceneVpValid ? D3D12_VIEWPORT{ g_sceneVp.x * k, g_sceneVp.y * ky, g_sceneVp.width * k, g_sceneVp.height * ky, g_sceneVp.min_depth, g_sceneVp.max_depth } : D3D12_VIEWPORT{ 0, 0, float(cd.texture.width), float(cd.texture.height), 0, 1 };
            float jitCur[2]; jitter_ndc(jitCur);
            objmv::velocity(native, D3D12_CPU_DESCRIPTOR_HANDLE{ (SIZE_T)g_mvRtv.handle }, D3D12_CPU_DESCRIPTOR_HANDLE{ (SIZE_T)itv->second.handle }, cd.texture.width, cd.texture.height, svp, jitCur, g_prevJitNdc);
            cmd->barrier(g_mv, resource_usage::render_target, resource_usage::shader_resource_non_pixel); g_mvState = resource_usage::shader_resource_non_pixel;
            cmd->barrier(depth, resource_usage::depth_stencil_write, resource_usage::shader_resource_non_pixel);
            g_objMvFrames++;
        }
    }
    NVSDK_NGX_D3D12_DLSS_Eval_Params ep = {};
    ep.Feature.pInColor = reinterpret_cast<ID3D12Resource*>(color.handle);
    ep.Feature.pInOutput = reinterpret_cast<ID3D12Resource*>(g_out.handle);
    ep.Feature.InSharpness = g_cfgSharpness100 / 100.0f;
    ep.pInDepth = reinterpret_cast<ID3D12Resource*>(depth.handle);
    ep.pInMotionVectors = reinterpret_cast<ID3D12Resource*>(g_mv.handle);
    ep.pInBiasCurrentColorMask = (g_cfgDynMask && g_mask.handle) ? reinterpret_cast<ID3D12Resource*>(g_mask.handle) : nullptr;
    g_lastDepth = depth.handle;
    ep.InJitterOffsetX = g_cfgJitter ? g_jitterX : 0.0f; ep.InJitterOffsetY = g_cfgJitter ? g_jitterY : 0.0f;
    ep.InRenderSubrectDimensions = { subW, subH };
    ep.InReset = (g_frame <= g_createdFrame + 1 || (mvReset && g_cfgMotionVectors)) ? 1 : 0;
    ep.InMVScaleX = 1.0f; ep.InMVScaleY = 1.0f;
    ep.InPreExposure = 1.0f; ep.InExposureScale = 1.0f;
    NVSDK_NGX_Result r = NVSDK_NGX_Result_Success;
    if (g_cfgDebugMode == 5) {
        vis_dispatch(cmd, subW, subH, outW, outH);   // show the MV field (sub-rect stretched) instead of the DLSS result
    } else if (g_cfgDebugMode == 7 && g_hudless.handle && g_preHudCaptured && !upscale) {
        // show the HUD-less colour DLSS-G would get: build it here (same steps as the tagging path), then copy it over the output
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
        // show the replayed UI layer instead of the DLSS result (what DLSS-G gets as UI colour + alpha)
        cmd->barrier(g_out, resource_usage::unordered_access, resource_usage::copy_dest);
        if (g_uiState != resource_usage::copy_source) { cmd->barrier(g_ui, g_uiState, resource_usage::copy_source); g_uiState = resource_usage::copy_source; }
        cmd->copy_resource(g_ui, g_out);
        cmd->barrier(g_out, resource_usage::copy_dest, resource_usage::unordered_access);
    } else {
        r = NGX_D3D12_EVALUATE_DLSS_EXT(native, g_dlss, g_ngxParams, &ep);
        if (NVSDK_NGX_FAILED(r)) { if (g_failCount++ < 5) logmsg("NGX EvaluateFeature -> %s", ngx_str(r)); }
        else { if (++g_evalCount == 1 || (g_cfgLogEveryN && g_evalCount % g_cfgLogEveryN == 0)) logmsg("NGX EvaluateFeature ok (#%u)", g_evalCount); }
    }

    objmv::mark_frame_end(native);

    if (fg::status().initialised && g_cfgFgMode != 0) {
        // Frame generation inputs: depth + motion vectors (render res) and, before post/HUD, the anti-aliased image as
        // HUD-less colour. In composite mode the UI is baked into the image so no HUD-less colour is tagged.
        fg::FrameInputs fi = {};
        fi.cmd = native;
        fi.depth = reinterpret_cast<ID3D12Resource*>(depth.handle); fi.depthFormat = static_cast<DXGI_FORMAT>(dd.texture.format); fi.depthState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        fi.mv = reinterpret_cast<ID3D12Resource*>(g_mv.handle); fi.mvState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        if (g_cfgPrePost && !upscale && g_cfgDebugMode != 5) { fi.hudless = reinterpret_cast<ID3D12Resource*>(g_out.handle); fi.hudlessFormat = static_cast<DXGI_FORMAT>(cd.texture.format); fi.hudlessState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; }
        else if (!g_cfgPrePost && !upscale && !drsActive && g_cfgDebugMode != 5 && g_ui.handle && g_uiDrawsThisFrame > 0) {
            // composite mode: the image DLSS-G sees has the HUD baked in; hand it the replayed HUD layer as UI colour+alpha
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
        fi.renderW = subW; fi.renderH = subH; fi.bbW = g_bbW; fi.bbH = g_bbH;
        fi.vpX = (int32_t)g_gameVp[0]; fi.vpY = (int32_t)g_gameVp[1]; fi.vpW = (uint32_t)g_gameVp[2]; fi.vpH = (uint32_t)g_gameVp[3];
        fg::CameraInput ci = { g_frameVP, g_havePrevVP ? g_prevVP : g_frameVP, ep.InJitterOffsetX, ep.InJitterOffsetY, ep.InReset != 0, subW, subH };
        fg::frame_inputs(g_frame, fi, ci);
    }

    if ((!g_recreated && g_cfgRecreateAfter > 0 && g_evalCount >= (uint32_t)g_cfgRecreateAfter) || (g_recreateRequested && !g_oldFeature)) {
        g_recreated = true; g_recreateRequested = false;
        g_oldFeature = g_dlss; g_oldFeatureFrame = g_frame; g_dlss = nullptr;
        logmsg("re-creating DLSS feature (old feature released in a few frames)");
    }
    { ULONGLONG t = GetTickCount64(); g_evalRateN++; if (t - g_evalRateT0 >= 1000) { g_evalRate = g_evalRateN * 1000.0f / float(t - g_evalRateT0); g_evalRateN = 0; g_evalRateT0 = t; } }

    // DRS: the game's composite / post chain samples only the sub-rect, so the full-size output goes back into it.
    const bool resampled = drsActive && g_scratch.handle && g_resamplePso && !NVSDK_NGX_FAILED(r);
    if (resampled) {
        cmd->barrier(g_out, resource_usage::unordered_access, resource_usage::shader_resource_non_pixel);
        if (g_scratchState != resource_usage::unordered_access) { cmd->barrier(g_scratch, g_scratchState, resource_usage::unordered_access); g_scratchState = resource_usage::unordered_access; }
        resample_dispatch(cmd, subW, subH, outW, outH, static_cast<DXGI_FORMAT>(cd.texture.format));
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
        if (!NVSDK_NGX_FAILED(r)) cmd->copy_resource(g_out, color);
        const resource res2[2] = { color, g_out };
        const resource_usage from2[2] = { resource_usage::copy_dest, resource_usage::copy_source };
        const resource_usage to2[2] = { colorState, resource_usage::unordered_access };
        cmd->barrier(2, res2, from2, to2); g_outState = resource_usage::unordered_access;
    }
    if (g_cfgDebugMode == 1) {
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
    {
        std::lock_guard<std::mutex> lock(g_scaledMutex);
        g_scaledTex.erase(res.handle);
        g_liveTex.erase(res.handle);
        g_dsvForDs.erase(res.handle);
    }
    // forget every cross-frame reference to it (level transitions destroy and recreate the render targets)
    if (g_prevBusiestRt == res.handle) g_prevBusiestRt = 0;
    if (g_prevBusiestRt2 == res.handle) g_prevBusiestRt2 = 0;
    if (g_finalRt[0] == res.handle) g_finalRt[0] = 0;
    if (g_finalRt[1] == res.handle) g_finalRt[1] = 0;
    if (g_geoRt == res.handle) g_geoRt = 0;
    if (g_curGeoRt == res.handle) g_curGeoRt = 0;
    g_depthDrawsPerRt.erase(res.handle);
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
    return d.type == resource_type::texture_2d && d.texture.width >= 640 && d.texture.width <= g_bbW && d.texture.height >= 360 && d.texture.height <= g_bbH;
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
    if (!is_backbuffer(s.rt)) {
        if (s.rt_w >= 640 && s.rt_h >= 360) {
            if (++g_sceneDrawsThisFrame == 1) { fg::frame_begin(g_frame); objmv::mark_frame_begin(reinterpret_cast<ID3D12GraphicsCommandList*>(cmd->get_native())); }
            g_drawsPerRt[s.rt.handle]++;
            if (s.ds.handle) {
                g_dsForRt[s.rt.handle] = s.ds.handle; g_drawsPerDs[s.ds.handle]++;
                if (s.dsv.handle) g_dsvForDs[s.ds.handle] = s.dsv;
                if (s.vp_valid && s.rt_w >= 640 && s.vp.width <= s.rt_w && (g_dlssW == 0 || s.rt_w == g_dlssW)
                    && (g_curGeoRt == 0 || s.rt.handle == g_curGeoRt)          // only the scene target, not shadow/reflection passes
                    && s.vp.width >= s.rt_w * 0.5f && s.vp.height >= s.rt_h * 0.5f   // a plausible full-frame viewport
                    && s.vp.x <= 1.0f && s.vp.y <= 1.0f) {
                    // the 3D scene's viewport = the one most depth-tested draws use (a few full-size depth-tested quads exist too)
                    auto& e = g_vpHist[(uint64_t)(uint32_t)(s.vp.width + 0.5f) << 32 | (uint32_t)(s.vp.height + 0.5f)];
                    if (e.first++ == 0) e.second = s.vp;
                    if (!g_sceneVpFrameValid || e.first > g_vpHist[(uint64_t)(uint32_t)(g_sceneVpFrame.width + 0.5f) << 32 | (uint32_t)(g_sceneVpFrame.height + 0.5f)].first) { g_sceneVpFrame = e.second; g_sceneVpFrameValid = true; }
                }
                if (dumping()) analyse_scene_draw(s);
                const bool skinned = pso_get(s.pso).skinned;
                int cls = -1;
                if (g_cfgEnabled && g_cfgDebugMode != 2) cls = jitter_scene_draw(s);
                if (skinned) g_skinnedDrawsThisFrame++;
                // Phase 2: dynamic draws = skinned meshes (optionally props with their own model matrix).
                const bool dynamic = skinned || (g_cfgDynMaskProps && cls >= 1);
                // Per-object motion: stream out this draw's clip positions with one extra draw under the game's own state
                // (root signature, root arguments, IA buffers all as bound; only the pipeline and SO targets change).
                if (g_cfgObjectMV && dynamic && objmv::ready() && !g_injectedThisFrame && s.ds.handle == g_lastDepth && da.count > 6 && s.pso) {
                    // Identity across frames: the geometry (buffers, index range) plus the n-th time it is drawn this frame
                    // (pass / instance). Not the PSO: bgfx hands the same draw a different pipeline object every frame.
                    uint64_t key = 1469598103934665603ull;
                    const uint64_t parts[8] = { s.vb0.handle, s.vb0_off, s.ib.handle, s.ib_off, da.first, da.count, (uint64_t)(int64_t)da.vertex_offset, da.instances };
                    for (uint64_t v : parts) { key ^= v; key *= 1099511628211ull; }
                    const uint32_t occurrence = g_drawOccurrence[key]++;
                    key ^= occurrence; key *= 1099511628211ull;
                    objmv::DrawArgs oda = { da.indexed, da.count, da.instances, da.first, da.vertex_offset, da.first_instance };
                    const bool jittered = g_cfgJitter && (cls == 0 || cls == 1);   // its clip matrix was patched in place this frame
                    objmv::capture(reinterpret_cast<ID3D12GraphicsCommandList*>(cmd->get_native()), key, s.pso, s.topology, oda, jittered);
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
            const bool depthOn = pso_depth_enabled(s.pso);
            if (s.ds.handle) {
                const uint32_t n = ++g_depthDrawsPerRt[s.rt.handle];
                if (s.rt.handle == g_finalRt[0] || s.rt.handle == g_finalRt[1]) g_depthDrawsIntoFinal++;
                // in-frame detection of the 3D target: the first depth-bound RT that reaches 40% of last frame's peak
                if (!g_curGeoRt && n >= 20 && n * 10 >= g_geoDrawsLast * 4) g_curGeoRt = s.rt.handle;
            }
            g_geoRt = g_curGeoRt;
            // Frame generation, composite mode: replay HUD draws into the UI layer. HUD draws = depth-off draws into the
            // final texture once the 3D scene is in; post-process passes into it are told apart by sampling a scene-sized
            // input (half the frame size or more), HUD draws only sample atlases.
            const bool uiCandidate = !g_injectedThisFrame && !depthOn && g_geoRt
                && (s.rt.handle == g_finalRt[0] || s.rt.handle == g_finalRt[1]) && s.rt_w == g_dlssW && s.rt_h == g_dlssH;
            const bool uiReplay = uiCandidate && g_ui.handle && g_uiRtv.handle && g_cfgFgMode != 0 && !g_cfgPrePost;
            if (uiCandidate) {
                auto itd = g_depthDrawsPerRt.find(g_geoRt);
                const uint32_t done = itd != g_depthDrawsPerRt.end() ? itd->second : 0;
                if (done >= 20 && done * 10 >= g_geoDrawsLast * 8) {
                    bool post = false;
                    for (int p = 1; p < 5 && !post; ++p) if (s.table_set[p]) for (int i = 0; i < 8 && !post; ++i) {
                        resource r = resolve_descriptor(dev, s.tables[p], i);
                        if (r.handle && is_live(r.handle)) { resource_desc d = dev->get_resource_desc(r); if (d.type == resource_type::texture_2d && d.texture.width * 2 >= g_dlssW && d.texture.height * 2 >= g_dlssH) post = true; }
                    }
                    if (post) g_uiPostSkippedThisFrame++;
                    else if (!uiReplay) g_hudDrawsThisFrame++;   // classification only
                    else {
                        g_hudDrawsThisFrame++;
                        t_reentrant = true;
                        if (g_uiState != resource_usage::render_target) { cmd->barrier(g_ui, g_uiState, resource_usage::render_target); g_uiState = resource_usage::render_target; }
                        if (!g_uiClearedThisFrame) {
                            const float zero[4] = { 0, 0, 0, 0 }; cmd->clear_render_target_view(g_uiRtv, zero); g_uiClearedThisFrame = true;
                            if (g_preHud.handle) {   // the final texture right before its first HUD draw = post-processed scene without HUD
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
        if (g_dumpPending) { g_dumpPending = false; g_dumpUntil = g_frame + 3; g_dumpDrawsLogged = 0; logmsg("analysing scene draw constants for frames %u..%u", g_frame + 1, g_dumpUntil - 1); }
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

    if (color.handle && color.handle != g_finalRt[0]) { g_finalRt[1] = g_finalRt[0]; g_finalRt[0] = color.handle; }
    if (color.handle && s.vp.width > 0) { g_gameVp[0] = s.vp.x; g_gameVp[1] = s.vp.y; g_gameVp[2] = s.vp.width; g_gameVp[3] = s.vp.height; }
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
    resource_desc cd = dev->get_resource_desc(color);
    if (!g_scaling || !is_scaled(color)) remember_internal_res(cd.texture.width, cd.texture.height);   // game's real render size (changed in-game?)
    run_dlss(cmd, &s, color, resource_usage::shader_resource_pixel, srvCpu);
}
static bool on_draw(command_list* cmd, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi) { handle_draw(cmd, draw_args{ false, vc, ic, fv, fi, 0 }); return false; }
static bool on_draw_indexed(command_list* cmd, uint32_t ic, uint32_t inst, uint32_t fi, int32_t vo, uint32_t finst) { handle_draw(cmd, draw_args{ true, ic, inst, fi, finst, vo }); return false; }

static uint32_t g_ppMissFrames = 0;   // frames where the geometry target was finished but no draw sampled it (pre-post could not insert)
static void handle_copy(command_list* cmd, resource src, resource dst, const char* what)
{
    if (g_bbW == 0 || fg::inside_streamline()) return;
    device* dev = cmd->get_device();
    if (tracing() && (src.handle == g_geoRt || dst.handle == g_geoRt) && is_live(src.handle) && is_live(dst.handle))
        logmsg("f%u %s %s -> %s (3D target involved)", g_frame, what, desc_str(dev, src).c_str(), desc_str(dev, dst).c_str());
    if (!is_backbuffer(dst)) return;
    if (tracing()) logmsg("f%u %s src=%s -> backbuffer (scene draws so far %u)", g_frame, what, desc_str(dev, src).c_str(), g_sceneDrawsThisFrame);
    if (g_injectedThisFrame || !g_cfgEnabled || g_sceneDrawsThisFrame < 20 || !scene_sized(dev, src) || g_scaling) return;
    g_injectedThisFrame = true;
    cl_state s; { std::lock_guard<std::mutex> lock(g_clMutex); s = g_cl[cmd]; }
    run_dlss(cmd, &s, src, resource_usage::copy_source, 0);
}
static bool on_copy_resource(command_list* cmd, resource src, resource dst) { handle_copy(cmd, src, dst, "copy_resource"); return false; }
static bool on_copy_texture_region(command_list* cmd, resource src, uint32_t, const subresource_box*, resource dst, uint32_t, const subresource_box*, filter_mode) { handle_copy(cmd, src, dst, "copy_texture_region"); return false; }

static void reload_config()
{
    g_cfgEnabled = GetPrivateProfileIntA("DLSS", "Enabled", 1, g_iniPath);
    g_cfgSharpness100 = GetPrivateProfileIntA("DLSS", "Sharpness", 0, g_iniPath);
    g_cfgDebugMode = GetPrivateProfileIntA("DLSS", "DebugMode", 0, g_iniPath);
    g_cfgJitter = GetPrivateProfileIntA("DLSS", "Jitter", 1, g_iniPath);
    g_cfgDynMask = GetPrivateProfileIntA("DLSS", "DynamicMask", 0, g_iniPath);
    g_cfgDynMaskProps = GetPrivateProfileIntA("DLSS", "DynamicMaskProps", 0, g_iniPath);
    g_cfgDynZeroMV = GetPrivateProfileIntA("DLSS", "DynamicZeroMV", 0, g_iniPath);
    g_cfgMotionVectors = GetPrivateProfileIntA("DLSS", "MotionVectors", 1, g_iniPath);
    {
        char pp[16] = "auto"; GetPrivateProfileStringA("DLSS", "PrePost", "auto", pp, sizeof(pp), g_iniPath);
        g_cfgPrePostMode = (_stricmp(pp, "auto") == 0 || strcmp(pp, "-1") == 0) ? -1 : (atoi(pp) != 0 ? 1 : 0);
        // Add-ons that post-process DLSS's output (the DLSS 5 Neural Rendering add-on renodx-dlss5 is the known one)
        // need the final image, so when one of the listed modules is loaded we insert at the composite. Without any,
        // pre-post gives the cleanest AA (vignette/HUD outside DLSS). Extend the list in the ini for other tools.
        char list[512] = ""; GetPrivateProfileStringA("DLSS", "CompositeIfLoaded", "renodx-dlss5.addon64", list, sizeof(list), g_iniPath);
        g_nrAddonLoaded = false; g_nrAddonName[0] = 0;
        for (char* tok = strtok(list, ";,"); tok; tok = strtok(nullptr, ";,")) {
            while (*tok == ' ') ++tok;
            if (*tok && GetModuleHandleA(tok) != nullptr) { g_nrAddonLoaded = true; strncpy_s(g_nrAddonName, tok, _TRUNCATE); break; }
        }
        const int eff = g_cfgPrePostMode < 0 ? (g_nrAddonLoaded ? 0 : 1) : g_cfgPrePostMode;
        if (eff != g_cfgPrePost) logmsg("insertion: %s (PrePost=%s, DLSS post-processing add-on %s)", eff ? "pre-post (before post-process/HUD)" : "composite (final image)", g_cfgPrePostMode < 0 ? "auto" : (g_cfgPrePostMode ? "1" : "0"), g_nrAddonLoaded ? g_nrAddonName : "not loaded");
        g_cfgPrePost = eff;
    }
    g_cfgDynMask = GetPrivateProfileIntA("DLSS", "DynamicMask", 0, g_iniPath);
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
    g_cfgDRS = GetPrivateProfileIntA("DLSS", "DRS", 0, g_iniPath);
    g_cfgSceneLog = GetPrivateProfileIntA("DLSS", "SceneLog", 1, g_iniPath);
    g_cfgHudMin = GetPrivateProfileIntA("DLSS", "HudMinDraws", 25, g_iniPath);
    g_cfgJitterSignX = GetPrivateProfileIntA("DLSS", "JitterSignX", 1, g_iniPath) < 0 ? -1.0f : 1.0f;
    g_cfgJitterSignY = GetPrivateProfileIntA("DLSS", "JitterSignY", -1, g_iniPath) < 0 ? -1.0f : 1.0f;
    if (g_cfgDebugMode != g_cfgLastDebugMode) {
        logmsg("DebugMode -> %d", g_cfgDebugMode); g_cfgLastDebugMode = g_cfgDebugMode;
        if (g_cfgDebugMode == 3) { g_traceUntil = g_frame + 3; logmsg("tracing backbuffer draws/copies for frames %u..%u", g_frame, g_traceUntil - 1); }
        if (g_cfgDebugMode == 4) { if (g_traceArmed) { g_dumpUntil = g_frame + 2; g_dumpDrawsLogged = 0; logmsg("analysing scene draw constants for frames %u..%u", g_frame, g_dumpUntil - 1); } else g_dumpPending = true; }
    }
}

// End-of-frame bookkeeping. Runs on the game's Present: from ReShade's present event normally, or from the
// Streamline proxy swapchain's Present hook when frame generation is set up (then ReShade's present event fires on
// Streamline's present thread, for generated frames too, and must not touch the per-frame state).
static void frame_rollover()
{
    fg::poll();
    g_frame++;
    if (g_oldFeature && g_frame > g_oldFeatureFrame + 6) {
        NVSDK_NGX_Result r = NVSDK_NGX_D3D12_ReleaseFeature(g_oldFeature); g_oldFeature = nullptr;
        logmsg("released previous DLSS feature -> %s", ngx_str(r));
    }
    if (tracing()) { logmsg("f%u: %u shader-view creations, %u descriptor copies this frame", g_frame - 1, g_viewEvents, g_copyEvents); g_viewSamples = 0; }
    if (dumping() && !g_blockHist.empty()) report_blocks();
    g_viewEvents = 0; g_copyEvents = 0;
    if (g_cfgPrePost && !g_scaling && g_injectedThisFrame && g_prevBusiestRt) { static uint32_t lastPP = 0; if (g_prePostInjections == lastPP) g_ppMissFrames++; lastPP = g_prePostInjections; }
    g_injectedThisFrame = false; g_featureCreatedThisFrame = false;
    g_sceneDrawsLast = g_sceneDrawsThisFrame; g_sceneDrawsThisFrame = 0;
    g_dynDrawsLastFrame = g_dynDrawsThisFrame; g_dynDrawsThisFrame = 0; g_dynClearedThisFrame = false;
    g_uiDrawsLast = g_uiDrawsThisFrame; g_uiDrawsThisFrame = 0; g_uiPostSkippedLast = g_uiPostSkippedThisFrame; g_uiPostSkippedThisFrame = 0; g_uiClearedThisFrame = false; g_preHudCaptured = false;
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
    g_hudDrawsLast = g_hudDrawsThisFrame; g_hudDrawsThisFrame = 0;
    objmv::new_frame(g_frame); g_drawOccurrence.clear();
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
    g_sceneVpFrameValid = false; g_vpHist.clear();
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
        logmsg("jitter (this frame): calls %u, no-cbv %u, dup-region %u, map-fail %u, patched %u, no-matrix %u; VP found=%d (votes %zu); MV dispatches %u, resets %u, cam delta now rot %.3f pos %.1f, max in window rot %.3f pos %.1f, VP changed in %u frames; insertion pre-post %u / composite %u",
               g_jitStat[0], g_jitStat[1], g_jitStat[2], g_jitStat[3], g_jitStat[4], g_jitStat[5], (int)g_haveFrameVP, g_vpVotes.size(), g_mvDispatches, g_mvResets, g_camDeltaRot, g_camDeltaPos, g_camDeltaRotMax, g_camDeltaPosMax, g_vpChanges, g_prePostInjections, g_compositeInjections);
        logmsg("   dynamic draws replayed last frame: %u (skinned %u; mask %s, zero-MV %s); pre-HUD missed frames %u; final RTs %p/%p; 3D target %p (%u depth-bound draws); PSOs known %zu", g_dynDrawsLastFrame, g_skinnedDrawsLast, g_cfgDynMask ? "on" : "off", g_cfgDynZeroMV ? "on" : "off", g_ppMissFrames, (void*)g_finalRt[0], (void*)g_finalRt[1], (void*)g_geoRt, g_geoDrawsLast, g_psoDepth.size());
        logmsg("   scene viewport (last dynamic draw): %s (%.0f,%.0f %.0fx%.0f) in %ux%u targets", g_sceneVpValid ? "" : "unknown", g_sceneVp.x, g_sceneVp.y, g_sceneVp.width, g_sceneVp.height, g_dlssW, g_dlssH);
        { const objmv::Stats& os = objmv::stats(); logmsg("   object motion: ready %d, last frame captured %u (with history %u, skipped %u, overflow %u); SO PSOs %u (%u failed), velocity PSOs %u, root sigs %u (%u SO-enabled), PSOs seen %u, slots %u, velocity passes %u | GPU ms: scene %.2f, stream-out %.2f, velocity %.2f; CPU %.2f ms/frame", (int)os.ready, os.capturedLast, os.withPrevLast, os.skippedLast, os.overflowLast, os.soPsos, os.soPsoFailures, os.velPsos, os.rootSigsSeen, os.rootSigsSoEnabled, os.psosSeen, os.slotsUsed, os.velocityFrames, os.frameGpuMs, os.soGpuMs, os.velGpuMs, os.cpuMs); }
        for (int i = 0; i < 3; ++i) if (g_topVotes[i].count)
            logmsg("   vote #%d: %u regions, w-row (%.3f %.3f %.3f | %.1f), x-row (%.3f %.3f %.3f | %.1f), near %.2f", i, g_topVotes[i].count, g_topVotes[i].m[12], g_topVotes[i].m[13], g_topVotes[i].m[14], g_topVotes[i].m[15], g_topVotes[i].m[0], g_topVotes[i].m[1], g_topVotes[i].m[2], g_topVotes[i].m[3], g_topVotes[i].m[11]);
        g_camDeltaRotMax = g_camDeltaPosMax = 0; g_vpChanges = 0;
    }
    memset(g_jitStat, 0, sizeof(g_jitStat));
    if (!g_haveFrameVP) select_frame_vp();
    if (g_haveFrameVP) { memcpy(g_prevVP, g_frameVP, 64); g_havePrevVP = true; memcpy(g_lastGoodVP, g_frameVP, 64); g_haveLastGoodVP = true; }
    g_haveFrameVP = false; g_vpVotes.clear(); g_patchedRegions.clear(); g_patchedDraws = 0; g_matrixMisses = 0;
    jitter_ndc(g_prevJitNdc);   // this frame's offsets become 'previous' for the next velocity pass
    advance_jitter();
    { std::lock_guard<std::mutex> lock(g_clMutex); for (auto& kv : g_cl) kv.second.bb_draws = 0; }
    if (g_frame % 120 == 0) reload_config();
}
static void on_present(command_queue* queue, swapchain*, const rect*, const rect*, uint32_t, const rect*)
{
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
    logmsg("swapchain %s: %ux%u fmt=%u (%u buffers)", resize ? "resized" : "created", g_bbW, g_bbH, (unsigned)d.texture.format, sc->get_back_buffer_count());
}
static void on_destroy_swapchain(swapchain*, bool) { g_backbuffers.clear(); }
static void on_init_device(device* dev)
{
    logmsg("device created: api=%u (d3d12=%u)", (unsigned)dev->get_api(), (unsigned)device_api::d3d12);
    if (dev->get_api() != device_api::d3d12) { logmsg("not D3D12 - add-on inactive (install MGS4_D3D12.asi)"); g_cfgEnabled = 0; return; }
    g_d3d = reinterpret_cast<ID3D12Device*>(dev->get_native());
    objmv::init(g_d3d, logmsg);   // hooks root signature / PSO creation: must precede the game's pipelines
    // Streamline (frame generation) is only loaded when FrameGen is enabled at startup: it takes over the swapchain,
    // so everything else (ReShade, the DLAA path, NGX add-ons) must be known to work with it before it is on by default.
    if (g_cfgFgMode != 0) { fg::init(g_d3d, g_gameDirW, logmsg); fg::set_frame_callback(frame_rollover); }   // before the game creates its swapchain
    else logmsg("frame generation off at startup: Streamline not loaded (set FrameGen in the ini / overlay and restart to use it)");
    if (g_cfgEnabled && g_cfgMode != NVSDK_NGX_PerfQuality_Value_DLAA) {
        if (!g_internalW) logmsg("Mode=%s needs InternalRes; it will be detected and written to the ini this run - restart afterwards", g_cfgModeName);
        else if (ngx_init(dev)) setup_scaling();
    }
}
static void on_destroy_device(device* dev)
{
    release_dlss_resources(dev);
    if (g_mvCb) { g_mvCb->Unmap(0, nullptr); g_mvCb->Release(); g_mvCb = nullptr; g_mvCbPtr = nullptr; }
    if (g_dummyUav) { g_dummyUav->Release(); g_dummyUav = nullptr; }
    if (g_mvHeap) { g_mvHeap->Release(); g_mvHeap = nullptr; }
    if (g_mvPso) { g_mvPso->Release(); g_mvPso = nullptr; }
    if (g_visPso) { g_visPso->Release(); g_visPso = nullptr; }
    if (g_mvRootSig) { g_mvRootSig->Release(); g_mvRootSig = nullptr; }
    g_mvReady = false; g_mvInitTried = false;
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
    g_cfgLastDebugMode = -1;
    reload_config();
    logmsg("config: Enabled=%d Mode=%s InternalRes=%ux%u Preset=%d Sharpness=%d%% DebugMode=%d", g_cfgEnabled, g_cfgModeName, g_internalW, g_internalH, g_cfgPreset, g_cfgSharpness100, g_cfgDebugMode);
}

// ---- overlay (ReShade Add-ons tab) ---------------------------------------------------------------------------------
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
    if (g_uiMode != active)
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Restart the game to switch to %s (active now: %s)", kModeNames[g_uiMode], kModeNames[active]);
    int preset = g_cfgPreset == 10 ? 0 : 1; const char* presets[] = { "J", "K (transformer, default)" };
    if (ImGui::Combo("DLSS preset", &preset, presets, 2)) { g_cfgPreset = preset == 0 ? 10 : 11; write_ini_int("Preset", g_cfgPreset); g_recreateRequested = true; }
    if (ImGui::SliderInt("Sharpness", &g_cfgSharpness100, 0, 100, "%d%%")) write_ini_int("Sharpness", g_cfgSharpness100);
    const char* dbg[] = { "Off", "Magenta path test", "Bypass DLSS (A/B)", "Trace 3 frames", "Analyse draw constants", "Visualise motion vectors", "Visualise UI layer (frame generation)", "Visualise HUD-less colour (frame generation)" };
    int d = g_cfgDebugMode >= 0 && g_cfgDebugMode <= 7 ? g_cfgDebugMode : 0;
    if (ImGui::Combo("Debug", &d, dbg, 8)) { write_ini_int("DebugMode", d); reload_config(); }
    ImGui::Separator();
    ImGui::Text("NGX: %s", g_ngxReady ? "ready" : (g_ngxInitTried ? "FAILED" : "not initialised yet"));
    if (g_dlss) ImGui::Text("Feature: %s  %ux%u -> %ux%u, preset %s", g_cfgModeName, g_dlssW, g_dlssH, g_dlssOutW, g_dlssOutH, g_cfgPreset == 10 ? "J" : "K");
    else ImGui::Text("Feature: none yet");
    ImGui::Text("Evaluations: %u  (%.0f/s)", g_evalCount, g_evalRate);
    ImGui::Text("Internal res %ux%u, render %ux%u%s", g_internalW, g_internalH, g_scaling ? g_renderW : g_internalW, g_scaling ? g_renderH : g_internalH, g_scaling ? " (textures shrunk)" : "");
    bool drs = g_cfgDRS != 0;
    if (ImGui::Checkbox("Handle the game's dynamic resolution (DLSS on the scene sub-rect; restart to change)", &drs)) { g_cfgDRS = drs ? 1 : 0; write_ini_int("DRS", g_cfgDRS); }
    if (g_sceneVpValid && g_internalW) {
        const float fx = g_sceneVp.width / float(g_internalW);
        if (fx < 0.995f || g_drsActiveLast) ImGui::TextColored(g_cfgDRS ? ImVec4(0.6f, 0.9f, 0.6f, 1.0f) : ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "Game dynamic resolution: scene viewport %.0fx%.0f (%.0f%%) -> %s (%u frames so far)", g_sceneVp.width, g_sceneVp.height, fx * 100.0f, g_cfgDRS ? "DLSS evaluates that sub-rect, output resampled back into it" : "NOT handled (DRS=0): expect smearing while the game changes resolution", g_drsFrames);
        else ImGui::Text("Game dynamic resolution: scene viewport at full size (%.0fx%.0f)", g_sceneVp.width, g_sceneVp.height);
    }
    bool jit = g_cfgJitter != 0;
    if (ImGui::Checkbox("Camera jitter (Halton, patched into draw constants)", &jit)) { g_cfgJitter = jit ? 1 : 0; write_ini_int("Jitter", g_cfgJitter); }
    bool mvs = g_cfgMotionVectors != 0;
    if (ImGui::Checkbox("Camera motion vectors (reprojected from depth)", &mvs)) { g_cfgMotionVectors = mvs ? 1 : 0; write_ini_int("MotionVectors", g_cfgMotionVectors); }
    const char* ppNames[] = { "Auto (composite when a DLSS post-processing add-on such as DLSS 5 NR is loaded, else pre-post)", "Pre-post: DLAA before post-process/HUD", "Composite: DLAA on the final image" };
    int ppSel = g_cfgPrePostMode < 0 ? 0 : (g_cfgPrePostMode ? 1 : 2);
    if (ImGui::Combo("Insertion point (DLAA)", &ppSel, ppNames, 3)) { write_ini("PrePost", ppSel == 0 ? "auto" : (ppSel == 1 ? "1" : "0")); reload_config(); }
    ImGui::Text("Active: %s | DLSS post-processing add-on: %s | pre-post %u frames, composite %u frames", g_cfgPrePost ? "pre-post" : "composite", g_nrAddonLoaded ? g_nrAddonName : "none (CompositeIfLoaded list in ini)", g_prePostInjections, g_compositeInjections);
    bool dm = g_cfgDynMask != 0;
    if (ImGui::Checkbox("Character mask (skinned meshes -> bias current colour)", &dm)) { g_cfgDynMask = dm ? 1 : 0; write_ini_int("DynamicMask", g_cfgDynMask); }
    bool dp2 = g_cfgDynMaskProps != 0;
    if (ImGui::Checkbox("Also mask props with their own transform", &dp2)) { g_cfgDynMaskProps = dp2 ? 1 : 0; write_ini_int("DynamicMaskProps", g_cfgDynMaskProps); }
    bool dz = g_cfgDynZeroMV != 0;
    if (ImGui::Checkbox("Zero motion on masked objects (third-person camera turns)", &dz)) { g_cfgDynZeroMV = dz ? 1 : 0; write_ini_int("DynamicZeroMV", g_cfgDynZeroMV); }
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
            if (!st.dynamicSupported && st.initialised) ImGui::TextWrapped("Driver-side dynamic multi-frame generation is not reported as available; the add-on picks 2x/3x/4x itself from the measured game frame rate (now %ux).", st.adaptiveFrames + 1);
        }
        const char* rfNames[] = { "Off", "On", "On + Boost" };
        int rf = g_cfgReflex;
        if (ImGui::Combo("NVIDIA Reflex", &rf, rfNames, 3)) { write_ini_int("Reflex", rf); reload_config(); }
        if (!st.loaded) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "Streamline runtime (sl.interposer.dll, sl.dlss_g.dll, ...) not found next to mgs4.exe");
        else if (!st.initialised) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "Streamline failed to initialise: %s", st.lastError);
        else if (!st.supported) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "DLSS Frame Generation not supported: %s", st.lastError[0] ? st.lastError : "adapter/driver");
        else if (!st.swapchainProxied) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "Swapchain was not created through Streamline (restart the game)");
        else {
            ImGui::Text("%s | status 0x%X | max %ux | dynamic MFG %s | vsync %s | VRAM %.0f MB", st.slVersion, st.statusFlags, st.maxFrames + 1, st.dynamicSupported ? "yes" : "no", st.vsyncSupported ? "ok" : "off required", st.vramBytes / 1048576.0);
            ImGui::Text("Presented %u frames | HUD-less colour: %s", st.framesPresented, g_cfgPrePost ? "yes (pre-post)" : "composite: DLAA output + replayed UI layer");
            if (!g_cfgPrePost) ImGui::Text("UI layer: %u HUD draws replayed last frame, %u post passes skipped; HUD-less frames built %u", g_uiDrawsLast, g_uiPostSkippedLast, g_hudlessFrames);
            if (st.lastError[0]) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "%s", st.lastError);
        }
    }
    ImGui::Text("Jitter (%.3f, %.3f) px | VP: %s | MV pass: %s, %u resets | cam delta rot %.3f pos %.0f", g_jitterX, g_jitterY, g_havePrevVP ? "found" : "missing",
                g_mvReady ? "ok" : (g_mvInitTried ? "FAILED" : "idle"), g_mvResets, g_camDeltaRot, g_camDeltaPos);
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
        logmsg("mgs4_dlss v4 registered (header API %u)", RESHADE_API_VERSION);
        load_config();
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
        reshade::register_event<reshade::addon_event::copy_resource>(on_copy_resource);
        reshade::register_event<reshade::addon_event::copy_texture_region>(on_copy_texture_region);
        reshade::register_event<reshade::addon_event::present>(on_present);
        reshade::register_overlay(nullptr, draw_overlay);
        break; }
    case DLL_PROCESS_DETACH:
        reshade::unregister_overlay(nullptr, draw_overlay);
        reshade::unregister_addon(hModule);
        if (g_log) fclose(g_log);
        break;
    }
    return TRUE;
}
