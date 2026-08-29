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
#include <reshade.hpp>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
static int g_cfgRecreateAfter = 120;
static int g_cfgDebugMode = 0;        // 0 normal, 1 = paint the displayed texture magenta, 2 = bypass DLSS, 3 = trace 3 frames again
static int g_cfgLastDebugMode = 0;
static NVSDK_NGX_PerfQuality_Value g_cfgMode = NVSDK_NGX_PerfQuality_Value_DLAA;
static char g_cfgModeName[32] = "DLAA";
static uint32_t g_internalW = 0, g_internalH = 0;   // from ini InternalRes (size of the game's render targets)

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
    uint32_t rt_w = 0, rt_h = 0;
    bool rt_scaled = false;             // current RT/DS is one of the shrunk textures
    viewport vp = {};                   // as set by the game (unscaled)
    rect sc = {};
    bool vp_valid = false, sc_valid = false;
    descriptor_table tables[5] = {};
    bool table_set[5] = {};
    D3D12_GPU_VIRTUAL_ADDRESS cbv[5] = {};
    bool cbv_set[5] = {};
    ID3D12RootSignature* root_sig = nullptr;
    ID3D12PipelineState* pso = nullptr;
    uint32_t bb_draws = 0;
};
static std::unordered_map<command_list*, cl_state> g_cl;
static std::mutex g_clMutex;

static ID3D12Device* g_d3d = nullptr;
static bool tracing();
static bool scene_sized(device* dev, resource r, resource_desc* out = nullptr);
static std::string desc_str(device* dev, resource r);

// ---- resolution scaling (upscaling modes) ---------------------------------------------------------------------------
static uint32_t g_renderW = 0, g_renderH = 0;       // DLSS render resolution (== internal in DLAA mode)
static bool g_scaling = false;                      // render != internal
static std::unordered_set<uint64_t> g_scaledTex;    // resources created at the shrunk size
static std::mutex g_scaledMutex;
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
static resource lookup_view(device* dev, uint64_t cpu)
{
    {
        std::lock_guard<std::mutex> lock(g_cvMutex);
        auto it = g_copiedViews.find(cpu);
        if (it != g_copiedViews.end()) return resource{ it->second };
    }
    return dev->get_resource_from_view(resource_view{ cpu });
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
static NVSDK_NGX_Handle* g_oldFeature = nullptr;
static uint32_t g_oldFeatureFrame = 0;

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
    info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_OFF;

    wchar_t appData[MAX_PATH]; swprintf_s(appData, L"%s\\logs", g_gameDirW);
    NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init_with_ProjectID("7a2f8c3e-5d41-4b9a-9e0c-3f6d2b1a8c47", NVSDK_NGX_ENGINE_TYPE_CUSTOM, "0.1",
                                                            appData, g_d3d, &info, NVSDK_NGX_Version_API);
    logmsg("NGX D3D12 Init_with_ProjectID -> %s", ngx_str(r));
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
    if (g_mv.handle) { dev->destroy_resource(g_mv); g_mv = { 0 }; }
    if (g_out.handle) { dev->destroy_resource(g_out); g_out = { 0 }; }
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
    cp.Feature.InPerfQualityValue = g_cfgMode;
    cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
    ID3D12GraphicsCommandList* native = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd->get_native());
    NVSDK_NGX_Handle* handle = nullptr;
    NVSDK_NGX_Result r = NGX_D3D12_CREATE_DLSS_EXT(native, 1, 1, &handle, g_ngxParams, &cp);
    logmsg("NGX CreateFeature DLSS (%s %ux%u -> %ux%u, fmt=%u, preset=%d) -> %s", g_cfgModeName, w, h, outW, outH, (unsigned)fmt, g_cfgPreset, ngx_str(r));
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

    if (!dev->create_resource(resource_desc(w, h, 1, 1, format::r16g16_float, 1, memory_heap::default_, resource_usage::render_target | resource_usage::shader_resource),
                              nullptr, resource_usage::render_target, &g_mv)) { logmsg("create MV texture failed"); return false; }
    if (!dev->create_resource(resource_desc(outW, outH, 1, 1, fmt, 1, memory_heap::default_, resource_usage::unordered_access | resource_usage::copy_source | resource_usage::shader_resource),
                              nullptr, resource_usage::unordered_access, &g_out)) { logmsg("create output texture failed"); return false; }
    g_outState = resource_usage::unordered_access;
    dev->set_resource_name(g_mv, "MGS4DLSS motion vectors");
    dev->set_resource_name(g_out, "MGS4DLSS output");

    resource_view rtv = { 0 };
    if (dev->create_resource_view(g_mv, resource_usage::render_target, resource_view_desc(format::r16g16_float), &rtv)) {
        const float zero[4] = { 0, 0, 0, 0 };
        cmd->clear_render_target_view(rtv, zero);
        dev->destroy_resource_view(rtv);
    }
    cmd->barrier(g_mv, resource_usage::render_target, resource_usage::shader_resource_non_pixel);
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
}

