// mgs4_dlss.addon64 - injects NGX DLSS (DLAA) into Metal Gear Solid 4 (Master Collection, bgfx on D3D12).
//
// How it works (see docs/renderer-notes.md):
//  * The port renders the 3D scene into 3840x2160 RGBA8 targets (shared R24G8 depth), then gets that image into the
//    swapchain-sized backbuffer (a fullscreen draw sampling it, or a copy), followed by the UI.
//  * We intercept the first draw/copy into the backbuffer-sized target that consumes a scene-sized texture, run DLSS on
//    that texture with the scene depth, copy the result back over it, and restore bgfx's command-list state
//    (heaps, root signature, PSO, root params 0..4) so the following draws are unaffected.
//  * NGX calls go through the standard _nvngx.dll exports, so NGX-hooking add-ons (renodx-dlss5) see them. Those
//    add-ons install their hooks when nvngx_dlss.dll loads (inside our first CreateFeature), so we re-create the
//    feature once after a few frames to let them capture CreateFeature.
//  * v1: zero motion vectors and zero jitter. Jitter + camera MVs come next.
// Log: <game>\logs\mgs4_dlss.log. Config: <game>\mgs4_dlss.ini (live-reloaded every ~second).
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
#include <mutex>

using namespace reshade::api;

// ---- logging / config --------------------------------------------------------------------------------------------
static FILE* g_log = nullptr;
static std::mutex g_logMutex;
static char g_gameDir[MAX_PATH];
static wchar_t g_gameDirW[MAX_PATH];
static int g_cfgEnabled = 1;
static int g_cfgPreset = 11;          // NVSDK_NGX_DLSS_Hint_Render_Preset_K (transformer)
static int g_cfgSharpness100 = 0;
static int g_cfgLogEveryN = 600;
static int g_cfgRecreateAfter = 120;  // evaluations; re-create the feature once so late NGX hooks see CreateFeature
static int g_cfgDebugMode = 0;        // 0 normal, 1 = paint the written-back texture magenta, 2 = bypass DLSS, 3 = trace 3 frames again
static int g_cfgLastDebugMode = 0;

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
    viewport vp = {};
    descriptor_table tables[5] = {};
    bool table_set[5] = {};
    D3D12_GPU_VIRTUAL_ADDRESS cbv[5] = {};
    bool cbv_set[5] = {};
    ID3D12RootSignature* root_sig = nullptr;
    ID3D12PipelineState* pso = nullptr;
    uint32_t bb_draws = 0;      // draws into the backbuffer-sized RT on this list this frame
};
static std::unordered_map<command_list*, cl_state> g_cl;
static std::mutex g_clMutex;

// ---- descriptor resolution ----------------------------------------------------------------------------------------
// ReShade hands the application virtual CPU descriptor handles ((heap_index << 28) | byte_offset | heap_type) and keys
// its view map by them. We learn each original heap's virtual base from view-creation / descriptor-copy events, then
// turn any bound GPU table into the virtual handle of its descriptors and ask ReShade for the resource.
static ID3D12Device* g_d3d = nullptr;
static bool tracing();
static bool scene_sized(device* dev, resource r, resource_desc* out = nullptr);
static std::string desc_str(device* dev, resource r);

// ReShade registers views created with Create*View under their ORIGINAL CPU descriptor handle, but for CBV/SRV/UAV heaps
// its CopyDescriptors hooks do NOT register the destination slots (they only fire copy_descriptor_tables). bgfx fills its
// shader-visible heap by copying, so we mirror those copies ourselves: original CPU handle of the slot -> resource.
static std::unordered_map<uint64_t, uint64_t> g_copiedViews;
static std::mutex g_cvMutex;

// Turns a descriptor_table (ReShade virtual CPU table or GPU handle) into the original CPU handle of descriptor `binding`.
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
static uint32_t g_sceneDrawsThisFrame = 0;
static bool g_injectedThisFrame = false;
static std::unordered_map<uint64_t, uint32_t> g_drawsPerRt;   // rt handle -> draws (this frame)
static std::unordered_map<uint64_t, uint32_t> g_drawsPerDs;   // ds handle -> draws (this frame)
static std::unordered_map<uint64_t, uint64_t> g_dsForRt;      // rt handle -> last depth bound with it
static uint32_t g_traceUntil = 0;                             // frame index until which we log backbuffer draws/copies
static bool g_traceArmed = false;

