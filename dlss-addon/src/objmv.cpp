// Per-object motion vectors via stream output of the game's own vertex shaders. See objmv.h.
#define WIN32_LEAN_AND_MEAN
#include "objmv.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include "MinHook.h"
#include "velocity_vs.h"   // g_velocity_vs
#include "velocity_ps.h"   // g_velocity_ps
#include "monitor_vs.h"    // g_monitor_vs
#include "monitor_ps.h"    // g_monitor_ps

namespace objmv {

static LogFn g_log = nullptr;
#define LOG(...) do { if (g_log) g_log(__VA_ARGS__); } while (0)

static Stats g_st;
static ID3D12Device* g_dev = nullptr;
static std::mutex g_mutex;

// ---- recorded root signatures / pipelines --------------------------------------------------------------------------
static std::unordered_set<ID3D12RootSignature*> g_soRootSigs;   // the game's root signatures that got ALLOW_STREAM_OUTPUT at creation

struct PsoRec {
    std::vector<uint8_t> vs;
    ID3D12PipelineState* soPsoUv = nullptr; bool failedUv = false;   // stream-out variant that also captures a texcoord (the monitor)
    std::vector<D3D12_INPUT_ELEMENT_DESC> elems;
    std::vector<std::string> names;          // semantic name storage for elems
    D3D12_RASTERIZER_DESC raster = {};
    D3D12_PRIMITIVE_TOPOLOGY_TYPE topo = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    D3D12_INDEX_BUFFER_STRIP_CUT_VALUE stripCut = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    UINT nodeMask = 0;
    ID3D12RootSignature* rootSig = nullptr;
    ID3D12PipelineState* soPso = nullptr;   // not owned: shared entry in g_soShared
    uint64_t shareKey = 0;
    bool failed = false, skinned = false;
    uint64_t psHash = 0, vsHash = 0;
};
static std::unordered_map<ID3D12PipelineState*, PsoRec> g_psos;
static std::string g_dumpDir;                    // shader dump directory ("" = off)
static std::unordered_set<uint64_t> g_dumped;    // bytecode hashes already written
static uint64_t fnv(const void* data, size_t n, uint64_t h);
static void dump_shader(const D3D12_SHADER_BYTECODE& bc, uint64_t hash, const char* kind)
{
    if (g_dumpDir.empty() || !bc.pShaderBytecode || !bc.BytecodeLength || !g_dumped.insert(hash ^ (kind[0] == 'p' ? 1 : 0)).second) return;
    char path[MAX_PATH]; snprintf(path, MAX_PATH, "%s\\%016llx.%s.dxbc", g_dumpDir.c_str(), (unsigned long long)hash, kind);
    FILE* f = fopen(path, "wb"); if (!f) return;
    fwrite(bc.pShaderBytecode, 1, bc.BytecodeLength, f); fclose(f);
}
struct SoShared { ID3D12PipelineState* pso = nullptr; bool failed = false; uint32_t users = 0; ID3D12PipelineState* psoUv = nullptr; bool failedUv = false; };
static std::unordered_map<uint64_t, SoShared> g_soShared;   // bgfx re-creates pipeline objects continuously; the VS + layout repeat
static uint64_t fnv(const void* data, size_t n, uint64_t h = 1469598103934665603ull) { const uint8_t* b = static_cast<const uint8_t*>(data); for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; } return h; }

typedef HRESULT (STDMETHODCALLTYPE* PFN_CreateRootSignature)(ID3D12Device*, UINT, const void*, SIZE_T, REFIID, void**);
typedef HRESULT (STDMETHODCALLTYPE* PFN_CreateGraphicsPipelineState)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
static PFN_CreateRootSignature o_CreateRootSignature = nullptr;
static PFN_CreateGraphicsPipelineState o_CreateGraphicsPipelineState = nullptr;
typedef HRESULT (STDMETHODCALLTYPE* PFN_CreatePipelineState)(ID3D12Device2*, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);
static PFN_CreatePipelineState o_CreatePipelineState = nullptr;
static thread_local bool t_inside = false;

static void remember_pso(ID3D12PipelineState* pso, const D3D12_SHADER_BYTECODE& vs, const D3D12_INPUT_LAYOUT_DESC& il, const D3D12_RASTERIZER_DESC& raster,
                         D3D12_PRIMITIVE_TOPOLOGY_TYPE topo, D3D12_INDEX_BUFFER_STRIP_CUT_VALUE stripCut, UINT nodeMask, ID3D12RootSignature* rootSig, const D3D12_SHADER_BYTECODE& ps)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    PsoRec& p = g_psos[pso];
    p = PsoRec();
    p.vsHash = fnv(vs.pShaderBytecode, vs.BytecodeLength);
    p.psHash = ps.pShaderBytecode && ps.BytecodeLength ? fnv(ps.pShaderBytecode, ps.BytecodeLength) : 0;
    dump_shader(vs, p.vsHash, "vs"); if (p.psHash) dump_shader(ps, p.psHash, "ps");
    p.vs.assign(static_cast<const uint8_t*>(vs.pShaderBytecode), static_cast<const uint8_t*>(vs.pShaderBytecode) + vs.BytecodeLength);
    p.names.reserve(il.NumElements);
    for (UINT i = 0; i < il.NumElements; ++i) {
        const D3D12_INPUT_ELEMENT_DESC& e = il.pInputElementDescs[i];
        p.names.emplace_back(e.SemanticName ? e.SemanticName : "");
        if (_stricmp(p.names.back().c_str(), "BLENDWEIGHT") == 0 || _stricmp(p.names.back().c_str(), "BLENDINDICES") == 0) p.skinned = true;
    }
    for (UINT i = 0; i < il.NumElements; ++i) { D3D12_INPUT_ELEMENT_DESC e = il.pInputElementDescs[i]; e.SemanticName = p.names[i].c_str(); p.elems.push_back(e); }
    p.raster = raster; p.topo = topo; p.stripCut = stripCut; p.nodeMask = nodeMask; p.rootSig = rootSig;
    uint64_t k = fnv(p.vs.data(), p.vs.size());
    for (const D3D12_INPUT_ELEMENT_DESC& e : p.elems) { k = fnv(e.SemanticName, strlen(e.SemanticName), k); k = fnv(&e.SemanticIndex, sizeof(e.SemanticIndex), k); k = fnv(&e.Format, sizeof(e.Format), k); k = fnv(&e.InputSlot, sizeof(e.InputSlot), k); k = fnv(&e.AlignedByteOffset, sizeof(e.AlignedByteOffset), k); k = fnv(&e.InputSlotClass, sizeof(e.InputSlotClass), k); k = fnv(&e.InstanceDataStepRate, sizeof(e.InstanceDataStepRate), k); }
    k = fnv(&rootSig, sizeof(rootSig), k); k = fnv(&topo, sizeof(topo), k); k = fnv(&stripCut, sizeof(stripCut), k);
    p.shareKey = k;
    g_st.psosSeen++;
}