static resource pick_depth(device* dev, resource color)
{
    auto itDs = g_dsForRt.find(color.handle);
    resource depth = itDs != g_dsForRt.end() ? resource{ itDs->second } : resource{ 0 };
    if (!depth.handle) {
        uint32_t best = 0;
        for (auto& kv : g_drawsPerDs) if (kv.second > best) { best = kv.second; depth = resource{ kv.first }; }
    }
    return depth;
}

// Runs DLSS on `color` (state as bgfx tracks it) and either copies the result back over it (DLAA) or rewrites the SRV
// descriptor at `srvCpu` so the composite draw samples the full-size output (upscaling modes).
static void run_dlss(command_list* cmd, const cl_state* restore, resource color, resource_usage colorState, uint64_t srvCpu)
{
    device* dev = cmd->get_device();
    if (!ngx_init(dev)) return;
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
    if (!loggedOnce) { loggedOnce = true; logmsg("injecting: color=%p %ux%u fmt=%u depth=%p fmt=%u -> output %ux%u (%s) frame=%u", (void*)color.handle, cd.texture.width, cd.texture.height, (unsigned)cd.texture.format, (void*)depth.handle, (unsigned)dd.texture.format, outW, outH, upscale ? "SRV redirect" : "copy back", g_frame); }

    {
        const resource res[3] = { color, depth, g_out };
        const resource_usage from[3] = { colorState, resource_usage::depth_stencil_write, g_outState };
        const resource_usage to[3] = { resource_usage::shader_resource_non_pixel, resource_usage::shader_resource_non_pixel, resource_usage::unordered_access };
        cmd->barrier(3, res, from, to); g_outState = resource_usage::unordered_access;
    }
    ID3D12GraphicsCommandList* native = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd->get_native());
    NVSDK_NGX_D3D12_DLSS_Eval_Params ep = {};
    ep.Feature.pInColor = reinterpret_cast<ID3D12Resource*>(color.handle);
    ep.Feature.pInOutput = reinterpret_cast<ID3D12Resource*>(g_out.handle);
    ep.Feature.InSharpness = g_cfgSharpness100 / 100.0f;
    ep.pInDepth = reinterpret_cast<ID3D12Resource*>(depth.handle);
    ep.pInMotionVectors = reinterpret_cast<ID3D12Resource*>(g_mv.handle);
    ep.InJitterOffsetX = 0.0f; ep.InJitterOffsetY = 0.0f;
    ep.InRenderSubrectDimensions = { g_dlssW, g_dlssH };
    ep.InReset = (g_frame <= g_createdFrame + 1) ? 1 : 0;
    ep.InMVScaleX = 1.0f; ep.InMVScaleY = 1.0f;
    ep.InPreExposure = 1.0f; ep.InExposureScale = 1.0f;
    NVSDK_NGX_Result r = NGX_D3D12_EVALUATE_DLSS_EXT(native, g_dlss, g_ngxParams, &ep);
    if (NVSDK_NGX_FAILED(r)) { if (g_failCount++ < 5) logmsg("NGX EvaluateFeature -> %s", ngx_str(r)); }
    else { if (++g_evalCount == 1 || (g_cfgLogEveryN && g_evalCount % g_cfgLogEveryN == 0)) logmsg("NGX EvaluateFeature ok (#%u)", g_evalCount); }

    if (!g_recreated && g_cfgRecreateAfter > 0 && g_evalCount >= (uint32_t)g_cfgRecreateAfter) {
        g_recreated = true;
        g_oldFeature = g_dlss; g_oldFeatureFrame = g_frame; g_dlss = nullptr;
        logmsg("re-creating DLSS feature so NGX-hooking add-ons capture CreateFeature (old feature released in a few frames)");
    }

    if (upscale) {
        // Composite draw samples the full-size output instead of the shrunk texture: rewrite its SRV descriptor in place.
        const resource res[2] = { color, depth };
        const resource_usage from[2] = { resource_usage::shader_resource_non_pixel, resource_usage::shader_resource_non_pixel };
        const resource_usage to[2] = { colorState, resource_usage::depth_stencil_write };
        cmd->barrier(2, res, from, to);
        cmd->barrier(g_out, resource_usage::unordered_access, resource_usage::shader_resource_pixel); g_outState = resource_usage::shader_resource_pixel;
        if (!NVSDK_NGX_FAILED(r)) g_d3d->CreateShaderResourceView(reinterpret_cast<ID3D12Resource*>(g_out.handle), nullptr, D3D12_CPU_DESCRIPTOR_HANDLE{ (SIZE_T)srvCpu });
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
        if (dev->create_resource_view(target, resource_usage::render_target, resource_view_desc(cd.texture.format), &rtv)) {
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
    if (g_internalW == g_bbW && g_internalH == g_bbH && (desc.usage & resource_usage::render_target) == 0) return false;
    desc.texture.width = g_renderW; desc.texture.height = g_renderH;
    return true;   // desc modified
}
static void on_init_resource(device* dev, const resource_desc& desc, const subresource_data*, resource_usage, resource res)
{
    if (!g_scaling || desc.type != resource_type::texture_2d) return;
    if (desc.texture.width == g_renderW && desc.texture.height == g_renderH) {
        std::lock_guard<std::mutex> lock(g_scaledMutex);
        g_scaledTex.insert(res.handle);
        static int n = 0; if (n++ < 12) logmsg("shrunk texture %p to %ux%u (fmt=%u usage=0x%X)", (void*)res.handle, desc.texture.width, desc.texture.height, (unsigned)desc.texture.format, (unsigned)desc.usage);
    }
}
static void on_destroy_resource(device*, resource res)
{
    std::lock_guard<std::mutex> lock(g_scaledMutex);
    g_scaledTex.erase(res.handle);
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
    device* dev = cmd->get_device();
    resource rt = (count > 0 && rtvs[0].handle) ? dev->get_resource_from_view(rtvs[0]) : resource{ 0 };
    resource ds = dsv.handle ? dev->get_resource_from_view(dsv) : resource{ 0 };
    std::lock_guard<std::mutex> lock(g_clMutex);
    cl_state& s = g_cl[cmd];
    s.rt = rt; s.ds = ds; s.rt_w = s.rt_h = 0;
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
    resource_desc d = dev->get_resource_desc(r);
    char b[96]; snprintf(b, sizeof(b), "%p %ux%u f%u", (void*)r.handle, d.texture.width, d.texture.height, (unsigned)d.texture.format);
    return b;
}
static bool is_backbuffer(resource r) { return g_backbuffers.count(r.handle) != 0; }
static bool scene_sized(device* dev, resource r, resource_desc* out)
{
    if (!r.handle || is_backbuffer(r)) return false;
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

static void handle_draw(command_list* cmd)
{
    cl_state s;
    {
        std::lock_guard<std::mutex> lock(g_clMutex);
        s = g_cl[cmd];
    }
    if (!s.rt.handle || g_bbW == 0) return;
    device* dev = cmd->get_device();
    if (!is_backbuffer(s.rt)) {
        if (s.rt_w >= 640 && s.rt_h >= 360) {
            g_sceneDrawsThisFrame++;
            g_drawsPerRt[s.rt.handle]++;
            if (s.ds.handle) { g_dsForRt[s.rt.handle] = s.ds.handle; g_drawsPerDs[s.ds.handle]++; }
        }
        return;
    }
    if (g_sceneDrawsThisFrame < 20) return;
    if (!g_traceArmed) { g_traceArmed = true; g_traceUntil = g_frame + 3; logmsg("tracing backbuffer draws/copies for frames %u..%u", g_frame, g_traceUntil - 1); }

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

    if (g_injectedThisFrame || !g_cfgEnabled || !color.handle) return;
    g_injectedThisFrame = true;
    resource_desc cd = dev->get_resource_desc(color);
    if (!g_scaling || !is_scaled(color)) remember_internal_res(cd.texture.width, cd.texture.height);   // game's real render size (changed in-game?)
    run_dlss(cmd, &s, color, resource_usage::shader_resource_pixel, srvCpu);
}
static bool on_draw(command_list* cmd, uint32_t, uint32_t, uint32_t, uint32_t) { handle_draw(cmd); return false; }
static bool on_draw_indexed(command_list* cmd, uint32_t, uint32_t, uint32_t, int32_t, uint32_t) { handle_draw(cmd); return false; }

static void handle_copy(command_list* cmd, resource src, resource dst, const char* what)
{
    if (g_bbW == 0 || !is_backbuffer(dst)) return;
    device* dev = cmd->get_device();
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
    if (g_cfgDebugMode != g_cfgLastDebugMode) {
        logmsg("DebugMode -> %d", g_cfgDebugMode); g_cfgLastDebugMode = g_cfgDebugMode;
        if (g_cfgDebugMode == 3) { g_traceUntil = g_frame + 3; logmsg("tracing backbuffer draws/copies for frames %u..%u", g_frame, g_traceUntil - 1); }
    }
}

static void on_present(command_queue*, swapchain*, const rect*, const rect*, uint32_t, const rect*)
{
    g_frame++;
    if (g_oldFeature && g_frame > g_oldFeatureFrame + 6) {
        NVSDK_NGX_Result r = NVSDK_NGX_D3D12_ReleaseFeature(g_oldFeature); g_oldFeature = nullptr;
        logmsg("released previous DLSS feature -> %s", ngx_str(r));
    }
    if (tracing()) { logmsg("f%u: %u shader-view creations, %u descriptor copies this frame", g_frame - 1, g_viewEvents, g_copyEvents); g_viewSamples = 0; }
    g_viewEvents = 0; g_copyEvents = 0;
    g_injectedThisFrame = false; g_featureCreatedThisFrame = false;
    g_sceneDrawsThisFrame = 0;
    g_drawsPerRt.clear(); g_drawsPerDs.clear();
    { std::lock_guard<std::mutex> lock(g_clMutex); for (auto& kv : g_cl) kv.second.bb_draws = 0; }
    if (g_frame % 120 == 0) reload_config();
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
    if (g_cfgEnabled && g_cfgMode != NVSDK_NGX_PerfQuality_Value_DLAA) {
        if (!g_internalW) logmsg("Mode=%s needs InternalRes; it will be detected and written to the ini this run - restart afterwards", g_cfgModeName);
        else if (ngx_init(dev)) setup_scaling();
    }
}
static void on_destroy_device(device* dev)
{
    release_dlss_resources(dev);
    if (g_ngxParams) { NVSDK_NGX_D3D12_DestroyParameters(g_ngxParams); g_ngxParams = nullptr; }
    if (g_ngxReady) { NVSDK_NGX_D3D12_Shutdown1(g_d3d); g_ngxReady = false; }
}

static void load_config()
{
    snprintf(g_iniPath, MAX_PATH, "%s\\mgs4_dlss.ini", g_gameDir);
    g_cfgPreset = GetPrivateProfileIntA("DLSS", "Preset", 11, g_iniPath);
    g_cfgLogEveryN = GetPrivateProfileIntA("DLSS", "LogEveryN", 600, g_iniPath);
    g_cfgRecreateAfter = GetPrivateProfileIntA("DLSS", "RecreateAfter", 120, g_iniPath);
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
        reshade::register_event<reshade::addon_event::init_resource_view>(on_init_resource_view);
        reshade::register_event<reshade::addon_event::copy_descriptor_tables>(on_copy_descriptor_tables);
        reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind_rts);
        reshade::register_event<reshade::addon_event::bind_viewports>(on_bind_viewports);
        reshade::register_event<reshade::addon_event::bind_scissor_rects>(on_bind_scissor_rects);
        reshade::register_event<reshade::addon_event::bind_descriptor_tables>(on_bind_descriptor_tables);
        reshade::register_event<reshade::addon_event::push_descriptors>(on_push_descriptors);
        reshade::register_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
        reshade::register_event<reshade::addon_event::draw>(on_draw);
        reshade::register_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
        reshade::register_event<reshade::addon_event::copy_resource>(on_copy_resource);
        reshade::register_event<reshade::addon_event::copy_texture_region>(on_copy_texture_region);
        reshade::register_event<reshade::addon_event::present>(on_present);
        break; }
    case DLL_PROCESS_DETACH:
        reshade::unregister_addon(hModule);
        if (g_log) fclose(g_log);
        break;
    }
    return TRUE;
}