// ---- DLSS state ----------------------------------------------------------------------------------------------------
static device* g_dev = nullptr;
static bool g_ngxInitTried = false, g_ngxReady = false;
static NVSDK_NGX_Parameter* g_ngxParams = nullptr;
static NVSDK_NGX_Handle* g_dlss = nullptr;
static uint32_t g_dlssW = 0, g_dlssH = 0;
static format g_dlssFmt = format::unknown;
static resource g_mv = { 0 }, g_out = { 0 };
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

    r = NVSDK_NGX_D3D12_AllocateParameters(&g_ngxParams);
    if (NVSDK_NGX_FAILED(r) || !g_ngxParams) { logmsg("NGX AllocateParameters -> %s", ngx_str(r)); return false; }
    g_ngxReady = true;
    return true;
}

static void release_dlss_resources(device* dev)
{
    if (g_dlss) { NVSDK_NGX_D3D12_ReleaseFeature(g_dlss); g_dlss = nullptr; }
    if (g_mv.handle) { dev->destroy_resource(g_mv); g_mv = { 0 }; }
    if (g_out.handle) { dev->destroy_resource(g_out); g_out = { 0 }; }
}

static bool create_feature(command_list* cmd, uint32_t w, uint32_t h, format fmt)
{
    NVSDK_NGX_Parameter_SetUI(g_ngxParams, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, (unsigned)g_cfgPreset);
    NVSDK_NGX_DLSS_Create_Params cp = {};
    cp.Feature.InWidth = w; cp.Feature.InHeight = h;
    cp.Feature.InTargetWidth = w; cp.Feature.InTargetHeight = h;
    cp.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_DLAA;
    cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
    ID3D12GraphicsCommandList* native = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd->get_native());
    NVSDK_NGX_Handle* handle = nullptr;
    NVSDK_NGX_Result r = NGX_D3D12_CREATE_DLSS_EXT(native, 1, 1, &handle, g_ngxParams, &cp);
    logmsg("NGX CreateFeature DLSS (DLAA %ux%u, fmt=%u, preset=%d) -> %s", w, h, (unsigned)fmt, g_cfgPreset, ngx_str(r));
    if (NVSDK_NGX_FAILED(r)) return false;
    g_dlss = handle;
    g_dlssW = w; g_dlssH = h; g_dlssFmt = fmt;
    g_featureCreatedThisFrame = true; g_createdFrame = g_frame;
    return true;
}

static bool ensure_resources(device* dev, command_list* cmd, uint32_t w, uint32_t h, format fmt)
{
    if (g_dlss && g_dlssW == w && g_dlssH == h && g_dlssFmt == fmt) return true;
    if (!g_dlss && g_mv.handle && g_out.handle && g_dlssW == w && g_dlssH == h && g_dlssFmt == fmt)
        return create_feature(cmd, w, h, fmt);   // textures still valid, only the feature was dropped (re-create path)
    release_dlss_resources(dev);

    if (!dev->create_resource(resource_desc(w, h, 1, 1, format::r16g16_float, 1, memory_heap::default_, resource_usage::render_target | resource_usage::shader_resource),
                              nullptr, resource_usage::render_target, &g_mv)) { logmsg("create MV texture failed"); return false; }
    if (!dev->create_resource(resource_desc(w, h, 1, 1, fmt, 1, memory_heap::default_, resource_usage::unordered_access | resource_usage::copy_source | resource_usage::shader_resource),
                              nullptr, resource_usage::unordered_access, &g_out)) { logmsg("create output texture failed"); return false; }
    dev->set_resource_name(g_mv, "MGS4DLSS motion vectors");
    dev->set_resource_name(g_out, "MGS4DLSS output");

    resource_view rtv = { 0 };
    if (dev->create_resource_view(g_mv, resource_usage::render_target, resource_view_desc(format::r16g16_float), &rtv)) {
        const float zero[4] = { 0, 0, 0, 0 };
        cmd->clear_render_target_view(rtv, zero);
        dev->destroy_resource_view(rtv);
    }
    cmd->barrier(g_mv, resource_usage::render_target, resource_usage::shader_resource_non_pixel);
    g_dlssW = w; g_dlssH = h; g_dlssFmt = fmt;
    return create_feature(cmd, w, h, fmt);
}