// Subobject stream layout (CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT): { alignas(void*) TYPE; T value; } - the value sits at
// offset 4 or 8 depending on T's alignment and the whole entry is padded to 8 bytes.
template <typename T> static constexpr size_t so_off() { return alignof(T) > 4 ? 8 : 4; }
template <typename T> static constexpr size_t so_size() { return (so_off<T>() + sizeof(T) + 7) & ~size_t(7); }
static bool so_entry(UINT type, size_t& off, size_t& size)
{
    switch (type) {
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: off = so_off<ID3D12RootSignature*>(); size = so_size<ID3D12RootSignature*>(); return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS:
        off = so_off<D3D12_SHADER_BYTECODE>(); size = so_size<D3D12_SHADER_BYTECODE>(); return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: off = so_off<D3D12_STREAM_OUTPUT_DESC>(); size = so_size<D3D12_STREAM_OUTPUT_DESC>(); return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: off = so_off<D3D12_BLEND_DESC>(); size = so_size<D3D12_BLEND_DESC>(); return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK: off = so_off<UINT>(); size = so_size<UINT>(); return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER: off = so_off<D3D12_RASTERIZER_DESC>(); size = so_size<D3D12_RASTERIZER_DESC>(); return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: off = so_off<D3D12_DEPTH_STENCIL_DESC>(); size = so_size<D3D12_DEPTH_STENCIL_DESC>(); return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: off = so_off<D3D12_INPUT_LAYOUT_DESC>(); size = so_size<D3D12_INPUT_LAYOUT_DESC>(); return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS:
        off = so_off<UINT>(); size = so_size<UINT>(); return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: off = so_off<D3D12_RT_FORMAT_ARRAY>(); size = so_size<D3D12_RT_FORMAT_ARRAY>(); return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: off = so_off<DXGI_SAMPLE_DESC>(); size = so_size<DXGI_SAMPLE_DESC>(); return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: off = so_off<D3D12_CACHED_PIPELINE_STATE>(); size = so_size<D3D12_CACHED_PIPELINE_STATE>(); return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: off = so_off<D3D12_DEPTH_STENCIL_DESC1>(); size = so_size<D3D12_DEPTH_STENCIL_DESC1>(); return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: off = so_off<D3D12_VIEW_INSTANCING_DESC>(); size = so_size<D3D12_VIEW_INSTANCING_DESC>(); return true;
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2: off = so_off<D3D12_DEPTH_STENCIL_DESC2>(); size = so_size<D3D12_DEPTH_STENCIL_DESC2>(); return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER1: off = so_off<D3D12_RASTERIZER_DESC1>(); size = so_size<D3D12_RASTERIZER_DESC1>(); return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2: off = so_off<D3D12_RASTERIZER_DESC2>(); size = so_size<D3D12_RASTERIZER_DESC2>(); return true;
#endif
    default: return false;
    }
}

static HRESULT STDMETHODCALLTYPE hk_CreatePipelineState(ID3D12Device2* self, const D3D12_PIPELINE_STATE_STREAM_DESC* desc, REFIID riid, void** out)
{
    HRESULT hr = o_CreatePipelineState(self, desc, riid, out);
    if (FAILED(hr) || t_inside || !out || !*out || !desc || !desc->pPipelineStateSubobjectStream || riid != __uuidof(ID3D12PipelineState)) return hr;
    const uint8_t* ptr = static_cast<const uint8_t*>(desc->pPipelineStateSubobjectStream); const uint8_t* end = ptr + desc->SizeInBytes;
    D3D12_SHADER_BYTECODE vs = {}, ps = {}; D3D12_INPUT_LAYOUT_DESC il = {}; D3D12_RASTERIZER_DESC raster = {}; bool haveRaster = false;
    D3D12_PRIMITIVE_TOPOLOGY_TYPE topo = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; D3D12_INDEX_BUFFER_STRIP_CUT_VALUE cut = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    UINT nodeMask = 0; ID3D12RootSignature* rs = nullptr; bool otherStages = false, ok = true;
    while (ptr + 8 <= end) {
        const UINT type = *reinterpret_cast<const UINT*>(ptr);
        size_t off, size;
        if (!so_entry(type, off, size)) { static int n = 0; if (n++ < 3) LOG("objmv: unknown pipeline subobject type %u - pipeline not recorded", type); ok = false; break; }
        const uint8_t* v = ptr + off;
        switch (type) {
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: rs = *reinterpret_cast<ID3D12RootSignature* const*>(v); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS: vs = *reinterpret_cast<const D3D12_SHADER_BYTECODE*>(v); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS: ps = *reinterpret_cast<const D3D12_SHADER_BYTECODE*>(v); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
            if (reinterpret_cast<const D3D12_SHADER_BYTECODE*>(v)->pShaderBytecode) otherStages = true; break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER: raster = *reinterpret_cast<const D3D12_RASTERIZER_DESC*>(v); haveRaster = true; break;
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER1: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2: {
            const D3D12_RASTERIZER_DESC1* r1 = reinterpret_cast<const D3D12_RASTERIZER_DESC1*>(v);   // same leading fields
            raster.FillMode = r1->FillMode; raster.CullMode = r1->CullMode; raster.FrontCounterClockwise = r1->FrontCounterClockwise; raster.DepthClipEnable = r1->DepthClipEnable; haveRaster = true; break; }
#endif
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: il = *reinterpret_cast<const D3D12_INPUT_LAYOUT_DESC*>(v); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE: cut = *reinterpret_cast<const D3D12_INDEX_BUFFER_STRIP_CUT_VALUE*>(v); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY: topo = *reinterpret_cast<const D3D12_PRIMITIVE_TOPOLOGY_TYPE*>(v); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK: nodeMask = *reinterpret_cast<const UINT*>(v); break;
        default: break;
        }
        ptr += size;
    }
    if (!ok || !vs.pShaderBytecode || !vs.BytecodeLength || otherStages || topo != D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE) return hr;
    if (!haveRaster) { raster.FillMode = D3D12_FILL_MODE_SOLID; raster.CullMode = D3D12_CULL_MODE_BACK; raster.DepthClipEnable = TRUE; }
    remember_pso(reinterpret_cast<ID3D12PipelineState*>(*out), vs, il, raster, topo, cut, nodeMask, rs, ps);
    return hr;
}

// The game's root signatures get ALLOW_STREAM_OUTPUT added at creation, so a stream-out pipeline can be bound under the
// game's own root signature with the game's root arguments still in place (a root signature switch invalidates them all).
static HRESULT STDMETHODCALLTYPE hk_CreateRootSignature(ID3D12Device* self, UINT nodeMask, const void* blob, SIZE_T len, REFIID riid, void** out)
{
    if (!t_inside && blob && len && out && riid == __uuidof(ID3D12RootSignature)) {
        ID3D12VersionedRootSignatureDeserializer* de = nullptr;
        if (SUCCEEDED(D3D12CreateVersionedRootSignatureDeserializer(blob, len, IID_PPV_ARGS(&de))) && de) {
            const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* src = de->GetUnconvertedRootSignatureDesc();
            D3D12_VERSIONED_ROOT_SIGNATURE_DESC copy = *src;
            switch (copy.Version) {
            case D3D_ROOT_SIGNATURE_VERSION_1_0: copy.Desc_1_0.Flags |= D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT; break;
            case D3D_ROOT_SIGNATURE_VERSION_1_1: copy.Desc_1_1.Flags |= D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT; break;
            default: copy.Desc_1_2.Flags |= D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT; break;
            }
            ID3DBlob* nb = nullptr; ID3DBlob* err = nullptr;
            HRESULT hs = D3D12SerializeVersionedRootSignature(&copy, &nb, &err);
            de->Release();
            if (SUCCEEDED(hs) && nb) {
                HRESULT hr = o_CreateRootSignature(self, nodeMask, nb->GetBufferPointer(), nb->GetBufferSize(), riid, out);
                nb->Release(); if (err) err->Release();
                if (SUCCEEDED(hr) && *out) {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    g_soRootSigs.insert(reinterpret_cast<ID3D12RootSignature*>(*out));
                    g_st.rootSigsSeen++; g_st.rootSigsSoEnabled++;
                    return hr;
                }
                // fall through: create it as the game asked
            } else { if (err) err->Release(); static int n = 0; if (n++ < 3) LOG("objmv: could not add the stream-out flag to a root signature (0x%08lX)", (unsigned long)hs); }
        }
    }
    HRESULT hr = o_CreateRootSignature(self, nodeMask, blob, len, riid, out);
    if (SUCCEEDED(hr) && !t_inside && riid == __uuidof(ID3D12RootSignature)) { std::lock_guard<std::mutex> lock(g_mutex); g_st.rootSigsSeen++; }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_CreateGraphicsPipelineState(ID3D12Device* self, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID riid, void** out)
{
    HRESULT hr = o_CreateGraphicsPipelineState(self, desc, riid, out);
    if (SUCCEEDED(hr) && !t_inside && out && *out && desc && desc->VS.pShaderBytecode && desc->VS.BytecodeLength && riid == __uuidof(ID3D12PipelineState)
        && desc->PrimitiveTopologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE && !desc->GS.pShaderBytecode && !desc->HS.pShaderBytecode)
        remember_pso(reinterpret_cast<ID3D12PipelineState*>(*out), desc->VS, desc->InputLayout, desc->RasterizerState, desc->PrimitiveTopologyType, desc->IBStripCutValue, desc->NodeMask, desc->pRootSignature, desc->PS);
    return hr;
}

uint64_t pso_ps_hash(ID3D12PipelineState* pso) { std::lock_guard<std::mutex> lock(g_mutex); auto it = g_psos.find(pso); return it == g_psos.end() ? 0 : it->second.psHash; }
uint64_t pso_vs_hash(ID3D12PipelineState* pso) { std::lock_guard<std::mutex> lock(g_mutex); auto it = g_psos.find(pso); return it == g_psos.end() ? 0 : it->second.vsHash; }
void set_shader_dump_dir(const char* dir) { std::lock_guard<std::mutex> lock(g_mutex); g_dumpDir = dir ? dir : ""; if (!g_dumpDir.empty()) CreateDirectoryA(g_dumpDir.c_str(), nullptr); }

void forget_pso(ID3D12PipelineState* pso)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_psos.find(pso);
    if (it == g_psos.end()) return;
    g_psos.erase(it);   // shared stream-out variants stay cached (the same VS comes back with the next pipeline object)
}