// Re-apply what bgfx believes is bound on this command list (NGX clobbers heaps / root signature / PSO).
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
    if (!depth.handle) {   // color is a post-process output: use the depth with the most draws this frame
        uint32_t best = 0;
        for (auto& kv : g_drawsPerDs) if (kv.second > best) { best = kv.second; depth = resource{ kv.first }; }
    }
    return depth;
}

// Runs DLSS on `color` (state as bgfx tracks it: `colorState`) and copies the result back over it.
static void run_dlss(command_list* cmd, const cl_state* restore, resource color, resource_usage colorState)
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
    if (g_cfgDebugMode == 2) return;   // bypass

    if (!ensure_resources(dev, cmd, cd.texture.width, cd.texture.height, cd.texture.format)) return;
    if (g_featureCreatedThisFrame) { if (restore) restore_state(dev, cmd, *restore); return; }   // evaluate from the next frame on

    static bool loggedOnce = false;
    if (!loggedOnce) { loggedOnce = true; logmsg("injecting: color=%p %ux%u fmt=%u  depth=%p fmt=%u  frame=%u", (void*)color.handle, cd.texture.width, cd.texture.height, (unsigned)cd.texture.format, (void*)depth.handle, (unsigned)dd.texture.format, g_frame); }

    {
        const resource res[2] = { color, depth };
        const resource_usage from[2] = { colorState, resource_usage::depth_stencil_write };
        const resource_usage to[2] = { resource_usage::shader_resource_non_pixel, resource_usage::shader_resource_non_pixel };
        cmd->barrier(2, res, from, to);
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

    {
        const resource res[3] = { color, depth, g_out };
        const resource_usage from[3] = { resource_usage::shader_resource_non_pixel, resource_usage::shader_resource_non_pixel, resource_usage::unordered_access };
        const resource_usage to[3] = { resource_usage::copy_dest, resource_usage::depth_stencil_write, resource_usage::copy_source };
        cmd->barrier(3, res, from, to);
    }
    if (!NVSDK_NGX_FAILED(r)) cmd->copy_resource(g_out, color);
    {
        const resource res[2] = { color, g_out };
        const resource_usage from[2] = { resource_usage::copy_dest, resource_usage::copy_source };
        const resource_usage to[2] = { colorState, resource_usage::unordered_access };
        cmd->barrier(2, res, from, to);
    }
    if (g_cfgDebugMode == 1) {
        resource_view rtv = { 0 };
        if (dev->create_resource_view(color, resource_usage::render_target, resource_view_desc(cd.texture.format), &rtv)) {
            const float magenta[4] = { 1.0f, 0.0f, 1.0f, 1.0f };
            cmd->barrier(color, colorState, resource_usage::render_target);
            cmd->clear_render_target_view(rtv, magenta);
            cmd->barrier(color, resource_usage::render_target, colorState);
            dev->destroy_resource_view(rtv);
        } else { static bool once = false; if (!once) { once = true; logmsg("debug: could not create RTV on color %p", (void*)color.handle); } }
    }
    if (restore) restore_state(dev, cmd, *restore);
}

// ---- event handlers -------------------------------------------------------------------------------------------------
static uint32_t g_viewEvents = 0, g_copyEvents = 0, g_viewSamples = 0;
static void on_init_resource_view(device* dev, resource res, resource_usage usage, const resource_view_desc&, resource_view view)
{
    if (dev->get_api() != device_api::d3d12) return;
    if ((usage & (resource_usage::shader_resource | resource_usage::unordered_access)) == 0) return;
    g_viewEvents++;
    if (tracing() && g_viewSamples < 6 && scene_sized(dev, res)) {
        g_viewSamples++;
        logmsg("   init_resource_view: handle=%llx -> %s (lookup back: %p)", (unsigned long long)view.handle, desc_str(dev, res).c_str(), (void*)dev->get_resource_from_view(view).handle);
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
        if (tracing() && c == 0 && g_copyEvents <= 3)
            logmsg("   copy_descriptor_tables: src cpu=%llx -> dst cpu=%llx count=%u (src resolves to %p)", (unsigned long long)src, (unsigned long long)dst, cp.count, (void*)lookup_view(dev, src).handle);
    }
    return false;
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
}
static void on_bind_viewports(command_list* cmd, uint32_t first, uint32_t count, const viewport* vp)
{
    if (first != 0 || count == 0) return;
    std::lock_guard<std::mutex> lock(g_clMutex);
    g_cl[cmd].vp = vp[0];
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
static bool scene_sized(device* dev, resource r, resource_desc* out)
{
    if (!r.handle) return false;
    resource_desc d = dev->get_resource_desc(r);
    if (out) *out = d;
    return d.type == resource_type::texture_2d && d.texture.width >= 1280 && d.texture.width < g_bbW && d.texture.height >= 720 && d.texture.height <= g_bbH;
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
    const bool backbufferSized = s.rt_w == g_bbW && s.rt_h == g_bbH;
    if (!backbufferSized) {
        if (s.rt_w >= 1280 && s.rt_h >= 720) {
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

    // Find the scene-sized texture among the bound descriptor tables (any root param, first 4 descriptors).
    resource color = { 0 }; int colorParam = -1, colorIdx = -1;
    for (int p = 0; p < 5 && !color.handle; ++p) {
        if (!s.table_set[p]) continue;
        for (int i = 0; i < 4; ++i) {
            resource r = resolve_descriptor(dev, s.tables[p], i);
            if (scene_sized(dev, r)) { color = r; colorParam = p; colorIdx = i; break; }
        }
    }
    if (tracing() && drawIdx < 4) {
        logmsg("f%u bb-draw#%u vp=(%.0f,%.0f %.0fx%.0f) pso=%p rootsig=%p -> color=%s (param %d idx %d)", g_frame, drawIdx, s.vp.x, s.vp.y, s.vp.width, s.vp.height,
               (void*)s.pso, (void*)s.root_sig, desc_str(dev, color).c_str(), colorParam, colorIdx);
        for (int p = 0; p < 5; ++p) {
            if (s.table_set[p]) {
                char diag[128] = ""; std::string items;
                for (int i = 0; i < 4; ++i) { resource r = resolve_descriptor(dev, s.tables[p], i, i == 0 ? diag : nullptr, sizeof(diag)); items += " [" + std::to_string(i) + "]=" + desc_str(dev, r); }
                logmsg("   root[%d] table %s:%s", p, diag, items.c_str());
            } else if (s.cbv_set[p]) logmsg("   root[%d] cbv va=%llx", p, (unsigned long long)s.cbv[p]);
            else logmsg("   root[%d] unset", p);
        }
    }

    if (g_injectedThisFrame || !g_cfgEnabled) return;
    if (!color.handle) return;
    g_injectedThisFrame = true;
    if (tracing()) logmsg("f%u -> injecting at bb-draw#%u on %s", g_frame, drawIdx, desc_str(dev, color).c_str());
    run_dlss(cmd, &s, color, resource_usage::shader_resource_pixel);
}
static bool on_draw(command_list* cmd, uint32_t, uint32_t, uint32_t, uint32_t) { handle_draw(cmd); return false; }
static bool on_draw_indexed(command_list* cmd, uint32_t, uint32_t, uint32_t, int32_t, uint32_t) { handle_draw(cmd); return false; }

static void handle_copy(command_list* cmd, resource src, resource dst, const char* what)
{
    if (g_bbW == 0) return;
    device* dev = cmd->get_device();
    resource_desc dd = dev->get_resource_desc(dst);
    if (dd.type != resource_type::texture_2d || dd.texture.width != g_bbW || dd.texture.height != g_bbH) return;
    if (tracing()) logmsg("f%u %s src=%s -> backbuffer-sized dst=%s (scene draws so far %u)", g_frame, what, desc_str(dev, src).c_str(), desc_str(dev, dst).c_str(), g_sceneDrawsThisFrame);
    if (g_injectedThisFrame || !g_cfgEnabled || g_sceneDrawsThisFrame < 20 || !scene_sized(dev, src)) return;
    g_injectedThisFrame = true;
    if (tracing()) logmsg("f%u -> injecting before %s on %s", g_frame, what, desc_str(dev, src).c_str());
    cl_state s; { std::lock_guard<std::mutex> lock(g_clMutex); s = g_cl[cmd]; }
    run_dlss(cmd, &s, src, resource_usage::copy_source);
}
static bool on_copy_resource(command_list* cmd, resource src, resource dst) { handle_copy(cmd, src, dst, "copy_resource"); return false; }
static bool on_copy_texture_region(command_list* cmd, resource src, uint32_t, const subresource_box*, resource dst, uint32_t, const subresource_box*, filter_mode) { handle_copy(cmd, src, dst, "copy_texture_region"); return false; }

static void reload_config()
{
    char ini[MAX_PATH]; snprintf(ini, MAX_PATH, "%s\\mgs4_dlss.ini", g_gameDir);
    g_cfgEnabled = GetPrivateProfileIntA("DLSS", "Enabled", 1, ini);
    g_cfgSharpness100 = GetPrivateProfileIntA("DLSS", "Sharpness", 0, ini);
    g_cfgDebugMode = GetPrivateProfileIntA("DLSS", "DebugMode", 0, ini);
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
    resource_desc d = dev->get_resource_desc(sc->get_back_buffer(0));
    g_bbW = d.texture.width; g_bbH = d.texture.height;
    logmsg("swapchain %s: %ux%u fmt=%u", resize ? "resized" : "created", g_bbW, g_bbH, (unsigned)d.texture.format);
}
static void on_init_device(device* dev)
{
    g_dev = dev;
    logmsg("device created: api=%u (d3d12=%u)", (unsigned)dev->get_api(), (unsigned)device_api::d3d12);
    if (dev->get_api() != device_api::d3d12) { logmsg("not D3D12 - add-on inactive (install MGS4_D3D12.asi)"); g_cfgEnabled = 0; }
    else g_d3d = reinterpret_cast<ID3D12Device*>(dev->get_native());
}
static void on_destroy_device(device* dev)
{
    release_dlss_resources(dev);
    if (g_ngxParams) { NVSDK_NGX_D3D12_DestroyParameters(g_ngxParams); g_ngxParams = nullptr; }
    if (g_ngxReady) { NVSDK_NGX_D3D12_Shutdown1(g_d3d); g_ngxReady = false; }
}

static void load_config()
{
    char ini[MAX_PATH]; snprintf(ini, MAX_PATH, "%s\\mgs4_dlss.ini", g_gameDir);
    g_cfgPreset = GetPrivateProfileIntA("DLSS", "Preset", 11, ini);
    g_cfgLogEveryN = GetPrivateProfileIntA("DLSS", "LogEveryN", 600, ini);
    g_cfgRecreateAfter = GetPrivateProfileIntA("DLSS", "RecreateAfter", 120, ini);
    g_cfgLastDebugMode = -1;
    reload_config();
    logmsg("config: Enabled=%d Preset=%d Sharpness=%d%% DebugMode=%d", g_cfgEnabled, g_cfgPreset, g_cfgSharpness100, g_cfgDebugMode);
}

extern "C" __declspec(dllexport) const char* NAME = "MGS4 DLSS";
extern "C" __declspec(dllexport) const char* DESCRIPTION = "Injects NGX DLSS (DLAA) into Metal Gear Solid 4 (bgfx/D3D12) so NGX-based add-ons can hook it.";

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
        logmsg("mgs4_dlss v3 registered (header API %u)", RESHADE_API_VERSION);
        load_config();
        reshade::register_event<reshade::addon_event::init_device>(on_init_device);
        reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
        reshade::register_event<reshade::addon_event::init_swapchain>(on_init_swapchain);
        reshade::register_event<reshade::addon_event::init_resource_view>(on_init_resource_view);
        reshade::register_event<reshade::addon_event::copy_descriptor_tables>(on_copy_descriptor_tables);
        reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind_rts);
        reshade::register_event<reshade::addon_event::bind_viewports>(on_bind_viewports);
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