// Stream-out variant of a game pipeline: the same vertex shader and input layout, no rasterization, bound under the
// game's (stream-out enabled) root signature. Shared by VS + layout + root signature, since bgfx hands the same draw a
// new pipeline object all the time.
static ID3D12PipelineState* so_pso(ID3D12PipelineState* pso, const PsoRec** recOut)
{
    auto it = g_psos.find(pso);
    if (it == g_psos.end()) { g_st.skipped++; return nullptr; }
    PsoRec& p = it->second;
    *recOut = &p;
    if (p.soPso || p.failed) return p.soPso;
    SoShared& sh = g_soShared[p.shareKey];
    if (sh.pso) { p.soPso = sh.pso; sh.users++; return sh.pso; }
    if (sh.failed) { p.failed = true; return nullptr; }
    p.failed = true; sh.failed = true;
    if (!p.rootSig || !g_soRootSigs.count(p.rootSig)) {
        g_st.soPsoFailures++;
        if (g_st.soPsoFailures <= 3) LOG("objmv: pipeline %p uses root signature %p without the stream-out flag (created before the hook?) - not captured", (void*)pso, (void*)p.rootSig);
        return nullptr;
    }
    static const D3D12_SO_DECLARATION_ENTRY decl[1] = { { 0, "SV_Position", 0, 0, 4, 0 } };
    static const UINT strides[1] = { 16 };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
    d.pRootSignature = p.rootSig;
    d.VS = { p.vs.data(), p.vs.size() };
    d.StreamOutput = { decl, 1, strides, 1, D3D12_SO_NO_RASTERIZED_STREAM };
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;
    d.SampleMask = UINT_MAX;
    d.RasterizerState = p.raster;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.DepthStencilState.DepthEnable = FALSE; d.DepthStencilState.StencilEnable = FALSE;
    d.InputLayout = { p.elems.data(), (UINT)p.elems.size() };
    d.IBStripCutValue = p.stripCut;
    d.PrimitiveTopologyType = p.topo;
    d.NumRenderTargets = 0;
    d.DSVFormat = DXGI_FORMAT_UNKNOWN;
    d.SampleDesc = { 1, 0 };
    d.NodeMask = p.nodeMask;
    ID3D12PipelineState* out = nullptr;
    t_inside = true;
    HRESULT hr = g_dev->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&out));
    t_inside = false;
    if (FAILED(hr) || !out) {
        g_st.soPsoFailures++;
        if (g_st.soPsoFailures <= 5) LOG("objmv: stream-out PSO for %p failed 0x%08lX (VS %zu bytes, %zu input elements%s)", (void*)pso, (unsigned long)hr, p.vs.size(), p.elems.size(), p.skinned ? ", skinned" : "");
        snprintf(g_st.lastError, sizeof(g_st.lastError), "stream-out PSO creation failed 0x%08lX", (unsigned long)hr);
        return nullptr;
    }
    p.soPso = out; p.failed = false; sh.pso = out; sh.failed = false; sh.users = 1; g_st.soPsos++;
    if (g_st.soPsos <= 3) LOG("objmv: stream-out PSO #%u created for %p (%zu input elements%s, shared by VS hash)", g_st.soPsos, (void*)pso, p.elems.size(), p.skinned ? ", skinned" : "");
    return out;
}

// The vertex shader's output signature (DXBC OSGN / OSG5 chunk): the TEXCOORD outputs, for the monitor capture.
struct VsOutput { std::string name; uint32_t index; uint8_t mask; };
static std::vector<VsOutput> vs_outputs(const std::vector<uint8_t>& vs)
{
    std::vector<VsOutput> out;
    if (vs.size() < 32 || memcmp(vs.data(), "DXBC", 4) != 0) return out;
    const uint32_t nchunks = *reinterpret_cast<const uint32_t*>(vs.data() + 28);
    for (uint32_t c = 0; c < nchunks && 32 + 4 * c + 4 <= vs.size(); ++c) {
        const uint32_t off = *reinterpret_cast<const uint32_t*>(vs.data() + 32 + 4 * c);
        if (off + 8 > vs.size()) continue;
        const char* fourcc = reinterpret_cast<const char*>(vs.data() + off);
        const bool osgn = memcmp(fourcc, "OSGN", 4) == 0, osg5 = memcmp(fourcc, "OSG5", 4) == 0;
        if (!osgn && !osg5) continue;
        const uint8_t* data = vs.data() + off + 8; const uint32_t size = *reinterpret_cast<const uint32_t*>(vs.data() + off + 4);
        if (off + 8 + size > vs.size() || size < 8) continue;
        const uint32_t count = *reinterpret_cast<const uint32_t*>(data);
        const uint32_t entry = osg5 ? 28 : 24, skip = osg5 ? 4 : 0;
        for (uint32_t i = 0; i < count && 8 + (i + 1) * entry <= size; ++i) {
            const uint8_t* e = data + 8 + i * entry + skip;
            const uint32_t nameOff = *reinterpret_cast<const uint32_t*>(e), semIdx = *reinterpret_cast<const uint32_t*>(e + 4);
            const uint8_t mask = e[20];
            if (nameOff >= size) continue;
            out.push_back({ std::string(reinterpret_cast<const char*>(data + nameOff)), semIdx, mask });
        }
        break;
    }
    return out;
}
// Stream-out variant that captures the clip position and one TEXCOORD output (32-byte stride), for the in-world
// monitor: the first TEXCOORD with at least two components (the screen's texture coordinates on every mesh seen so far).
static ID3D12PipelineState* so_pso_uv(ID3D12PipelineState* pso, const PsoRec** recOut)
{
    auto it = g_psos.find(pso);
    if (it == g_psos.end()) { g_st.skipped++; return nullptr; }
    PsoRec& p = it->second;
    *recOut = &p;
    if (p.soPsoUv || p.failedUv) return p.soPsoUv;
    SoShared& sh = g_soShared[p.shareKey];
    if (sh.psoUv) { p.soPsoUv = sh.psoUv; return sh.psoUv; }
    if (sh.failedUv) { p.failedUv = true; return nullptr; }
    p.failedUv = true; sh.failedUv = true;
    if (!p.rootSig || !g_soRootSigs.count(p.rootSig)) return nullptr;
    const std::vector<VsOutput> outs = vs_outputs(p.vs);
    const VsOutput* tex = nullptr;
    for (const VsOutput& o : outs) if (_stricmp(o.name.c_str(), "TEXCOORD") == 0 && (o.mask & 3) == 3 && (!tex || o.index < tex->index)) tex = &o;
    { std::string sig; for (const VsOutput& o : outs) { char b[64]; snprintf(b, sizeof b, " %s%u(m%u)", o.name.c_str(), o.index, o.mask); sig += b; } snprintf(g_st.monitorInfo, sizeof g_st.monitorInfo, "vs %016llx outputs:%s -> %s%u", (unsigned long long)p.vsHash, sig.c_str(), tex ? "TEXCOORD" : "none", tex ? tex->index : 0u); LOG("objmv: monitor %s", g_st.monitorInfo); }
    if (!tex) return nullptr;
    uint8_t comps = 0; for (uint8_t m = tex->mask; m & 1; m >>= 1) comps++;
    D3D12_SO_DECLARATION_ENTRY decl[2] = { { 0, "SV_Position", 0, 0, 4, 0 }, { 0, "TEXCOORD", tex->index, 0, comps, 0 } };
    const UINT strides[1] = { 32 };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
    d.pRootSignature = p.rootSig;
    d.VS = { p.vs.data(), p.vs.size() };
    d.StreamOutput = { decl, 2, strides, 1, D3D12_SO_NO_RASTERIZED_STREAM };
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;
    d.SampleMask = UINT_MAX;
    d.RasterizerState = p.raster; d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.DepthStencilState.DepthEnable = FALSE; d.DepthStencilState.StencilEnable = FALSE;
    d.InputLayout = { p.elems.data(), (UINT)p.elems.size() };
    d.IBStripCutValue = p.stripCut; d.PrimitiveTopologyType = p.topo;
    d.NumRenderTargets = 0; d.DSVFormat = DXGI_FORMAT_UNKNOWN; d.SampleDesc = { 1, 0 }; d.NodeMask = p.nodeMask;
    ID3D12PipelineState* out = nullptr;
    t_inside = true; HRESULT hr = g_dev->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&out)); t_inside = false;
    if (FAILED(hr) || !out) { LOG("objmv: monitor stream-out PSO (position + TEXCOORD%u x%u) failed 0x%08lX", tex->index, comps, (unsigned long)hr); return nullptr; }
    p.soPsoUv = out; p.failedUv = false; sh.psoUv = out; sh.failedUv = false;
    LOG("objmv: monitor stream-out PSO created (position + TEXCOORD%u, %u components)", tex->index, comps);
    return out;
}

// ---- pass resources ---------------------------------------------------------------------------------------------------
static const uint32_t kMaxCaptures = 2048;                     // per frame
static const uint32_t kCtrStride = 16;                         // bytes between the per-draw buffer-filled-size counters
static const uint64_t kSoBytes = 48ull << 20;                  // per stream-out buffer (3M vertices)
static const uint32_t kQueries = 4 + 2 * kMaxCaptures;         // 0 frame begin, 1 frame end, 2/3 velocity, then one pair per capture
static const uint32_t kRings = 4;
static ID3D12Resource* g_soBuf[2] = {};  static D3D12_RESOURCE_STATES g_soState[2] = { D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_STREAM_OUT };
static ID3D12Resource* g_ctr[2] = {};    static D3D12_RESOURCE_STATES g_ctrState[2] = { D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_STREAM_OUT };
static ID3D12Resource* g_zero = nullptr;
static ID3D12RootSignature* g_velRs = nullptr;
struct VelPso { uint64_t key; ID3D12PipelineState* pso; };
static std::vector<VelPso> g_velPsos;                          // variants: the game's cull mode / winding / depth bias, DSV format
static ID3D12DescriptorHeap* g_heap = nullptr;                 // shader visible: parity p at p*4: SRV cur pos, prev pos, cur counters, prev counters
static ID3D12Resource* g_velCb = nullptr; static uint8_t* g_velCbPtr = nullptr;   // 4 x 256 B ring
static uint32_t g_velCbSlot = 0;
static ID3D12QueryHeap* g_queries = nullptr; static ID3D12Resource* g_readback = nullptr;
static uint64_t g_tsHz = 0;
static bool g_frameStarted = false;   // captures pending this frame
static bool g_frameBegun = false;     // frame-begin timestamp written
static uint32_t g_curFrame = 0, g_captures = 0;
static uint64_t g_soBytesThisFrame = 0;
struct RingInfo { uint32_t captures = 0; bool resolved = false, vel = false; };
static RingInfo g_rings[kRings];
static double g_accSo = 0, g_accVel = 0, g_accFrame = 0, g_accCpu = 0; static uint32_t g_accN = 0; static ULONGLONG g_accT0 = 0;
static double g_cpuThisFrame = 0; static LARGE_INTEGER g_qpf = {};
static bool g_velRan = false;

// One geometry key (buffers + index range) is drawn several times a frame when several instances share a mesh - every
// PMC soldier is the same mesh. Pairing this frame's n-th occurrence with last frame's n-th went wrong whenever their
// draw order changed (depth sorting as the camera walks around them): a soldier got another soldier's previous
// positions and a frame of bogus vectors on its body. Each occurrence therefore carries a signature - the head of its
// vertex constants (per-instance transform / first bones) - and is paired with the previous frame's unclaimed
// occurrence whose signature is nearest (frame-to-frame drift is small against the distance between two instances).
static const uint32_t kAnchorN = 32;
struct Occ { uint32_t off, bound, ctr; bool jit, ownVp, claimed; D3D12_VIEWPORT vp; float anchor[kAnchorN]; };
struct Slot { uint32_t lastFrame[2] = { UINT32_MAX, UINT32_MAX }; std::vector<Occ> occ[2]; };   // per frame parity
static std::unordered_map<uint64_t, Slot> g_slots;
static uint32_t g_reordered = 0;   // pairings that were not the same occurrence index (diagnostics)
struct VelEntry { uint64_t psoKey, psoKeyManual; uint32_t curOff, prevOff, curCtr, prevCtr, bound, flags; bool ownVp; D3D12_VIEWPORT vp; int view; };
static ID3D12PipelineState* g_projPso = nullptr;   // the monitor projector pass (monitor_vs + monitor_ps, additive)
static std::vector<VelEntry> g_vel;

static bool create_buffer(ID3D12Resource** out, uint64_t size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags, const wchar_t* name)
{
    D3D12_HEAP_PROPERTIES hp = { heap, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1 };
    D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = size; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc = { 1, 0 }; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; bd.Flags = flags;
    HRESULT hr = g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, state, nullptr, IID_PPV_ARGS(out));
    if (FAILED(hr)) { LOG("objmv: buffer %ls (%llu bytes) failed 0x%08lX", name, (unsigned long long)size, (unsigned long)hr); return false; }
    (*out)->SetName(name);
    return true;
}

static void transition(ID3D12GraphicsCommandList* cl, ID3D12Resource* r, D3D12_RESOURCE_STATES* state, D3D12_RESOURCE_STATES to)
{
    if (*state == to) return;
    D3D12_RESOURCE_BARRIER b = {}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition = { r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, *state, to };
    cl->ResourceBarrier(1, &b);
    *state = to;
}

static bool create_pass_resources()
{
    QueryPerformanceFrequency(&g_qpf);
    for (int i = 0; i < 2; ++i) {
        if (!create_buffer(&g_soBuf[i], kSoBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_FLAG_NONE, i ? L"MGS4DLSS objmv positions B" : L"MGS4DLSS objmv positions A")) return false;
        if (!create_buffer(&g_ctr[i], kMaxCaptures * kCtrStride, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_FLAG_NONE, i ? L"MGS4DLSS objmv counters B" : L"MGS4DLSS objmv counters A")) return false;
    }
    if (!create_buffer(&g_zero, kMaxCaptures * kCtrStride, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, L"MGS4DLSS objmv zero")) return false;
    D3D12_RANGE none = { 0, 0 };
    { void* p = nullptr; g_zero->Map(0, &none, &p); if (p) memset(p, 0, kMaxCaptures * kCtrStride); g_zero->Unmap(0, nullptr); }
    if (!create_buffer(&g_velCb, 4 * 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, L"MGS4DLSS objmv velocity cb")) return false;
    g_velCb->Map(0, &none, reinterpret_cast<void**>(&g_velCbPtr));
    if (!g_velCbPtr) return false;

    // velocity pass: root CBV (b0, per frame) + 8 root constants (b1, per draw) + SRV table (t0..t3)
    {
        D3D12_DESCRIPTOR_RANGE srvRange = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 6, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };   // t0..t3 buffers, t4 depth, t5 feed vectors
        D3D12_ROOT_PARAMETER params[3] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; params[0].Descriptor = { 0, 0 }; params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; params[1].Constants = { 1, 0, 8 }; params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; params[2].DescriptorTable = { 1, &srvRange }; params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rs = { 3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
        ID3DBlob* blob = nullptr; ID3DBlob* err = nullptr;
        HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
        if (FAILED(hr)) { LOG("objmv: velocity root signature failed 0x%08lX %s", (unsigned long)hr, err ? (const char*)err->GetBufferPointer() : ""); return false; }
        t_inside = true; hr = g_dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_velRs)); t_inside = false; blob->Release();
        if (FAILED(hr)) return false;
    }
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 18, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };   // parity p at p*6, the feed phase's range at 12
        HRESULT hr = g_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_heap));
        if (FAILED(hr)) { LOG("objmv: descriptor heap failed 0x%08lX", (unsigned long)hr); return false; }
        const UINT inc = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        for (int p = 0; p < 2; ++p) {
            D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_heap->GetCPUDescriptorHandleForHeapStart(); cpu.ptr += SIZE_T(p) * 6 * inc;
            for (int i = 0; i < 2; ++i) {   // [0] cur positions, [1] prev positions
                D3D12_SHADER_RESOURCE_VIEW_DESC srv = {}; srv.Format = DXGI_FORMAT_UNKNOWN; srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER; srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srv.Buffer.FirstElement = 0; srv.Buffer.NumElements = (UINT)(kSoBytes / 16); srv.Buffer.StructureByteStride = 16;
                g_dev->CreateShaderResourceView(g_soBuf[p ^ i], &srv, cpu); cpu.ptr += inc;
            }
            for (int i = 0; i < 2; ++i) {   // [2] cur counters, [3] prev counters (raw)
                D3D12_SHADER_RESOURCE_VIEW_DESC srv = {}; srv.Format = DXGI_FORMAT_R32_TYPELESS; srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER; srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srv.Buffer.FirstElement = 0; srv.Buffer.NumElements = kMaxCaptures * kCtrStride / 4; srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
                g_dev->CreateShaderResourceView(g_ctr[p ^ i], &srv, cpu); cpu.ptr += inc;
            }
        }
    }
    {
        D3D12_QUERY_HEAP_DESC qd = { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kQueries, 0 };
        if (FAILED(g_dev->CreateQueryHeap(&qd, IID_PPV_ARGS(&g_queries)))) { LOG("objmv: timestamp query heap failed - no GPU timing"); g_queries = nullptr; }
        else if (!create_buffer(&g_readback, uint64_t(kRings) * kQueries * 8, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE, L"MGS4DLSS objmv timestamps")) { g_queries->Release(); g_queries = nullptr; }
    }
    return true;
}

// Velocity pipeline matching the game pipeline's rasterizer settings (cull mode, winding, depth bias), so the pass
// covers exactly the surfaces the game rendered, plus a small bias towards the camera so equal depths pass.
static ID3D12PipelineState* vel_pso(const PsoRec* rec, DXGI_FORMAT dsvFmt, bool manualDepth, uint64_t* keyOut)
{
    D3D12_RASTERIZER_DESC r = {};
    r.FillMode = D3D12_FILL_MODE_SOLID; r.CullMode = D3D12_CULL_MODE_BACK; r.DepthClipEnable = TRUE;
    if (rec) { r.CullMode = rec->raster.CullMode; r.FrontCounterClockwise = rec->raster.FrontCounterClockwise; r.DepthBias = rec->raster.DepthBias; r.SlopeScaledDepthBias = rec->raster.SlopeScaledDepthBias; r.DepthBiasClamp = rec->raster.DepthBiasClamp; r.DepthClipEnable = rec->raster.DepthClipEnable; }
    r.DepthBias += 16;   // reversed-Z: towards the camera
    uint64_t key = fnv(&r, sizeof(r)); key = fnv(&dsvFmt, sizeof(dsvFmt), key); key = fnv(&manualDepth, sizeof(manualDepth), key);
    *keyOut = key;
    for (const VelPso& v : g_velPsos) if (v.key == key) return v.pso;
    if (g_velPsos.size() >= 16) return g_velPsos.empty() ? nullptr : g_velPsos[0].pso;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
    d.pRootSignature = g_velRs;
    d.VS = { g_velocity_vs, sizeof(g_velocity_vs) }; d.PS = { g_velocity_ps, sizeof(g_velocity_ps) };
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    d.SampleMask = UINT_MAX;
    d.RasterizerState = r;
    d.DepthStencilState.DepthEnable = manualDepth ? FALSE : TRUE; d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO; d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1; d.RTVFormats[0] = DXGI_FORMAT_R16G16_FLOAT; d.DSVFormat = manualDepth ? DXGI_FORMAT_UNKNOWN : dsvFmt;
    d.SampleDesc = { 1, 0 };
    ID3D12PipelineState* out = nullptr;
    t_inside = true; HRESULT hr = g_dev->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&out)); t_inside = false;
    if (FAILED(hr)) { LOG("objmv: velocity PSO failed 0x%08lX (cull %d, dsv fmt %d)", (unsigned long)hr, (int)r.CullMode, (int)dsvFmt); snprintf(g_st.lastError, sizeof(g_st.lastError), "velocity PSO creation failed 0x%08lX", (unsigned long)hr); return nullptr; }
    g_velPsos.push_back({ key, out }); g_st.velPsos++;
    return out;
}

void init(ID3D12Device* device, LogFn log)
{
    g_log = log; g_dev = device;
    MH_Initialize();
    void** vt = *reinterpret_cast<void***>(device);
    bool ok = true;
    if (MH_CreateHook(vt[16], (void*)hk_CreateRootSignature, (void**)&o_CreateRootSignature) != MH_OK || MH_EnableHook(vt[16]) != MH_OK) { ok = false; LOG("objmv: hook CreateRootSignature failed"); }
    if (MH_CreateHook(vt[10], (void*)hk_CreateGraphicsPipelineState, (void**)&o_CreateGraphicsPipelineState) != MH_OK || MH_EnableHook(vt[10]) != MH_OK) { ok = false; LOG("objmv: hook CreateGraphicsPipelineState failed"); }
    ID3D12Device2* d2 = nullptr;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&d2))) && d2) {
        void** vt2 = *reinterpret_cast<void***>(d2);
        if (MH_CreateHook(vt2[47], (void*)hk_CreatePipelineState, (void**)&o_CreatePipelineState) != MH_OK || MH_EnableHook(vt2[47]) != MH_OK) { ok = false; LOG("objmv: hook CreatePipelineState failed"); }
        d2->Release();
    } else LOG("objmv: ID3D12Device2 not available - stream-form pipelines will not be seen");
    if (ok && create_pass_resources()) { g_st.ready = true; LOG("objmv: ready (v2: one stream-out draw per object, ping-pong buffers %llu MB x2, %u captures/frame, GPU timing %s)", (unsigned long long)(kSoBytes >> 20), kMaxCaptures, g_queries ? "on" : "off"); }
    else LOG("objmv: not available");
}

void shutdown()
{
    g_st.ready = false;
    g_psos.clear();
    for (auto& kv : g_soShared) { if (kv.second.pso) kv.second.pso->Release(); if (kv.second.psoUv) kv.second.psoUv->Release(); }
    if (g_projPso) { g_projPso->Release(); g_projPso = nullptr; }
    g_soShared.clear();
    g_soRootSigs.clear();
    for (VelPso& v : g_velPsos) if (v.pso) v.pso->Release();
    g_velPsos.clear();
    ID3D12DeviceChild* objs[] = { g_soBuf[0], g_soBuf[1], g_ctr[0], g_ctr[1], g_zero, g_velRs, g_heap, g_velCb, g_queries, g_readback };
    for (ID3D12DeviceChild* o : objs) if (o) o->Release();
    g_soBuf[0] = g_soBuf[1] = nullptr; g_ctr[0] = g_ctr[1] = nullptr; g_zero = nullptr; g_velRs = nullptr; g_heap = nullptr; g_velCb = nullptr; g_queries = nullptr; g_readback = nullptr;
    g_velCbPtr = nullptr;
}

bool ready() { return g_st.ready; }
const Stats& stats() { return g_st; }
bool has_captures() { return g_frameStarted; }
static float g_maxPixels = 200.0f, g_maxGradient = 4.0f;
void set_limits(float maxPixels, float maxGradient) { g_maxPixels = maxPixels; g_maxGradient = maxGradient; }
void set_timestamp_frequency(uint64_t hz) { g_tsHz = hz; }

static double cpu_now_ms() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return g_qpf.QuadPart ? t.QuadPart * 1000.0 / double(g_qpf.QuadPart) : 0.0; }

static void read_timing(uint32_t frame)
{
    if (!g_queries || !g_tsHz) return;
    const uint32_t ring = (frame + kRings - 2) % kRings;   // written two frames ago
    RingInfo& ri = g_rings[ring];
    if (!ri.resolved) return;
    ri.resolved = false;
    const uint64_t base = uint64_t(ring) * kQueries * 8;
    const uint32_t n = 4 + 2 * ri.captures;
    D3D12_RANGE rr = { (SIZE_T)base, (SIZE_T)(base + uint64_t(n) * 8) };
    uint8_t* mapped = nullptr;
    if (FAILED(g_readback->Map(0, &rr, reinterpret_cast<void**>(&mapped))) || !mapped) return;
    const uint64_t* ts = reinterpret_cast<const uint64_t*>(mapped + base);
    const double toMs = 1000.0 / double(g_tsHz);
    auto span = [&](uint32_t a, uint32_t b) { return (ts[b] > ts[a]) ? double(ts[b] - ts[a]) * toMs : 0.0; };
    const double fr = span(0, 1), vel = ri.vel ? span(2, 3) : 0.0;
    double so = 0.0; for (uint32_t i = 0; i < ri.captures; ++i) so += span(4 + 2 * i, 5 + 2 * i);
    D3D12_RANGE none = { 0, 0 }; g_readback->Unmap(0, &none);
    if (fr > 0 && fr < 200.0) { g_accFrame += fr; g_accSo += so; g_accVel += vel; g_accN++; }
}

void new_frame(uint32_t frame)
{
    g_st.capturedLast = g_st.captured; g_st.withPrevLast = g_st.withPrev; g_st.skippedLast = g_st.skipped; g_st.overflowLast = g_st.overflow; g_st.reorderedLast = g_reordered;
    g_st.captured = g_st.withPrev = g_st.skipped = g_st.overflow = 0; g_reordered = 0; g_st.feedCaptured = g_st.monitorCaptured = 0;
    g_accCpu += g_cpuThisFrame; g_cpuThisFrame = 0;
    read_timing(frame);
    const ULONGLONG t = GetTickCount64();
    if (t - g_accT0 >= 1000) {
        if (g_accN) { g_st.frameGpuMs = float(g_accFrame / g_accN); g_st.soGpuMs = float(g_accSo / g_accN); g_st.velGpuMs = float(g_accVel / g_accN); g_st.cpuMs = float(g_accCpu / g_accN); }
        g_accFrame = g_accSo = g_accVel = g_accCpu = 0; g_accN = 0; g_accT0 = t;
    }
    g_curFrame = frame; g_soBytesThisFrame = 0; g_captures = 0; g_frameStarted = false; g_frameBegun = false; g_velRan = false;
    g_vel.clear();
    g_st.slotsUsed = (uint32_t)g_slots.size();
    if (g_slots.size() > 4096) {   // drop entries not seen recently
        for (auto it = g_slots.begin(); it != g_slots.end();) { const uint32_t a = it->second.lastFrame[0], b = it->second.lastFrame[1]; const uint32_t last = (a == UINT32_MAX) ? b : (b == UINT32_MAX ? a : (a > b ? a : b)); if (last == UINT32_MAX || frame - last > 120) it = g_slots.erase(it); else ++it; }
        if (g_slots.size() > 4096) g_slots.clear();
    }
}

void mark_frame_begin(ID3D12GraphicsCommandList* cl)
{
    if (!g_queries || g_frameBegun) return;
    g_frameBegun = true;
    cl->EndQuery(g_queries, D3D12_QUERY_TYPE_TIMESTAMP, 0);
}

void mark_frame_end(ID3D12GraphicsCommandList* cl)
{
    if (!g_queries || !g_frameBegun) return;
    cl->EndQuery(g_queries, D3D12_QUERY_TYPE_TIMESTAMP, 1);
    const uint32_t ring = g_curFrame % kRings;
    RingInfo& ri = g_rings[ring];
    ri.captures = g_captures; ri.vel = g_velRan;
    cl->ResolveQueryData(g_queries, D3D12_QUERY_TYPE_TIMESTAMP, 0, 4 + 2 * g_captures, g_readback, uint64_t(ring) * kQueries * 8);
    ri.resolved = true;
    g_frameBegun = false;
}

static void begin_captures(ID3D12GraphicsCommandList* cl)
{
    const uint32_t p = g_curFrame & 1;
    transition(cl, g_ctr[p], &g_ctrState[p], D3D12_RESOURCE_STATE_COPY_DEST);
    cl->CopyBufferRegion(g_ctr[p], 0, g_zero, 0, kMaxCaptures * kCtrStride);
    transition(cl, g_ctr[p], &g_ctrState[p], D3D12_RESOURCE_STATE_STREAM_OUT);
    transition(cl, g_soBuf[p], &g_soState[p], D3D12_RESOURCE_STATE_STREAM_OUT);
}

bool capture(ID3D12GraphicsCommandList* cl, uint64_t key, ID3D12PipelineState* gamePso, uint32_t topology, const DrawArgs& da, bool jittered, const D3D12_VIEWPORT* ownVp, const float* anchor, uint32_t anchorN, int view)
{
    if (!g_st.ready || !gamePso) return false;
    const double t0 = cpu_now_ms();
    uint32_t verts = 0;
    if (topology == D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST) verts = da.count - da.count % 3;
    else if (topology == D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP) verts = da.count >= 3 ? (da.count - 2) * 3 : 0;
    else { g_st.skipped++; return false; }
    const uint64_t bound64 = uint64_t(verts) * (da.instances ? da.instances : 1);
    if (bound64 == 0 || bound64 > 0xFFFFFFFFull) { g_st.skipped++; return false; }
    const uint32_t bound = (uint32_t)bound64;
    const bool monitor = view == 3;
    ID3D12PipelineState* so; const PsoRec* rec = nullptr;
    { std::lock_guard<std::mutex> lock(g_mutex); so = monitor ? so_pso_uv(gamePso, &rec) : so_pso(gamePso, &rec); }
    if (!so || !rec) return false;
    const uint64_t bytes = uint64_t(bound) * (monitor ? 32 : 16);
    if (g_soBytesThisFrame + bytes > kSoBytes) { g_st.overflow++; return false; }
    if (g_captures >= kMaxCaptures) { g_st.skipped++; return false; }
    if (!g_frameStarted) { begin_captures(cl); g_frameStarted = true; }
    const uint32_t p = g_curFrame & 1;
    const uint32_t idx = g_captures++;
    const uint32_t off = (uint32_t)(g_soBytesThisFrame / 16);
    g_soBytesThisFrame += bytes;

    if (monitor) {   // no pairing: the projector pass needs this frame's positions and texture coordinates only
        VelEntry e = { 0, 0, off, off, idx, idx, bound, 0, ownVp != nullptr, ownVp ? *ownVp : D3D12_VIEWPORT{}, 3 };
        g_vel.push_back(e); g_st.monitorCaptured++;
        if (g_queries) cl->EndQuery(g_queries, D3D12_QUERY_TYPE_TIMESTAMP, 4 + 2 * idx);
        cl->SetPipelineState(so);
        D3D12_STREAM_OUTPUT_BUFFER_VIEW v = { g_soBuf[p]->GetGPUVirtualAddress() + uint64_t(off) * 16, bytes, g_ctr[p]->GetGPUVirtualAddress() + uint64_t(idx) * kCtrStride };
        cl->SOSetTargets(0, 1, &v);
        if (da.indexed) cl->DrawIndexedInstanced(da.count, da.instances, da.first, da.vertexOffset, da.firstInstance);
        else cl->DrawInstanced(da.count, da.instances, da.first, da.firstInstance);
        D3D12_STREAM_OUTPUT_BUFFER_VIEW none = { 0, 0, 0 };
        cl->SOSetTargets(0, 1, &none);
        cl->SetPipelineState(gamePso);
        if (g_queries) cl->EndQuery(g_queries, D3D12_QUERY_TYPE_TIMESTAMP, 5 + 2 * idx);
        g_st.captured++;
        g_cpuThisFrame += cpu_now_ms() - t0;
        return true;
    }
    if (view == 2) g_st.feedCaptured++;
    Slot& s = g_slots[key];
    if (s.lastFrame[p] != g_curFrame) { s.occ[p].clear(); s.lastFrame[p] = g_curFrame; }
    // Diagnostics: a geometry drawn more than once a frame, and a change in how often, is where a wrong pairing comes from
    if (!s.occ[p].empty()) {
        static uint32_t nlog = 0;
        const size_t prevN = s.lastFrame[p ^ 1] == g_curFrame - 1 ? s.occ[p ^ 1].size() : 0;
        if (s.occ[p].size() + 1 != prevN && nlog < 60) { nlog++; LOG("objmv: f%u geometry %016llx drawn %zu+ times this frame (%zu last frame): occurrence %zu vs %016llx ps %016llx, %u vertices%s", g_curFrame, (unsigned long long)key, s.occ[p].size() + 1, prevN, s.occ[p].size(), (unsigned long long)rec->vsHash, (unsigned long long)rec->psHash, bound, rec->skinned ? ", skinned" : ""); }
    }
    Occ cur = {}; cur.off = off; cur.bound = bound; cur.ctr = idx; cur.jit = jittered; cur.ownVp = ownVp != nullptr; if (ownVp) cur.vp = *ownVp;
    for (uint32_t i = 0; i < kAnchorN && i < anchorN; ++i) { const float v = anchor ? anchor[i] : 0.0f; cur.anchor[i] = (v == v && fabsf(v) < 1e12f) ? v : 0.0f; }   // NaN / packed values -> 0
    uint64_t velKey = 0, velKeyM = 0; vel_pso(rec, DXGI_FORMAT_D24_UNORM_S8_UINT, false, &velKey); vel_pso(rec, DXGI_FORMAT_D24_UNORM_S8_UINT, true, &velKeyM);   // creates the variants lazily
    if (s.lastFrame[p ^ 1] == g_curFrame - 1) {
        // the previous frame's occurrence of this geometry with the nearest signature (same vertex count), not yet paired
        std::vector<Occ>& prev = s.occ[p ^ 1];
        Occ* best = nullptr; float bestD = 0.0f; size_t bestI = 0;
        for (size_t i = 0; i < prev.size(); ++i) {
            Occ& o = prev[i];
            if (o.claimed || o.bound != bound) continue;
            float d = 0.0f; for (uint32_t k = 0; k < kAnchorN; ++k) { const float t = o.anchor[k] - cur.anchor[k]; d += t * t; }
            if (!best || d < bestD) { best = &o; bestD = d; bestI = i; }
        }
        if (best) {
            best->claimed = true;
            if (bestI != s.occ[p].size()) g_reordered++;
            VelEntry e = { velKey, velKeyM, off, best->off, idx, best->ctr, bound, (jittered ? 1u : 0u) | (best->jit ? 2u : 0u), ownVp != nullptr, ownVp ? *ownVp : D3D12_VIEWPORT{}, view };
            g_vel.push_back(e); g_st.withPrev++;
        }
    }
    s.occ[p].push_back(cur);

    if (g_queries) cl->EndQuery(g_queries, D3D12_QUERY_TYPE_TIMESTAMP, 4 + 2 * idx);
    cl->SetPipelineState(so);
    D3D12_STREAM_OUTPUT_BUFFER_VIEW v = { g_soBuf[p]->GetGPUVirtualAddress() + uint64_t(off) * 16, bytes, g_ctr[p]->GetGPUVirtualAddress() + uint64_t(idx) * kCtrStride };
    cl->SOSetTargets(0, 1, &v);
    if (da.indexed) cl->DrawIndexedInstanced(da.count, da.instances, da.first, da.vertexOffset, da.firstInstance);
    else cl->DrawInstanced(da.count, da.instances, da.first, da.firstInstance);
    D3D12_STREAM_OUTPUT_BUFFER_VIEW none = { 0, 0, 0 };
    cl->SOSetTargets(0, 1, &none);
    cl->SetPipelineState(gamePso);
    if (g_queries) cl->EndQuery(g_queries, D3D12_QUERY_TYPE_TIMESTAMP, 5 + 2 * idx);
    g_st.captured++;
    g_cpuThisFrame += cpu_now_ms() - t0;
    return true;
}

static ID3D12PipelineState* proj_pso()
{
    if (g_projPso) return g_projPso;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
    d.pRootSignature = g_velRs;
    d.VS = { g_monitor_vs, sizeof(g_monitor_vs) }; d.PS = { g_monitor_ps, sizeof(g_monitor_ps) };
    D3D12_RENDER_TARGET_BLEND_DESC& b = d.BlendState.RenderTarget[0];
    b.BlendEnable = TRUE; b.SrcBlend = D3D12_BLEND_ONE; b.DestBlend = D3D12_BLEND_ONE; b.BlendOp = D3D12_BLEND_OP_ADD; b.SrcBlendAlpha = D3D12_BLEND_ONE; b.DestBlendAlpha = D3D12_BLEND_ZERO; b.BlendOpAlpha = D3D12_BLEND_OP_ADD; b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; d.RasterizerState.DepthClipEnable = TRUE;
    d.DepthStencilState.DepthEnable = FALSE;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1; d.RTVFormats[0] = DXGI_FORMAT_R16G16_FLOAT; d.DSVFormat = DXGI_FORMAT_UNKNOWN;
    d.SampleDesc = { 1, 0 };
    t_inside = true; HRESULT hr = g_dev->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&g_projPso)); t_inside = false;
    if (FAILED(hr)) { LOG("objmv: monitor projector PSO failed 0x%08lX", (unsigned long)hr); g_projPso = nullptr; }
    return g_projPso;
}
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

void velocity(ID3D12GraphicsCommandList* cl, D3D12_CPU_DESCRIPTOR_HANDLE mvRtv, D3D12_CPU_DESCRIPTOR_HANDLE sceneDsv, uint32_t w, uint32_t h, const D3D12_VIEWPORT& sceneVp,
              const float jitterCur[2], const float jitterPrev[2], const float prevSize[2], ID3D12Resource* manualDepth,
              ID3D12Resource* feedDepth, D3D12_CPU_DESCRIPTOR_HANDLE feedMvRtv, ID3D12Resource* feedMv, const float* feedRect, bool flipFeedV, bool skipWindow)
{
    if (!g_st.ready || !g_frameStarted) return;
    const double t0 = cpu_now_ms();
    g_frameStarted = false; g_velRan = true;
    const uint32_t p = g_curFrame & 1;
    if (g_queries) cl->EndQuery(g_queries, D3D12_QUERY_TYPE_TIMESTAMP, 2);
    for (int i = 0; i < 2; ++i) { transition(cl, g_soBuf[i], &g_soState[i], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE); transition(cl, g_ctr[i], &g_ctrState[i], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE); }
    if (!g_vel.empty()) {
        const uint32_t cbSlot = g_velCbSlot++ % 4;
        const float cw = sceneVp.Width > 0 ? sceneVp.Width : float(w), ch = sceneVp.Height > 0 ? sceneVp.Height : float(h);
        float cb[12] = { cw, ch, jitterCur[0], jitterCur[1], jitterPrev[0], jitterPrev[1], prevSize && prevSize[0] > 0 ? prevSize[0] : cw, prevSize && prevSize[1] > 0 ? prevSize[1] : ch, g_maxPixels, g_maxGradient, 0.0f, 0.0f };
        memcpy(g_velCbPtr + cbSlot * 256, cb, sizeof(cb));
        const UINT inc = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        ID3D12DescriptorHeap* heaps[1] = { g_heap };
        cl->SetDescriptorHeaps(1, heaps);
        cl->SetGraphicsRootSignature(g_velRs);
        cl->SetGraphicsRootConstantBufferView(0, g_velCb->GetGPUVirtualAddress() + cbSlot * 256);
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_heap->GetGPUDescriptorHandleForHeapStart(); gpu.ptr += UINT64(p) * 6 * inc;
        D3D12_CPU_DESCRIPTOR_HANDLE pcpu = g_heap->GetCPUDescriptorHandleForHeapStart(); pcpu.ptr += SIZE_T(p) * 6 * inc;
        if (manualDepth) {   // [4] full-grid depth for the manual test
            D3D12_CPU_DESCRIPTOR_HANDLE dcpu = pcpu; dcpu.ptr += 4 * inc;
            D3D12_SHADER_RESOURCE_VIEW_DESC dsrv = {}; dsrv.Format = DXGI_FORMAT_R32_FLOAT; dsrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; dsrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; dsrv.Texture2D.MipLevels = 1;
            g_dev->CreateShaderResourceView(manualDepth, &dsrv, dcpu);
        }
        if (feedMv) {   // [5] the feed's vectors for the projector pass
            D3D12_CPU_DESCRIPTOR_HANDLE fcpu = pcpu; fcpu.ptr += 5 * inc;
            D3D12_SHADER_RESOURCE_VIEW_DESC fsrv = {}; fsrv.Format = DXGI_FORMAT_R16G16_FLOAT; fsrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; fsrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; fsrv.Texture2D.MipLevels = 1;
            g_dev->CreateShaderResourceView(feedMv, &fsrv, fcpu);
        }
        cl->SetGraphicsRootDescriptorTable(2, gpu);
        if (manualDepth) cl->OMSetRenderTargets(1, &mvRtv, FALSE, nullptr); else cl->OMSetRenderTargets(1, &mvRtv, FALSE, &sceneDsv);
        D3D12_VIEWPORT vp = sceneVp.Width > 0 ? sceneVp : D3D12_VIEWPORT{ 0, 0, float(w), float(h), 0, 1 }; cl->RSSetViewports(1, &vp);
        D3D12_RECT sc = { 0, 0, (LONG)w, (LONG)h }; cl->RSSetScissorRects(1, &sc);
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const bool manual = manualDepth != nullptr;
        std::sort(g_vel.begin(), g_vel.end(), [manual](const VelEntry& a, const VelEntry& b) { return (manual ? a.psoKeyManual : a.psoKey) < (manual ? b.psoKeyManual : b.psoKey); });
        uint64_t boundKey = ~0ull; ID3D12PipelineState* cur = nullptr; bool curOwn = false; D3D12_VIEWPORT curVp = vp;
        for (const VelEntry& e : g_vel) {
            if (e.view >= 2) continue;   // the caller feed and the monitor: below
            if (e.view == 1 && skipWindow) continue;   // the window's camera cut this frame
            // objects drawn into their own 3D window use that viewport (their clip positions map to it)
            if (e.ownVp != curOwn || (e.ownVp && memcmp(&e.vp, &curVp, sizeof(curVp)) != 0)) { curOwn = e.ownVp; curVp = e.ownVp ? e.vp : vp; cl->RSSetViewports(1, &curVp); }
            const uint64_t want = manual ? e.psoKeyManual : e.psoKey;
            if (want != boundKey) { boundKey = want; cur = nullptr; for (const VelPso& v : g_velPsos) if (v.key == want) { cur = v.pso; break; } if (cur) cl->SetPipelineState(cur); }
            if (!cur) continue;
            uint32_t vw = 0, vh = 0; if (e.ownVp) { memcpy(&vw, &e.vp.Width, 4); memcpy(&vh, &e.vp.Height, 4); }   // the window's size for the pixel mapping (flag 8)
            const uint32_t consts[8] = { e.curOff, e.prevOff, e.curCtr, e.prevCtr, e.flags | (manual ? 4u : 0u) | (e.ownVp ? 8u : 0u), vw, vh, 0 };
            cl->SetGraphicsRoot32BitConstants(1, 8, consts, 0);
            cl->DrawInstanced(e.bound, 1, 0, 0);
        }
        // The video call's caller feed: its objects' vectors into the feed-vector texture, in the feed's rectangle,
        // tested against the feed's own depth copy (t4 of a second descriptor range).
        bool anyFeed = false, anyMon = false; for (const VelEntry& e : g_vel) { if (e.view == 2) anyFeed = true; if (e.view == 3) anyMon = true; }
        if (feedMvRtv.ptr && feedMv && feedRect && anyFeed) {
            D3D12_CPU_DESCRIPTOR_HANDLE fr = g_heap->GetCPUDescriptorHandleForHeapStart(); fr.ptr += SIZE_T(12) * inc;
            g_dev->CopyDescriptorsSimple(4, fr, pcpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            if (feedDepth) { D3D12_CPU_DESCRIPTOR_HANDLE dcpu = fr; dcpu.ptr += 4 * inc; D3D12_SHADER_RESOURCE_VIEW_DESC dsrv = {}; dsrv.Format = DXGI_FORMAT_R32_FLOAT; dsrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; dsrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; dsrv.Texture2D.MipLevels = 1; g_dev->CreateShaderResourceView(feedDepth, &dsrv, dcpu); }
            else { D3D12_CPU_DESCRIPTOR_HANDLE a = fr, b2 = pcpu; a.ptr += 4 * inc; b2.ptr += 4 * inc; g_dev->CopyDescriptorsSimple(1, a, b2, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV); }
            { D3D12_CPU_DESCRIPTOR_HANDLE a = fr, b2 = pcpu; a.ptr += 5 * inc; b2.ptr += 5 * inc; g_dev->CopyDescriptorsSimple(1, a, b2, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV); }
            D3D12_GPU_DESCRIPTOR_HANDLE fgpu = g_heap->GetGPUDescriptorHandleForHeapStart(); fgpu.ptr += UINT64(12) * inc;
            cl->SetGraphicsRootDescriptorTable(2, fgpu);
            cl->OMSetRenderTargets(1, &feedMvRtv, FALSE, nullptr);
            boundKey = ~0ull; cur = nullptr;
            for (const VelEntry& e : g_vel) {
                if (e.view != 2) continue;
                if (memcmp(&e.vp, &curVp, sizeof(curVp)) != 0) { curVp = e.vp; cl->RSSetViewports(1, &curVp); }
                if (e.psoKeyManual != boundKey) { boundKey = e.psoKeyManual; cur = nullptr; for (const VelPso& v : g_velPsos) if (v.key == boundKey) { cur = v.pso; break; } if (cur) cl->SetPipelineState(cur); }
                if (!cur) continue;
                uint32_t vw = 0, vh = 0; memcpy(&vw, &e.vp.Width, 4); memcpy(&vh, &e.vp.Height, 4);
                const uint32_t consts[8] = { e.curOff, e.prevOff, e.curCtr, e.prevCtr, e.flags | (feedDepth ? 4u : 0u) | 8u, vw, vh, 0 };
                cl->SetGraphicsRoot32BitConstants(1, 8, consts, 0);
                cl->DrawInstanced(e.bound, 1, 0, 0);
            }
            // the monitor reads the feed's vectors now
            D3D12_RESOURCE_BARRIER bar = {}; bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; bar.Transition = { feedMv, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
            cl->ResourceBarrier(1, &bar);
            cl->SetGraphicsRootDescriptorTable(2, gpu);
            cl->OMSetRenderTargets(1, &mvRtv, FALSE, nullptr);
            cl->RSSetViewports(1, &vp); curVp = vp; curOwn = false;
            // The monitor: the feed's motion projected through the monitor's texture mapping, added to the surface's own vector
            if (anyMon && proj_pso()) {
                cl->SetPipelineState(g_projPso);
                for (const VelEntry& e : g_vel) {
                    if (e.view != 3) continue;
                    const uint32_t consts[8] = { e.curOff, f2u(feedRect[2]), e.curCtr, f2u(feedRect[3]), (manual ? 4u : 0u) | (flipFeedV ? 32u : 0u), f2u(feedRect[0]), f2u(feedRect[1]), 0 };
                    cl->SetGraphicsRoot32BitConstants(1, 8, consts, 0);
                    cl->DrawInstanced(e.bound, 1, 0, 0);
                    g_st.monitorDrawn++;
                }
            }
            bar.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE; bar.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            cl->ResourceBarrier(1, &bar);
        }
    }
    if (g_queries) cl->EndQuery(g_queries, D3D12_QUERY_TYPE_TIMESTAMP, 3);
    g_st.velocityFrames++;
    g_cpuThisFrame += cpu_now_ms() - t0;
}

} // namespace objmv
