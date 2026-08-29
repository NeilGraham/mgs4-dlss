// Per-object motion vectors via stream output of the game's own vertex shaders. See objmv.h.
#define WIN32_LEAN_AND_MEAN
#include "objmv.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <unordered_map>
#include <mutex>
#include "MinHook.h"
#include "velocity_vs.h"   // g_velocity_vs
#include "velocity_ps.h"   // g_velocity_ps
#include "soargs_cs.h"     // g_soargs_cs

namespace objmv {

static LogFn g_log = nullptr;
#define LOG(...) do { if (g_log) g_log(__VA_ARGS__); } while (0)

static Stats g_st;
static ID3D12Device* g_dev = nullptr;
static std::mutex g_mutex;

// ---- recorded root signatures / pipelines --------------------------------------------------------------------------
struct RootSigRec { std::vector<uint8_t> blob; ID3D12RootSignature* soVariant = nullptr; bool failed = false; };
static std::unordered_map<ID3D12RootSignature*, RootSigRec> g_rootSigs;

struct PsoRec {
    std::vector<uint8_t> vs;
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
};
static std::unordered_map<ID3D12PipelineState*, PsoRec> g_psos;
struct SoShared { ID3D12PipelineState* pso = nullptr; bool failed = false; uint32_t users = 0; };
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
                         D3D12_PRIMITIVE_TOPOLOGY_TYPE topo, D3D12_INDEX_BUFFER_STRIP_CUT_VALUE stripCut, UINT nodeMask, ID3D12RootSignature* rootSig)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    PsoRec& p = g_psos[pso];
    p = PsoRec();
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
    D3D12_SHADER_BYTECODE vs = {}; D3D12_INPUT_LAYOUT_DESC il = {}; D3D12_RASTERIZER_DESC raster = {}; bool haveRaster = false;
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
    remember_pso(reinterpret_cast<ID3D12PipelineState*>(*out), vs, il, raster, topo, cut, nodeMask, rs);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_CreateRootSignature(ID3D12Device* self, UINT nodeMask, const void* blob, SIZE_T len, REFIID riid, void** out)
{
    HRESULT hr = o_CreateRootSignature(self, nodeMask, blob, len, riid, out);
    if (SUCCEEDED(hr) && !t_inside && out && *out && blob && len && riid == __uuidof(ID3D12RootSignature)) {
        std::lock_guard<std::mutex> lock(g_mutex);
        RootSigRec& r = g_rootSigs[reinterpret_cast<ID3D12RootSignature*>(*out)];
        r.blob.assign(static_cast<const uint8_t*>(blob), static_cast<const uint8_t*>(blob) + len);
        r.soVariant = nullptr; r.failed = false;
        g_st.rootSigsSeen++;
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_CreateGraphicsPipelineState(ID3D12Device* self, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID riid, void** out)
{
    HRESULT hr = o_CreateGraphicsPipelineState(self, desc, riid, out);
    if (SUCCEEDED(hr) && !t_inside && out && *out && desc && desc->VS.pShaderBytecode && desc->VS.BytecodeLength && riid == __uuidof(ID3D12PipelineState)
        && desc->PrimitiveTopologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE && !desc->GS.pShaderBytecode && !desc->HS.pShaderBytecode)
        remember_pso(reinterpret_cast<ID3D12PipelineState*>(*out), desc->VS, desc->InputLayout, desc->RasterizerState, desc->PrimitiveTopologyType, desc->IBStripCutValue, desc->NodeMask, desc->pRootSignature);
    return hr;
}

void forget_pso(ID3D12PipelineState* pso)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_psos.find(pso);
    if (it == g_psos.end()) return;
    g_psos.erase(it);   // shared stream-out variants stay cached (the same VS comes back with the next pipeline object)
}

// root signature clone with the stream-output flag
static ID3D12RootSignature* so_root_signature(ID3D12RootSignature* rs)
{
    auto it = g_rootSigs.find(rs);
    if (it == g_rootSigs.end()) { static int n = 0; if (n++ < 3) LOG("objmv: root signature %p was created before the hook - no stream-out variant", (void*)rs); return nullptr; }
    RootSigRec& r = it->second;
    if (r.soVariant || r.failed) return r.soVariant;
    r.failed = true;
    ID3D12VersionedRootSignatureDeserializer* de = nullptr;
    HRESULT hr = D3D12CreateVersionedRootSignatureDeserializer(r.blob.data(), r.blob.size(), IID_PPV_ARGS(&de));
    if (FAILED(hr) || !de) { LOG("objmv: root signature deserialize failed 0x%08lX", (unsigned long)hr); return nullptr; }
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* src = de->GetUnconvertedRootSignatureDesc();
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC copy = *src;
    switch (copy.Version) {
    case D3D_ROOT_SIGNATURE_VERSION_1_0: copy.Desc_1_0.Flags |= D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT; break;
    case D3D_ROOT_SIGNATURE_VERSION_1_1: copy.Desc_1_1.Flags |= D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT; break;
    default: copy.Desc_1_2.Flags |= D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT; break;
    }
    ID3DBlob* blob = nullptr; ID3DBlob* err = nullptr;
    hr = D3D12SerializeVersionedRootSignature(&copy, &blob, &err);
    de->Release();
    if (FAILED(hr) || !blob) { LOG("objmv: root signature re-serialize failed 0x%08lX %s", (unsigned long)hr, err ? (const char*)err->GetBufferPointer() : ""); if (err) err->Release(); return nullptr; }
    ID3D12RootSignature* out = nullptr;
    t_inside = true;
    hr = g_dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&out));
    t_inside = false;
    blob->Release();
    if (FAILED(hr) || !out) { LOG("objmv: stream-out root signature creation failed 0x%08lX", (unsigned long)hr); return nullptr; }
    r.soVariant = out; r.failed = false;
    LOG("objmv: stream-out root signature created for %p (version %d)", (void*)rs, (int)copy.Version);
    return out;
}

static ID3D12PipelineState* so_pso(ID3D12PipelineState* pso)
{
    auto it = g_psos.find(pso);
    if (it == g_psos.end()) { g_st.skipped++; return nullptr; }
    PsoRec& p = it->second;
    if (p.soPso || p.failed) return p.soPso;
    SoShared& sh = g_soShared[p.shareKey];
    if (sh.pso) { p.soPso = sh.pso; sh.users++; return sh.pso; }
    if (sh.failed) { p.failed = true; return nullptr; }
    p.failed = true; sh.failed = true;
    ID3D12RootSignature* rs = so_root_signature(p.rootSig);
    if (!rs) { g_st.soPsoFailures++; return nullptr; }
    static const D3D12_SO_DECLARATION_ENTRY decl[1] = { { 0, "SV_Position", 0, 0, 4, 0 } };
    static const UINT strides[1] = { 16 };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
    d.pRootSignature = rs;
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

// ---- pass resources ---------------------------------------------------------------------------------------------------
static const uint32_t kSlots = 2048, kSlotBytes = 8192;        // recorded constants: 2 x kSlots x kSlotBytes
static const uint64_t kSoBytes = 48ull << 20;                  // per stream-out buffer (3M vertices)
static ID3D12Resource* g_cbUpload = nullptr; static uint8_t* g_cbPtr = nullptr;
static ID3D12Resource* g_soBuf[2] = {}; static ID3D12Resource* g_counters = nullptr; static ID3D12Resource* g_zero = nullptr;
static ID3D12Resource* g_args = nullptr;
static ID3D12RootSignature* g_velRs = nullptr; static ID3D12PipelineState* g_velPso = nullptr;
static ID3D12RootSignature* g_argsRs = nullptr; static ID3D12PipelineState* g_argsPso = nullptr;
static ID3D12CommandSignature* g_cmdSig = nullptr;
static ID3D12DescriptorHeap* g_heap = nullptr;                 // shader visible: [0] SRV cur, [1] SRV prev
static ID3D12Resource* g_velCb = nullptr; static uint8_t* g_velCbPtr = nullptr;   // 4 x 256 B ring
static uint32_t g_velCbSlot = 0;
static D3D12_RESOURCE_STATES g_soState = D3D12_RESOURCE_STATE_STREAM_OUT, g_ctrState = D3D12_RESOURCE_STATE_STREAM_OUT;
static bool g_frameStarted = false;   // counters reset + captures pending this frame
static uint32_t g_curFrame = 0;
static uint64_t g_soBytesThisFrame = 0;

struct Slot { uint32_t idx; uint32_t lastFrame; uint32_t size[2]; };
static std::unordered_map<uint64_t, Slot> g_slots;
static uint32_t g_nextSlot = 0;

static bool create_buffer(ID3D12Resource** out, uint64_t size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags, const wchar_t* name)
{
    D3D12_HEAP_PROPERTIES hp = { heap, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1 };
    D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = size; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc = { 1, 0 }; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; bd.Flags = flags;
    HRESULT hr = g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, state, nullptr, IID_PPV_ARGS(out));
    if (FAILED(hr)) { LOG("objmv: buffer %ls (%llu bytes) failed 0x%08lX", name, (unsigned long long)size, (unsigned long)hr); return false; }
    (*out)->SetName(name);
    return true;
}

static bool create_pass_resources()
{
    if (!create_buffer(&g_cbUpload, 2ull * kSlots * kSlotBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, L"MGS4DLSS objmv constants")) return false;
    D3D12_RANGE none = { 0, 0 }; g_cbUpload->Map(0, &none, reinterpret_cast<void**>(&g_cbPtr));
    if (!g_cbPtr) return false;
    for (int i = 0; i < 2; ++i) if (!create_buffer(&g_soBuf[i], kSoBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_FLAG_NONE, i ? L"MGS4DLSS objmv prev positions" : L"MGS4DLSS objmv cur positions")) return false;
    if (!create_buffer(&g_counters, 256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_FLAG_NONE, L"MGS4DLSS objmv SO counters")) return false;
    if (!create_buffer(&g_zero, 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, L"MGS4DLSS objmv zero")) return false;
    { void* p = nullptr; g_zero->Map(0, &none, &p); if (p) memset(p, 0, 256); g_zero->Unmap(0, nullptr); }
    if (!create_buffer(&g_args, 64, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"MGS4DLSS objmv draw args")) return false;
    if (!create_buffer(&g_velCb, 4 * 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, L"MGS4DLSS objmv velocity cb")) return false;
    g_velCb->Map(0, &none, reinterpret_cast<void**>(&g_velCbPtr));

    // velocity pass: root CBV + SRV table (t0, t1)
    {
        D3D12_DESCRIPTOR_RANGE srvRange = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };
        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; params[0].Descriptor = { 0, 0 }; params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; params[1].DescriptorTable = { 1, &srvRange }; params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        D3D12_ROOT_SIGNATURE_DESC rs = { 2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
        ID3DBlob* blob = nullptr; ID3DBlob* err = nullptr;
        HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
        if (FAILED(hr)) { LOG("objmv: velocity root signature failed 0x%08lX %s", (unsigned long)hr, err ? (const char*)err->GetBufferPointer() : ""); return false; }
        t_inside = true; hr = g_dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_velRs)); t_inside = false; blob->Release();
        if (FAILED(hr)) return false;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
        d.pRootSignature = g_velRs;
        d.VS = { g_velocity_vs, sizeof(g_velocity_vs) }; d.PS = { g_velocity_ps, sizeof(g_velocity_ps) };
        d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        d.SampleMask = UINT_MAX;
        d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; d.RasterizerState.DepthClipEnable = TRUE;
        d.RasterizerState.DepthBias = 2048; d.RasterizerState.SlopeScaledDepthBias = 1.0f; d.RasterizerState.DepthBiasClamp = 0.0f;   // reversed-Z: bias towards the camera so equal surfaces pass
        d.DepthStencilState.DepthEnable = TRUE; d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO; d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        d.NumRenderTargets = 1; d.RTVFormats[0] = DXGI_FORMAT_R16G16_FLOAT; d.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
        d.SampleDesc = { 1, 0 };
        t_inside = true; hr = g_dev->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&g_velPso)); t_inside = false;
        if (FAILED(hr)) { LOG("objmv: velocity PSO failed 0x%08lX", (unsigned long)hr); return false; }
    }
    // args compute: root SRV (counters) + root UAV (args)
    {
        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; params[0].Descriptor = { 0, 0 }; params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; params[1].Descriptor = { 0, 0 }; params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rs = { 2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
        ID3DBlob* blob = nullptr; ID3DBlob* err = nullptr;
        HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
        if (FAILED(hr)) { LOG("objmv: args root signature failed 0x%08lX", (unsigned long)hr); return false; }
        t_inside = true; hr = g_dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_argsRs)); t_inside = false; blob->Release();
        if (FAILED(hr)) return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC cd = {}; cd.pRootSignature = g_argsRs; cd.CS = { g_soargs_cs, sizeof(g_soargs_cs) };
        hr = g_dev->CreateComputePipelineState(&cd, IID_PPV_ARGS(&g_argsPso));
        if (FAILED(hr)) { LOG("objmv: args PSO failed 0x%08lX", (unsigned long)hr); return false; }
    }
    {
        D3D12_INDIRECT_ARGUMENT_DESC arg = {}; arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
        D3D12_COMMAND_SIGNATURE_DESC cs = { sizeof(D3D12_DRAW_ARGUMENTS), 1, &arg, 0 };
        HRESULT hr = g_dev->CreateCommandSignature(&cs, nullptr, IID_PPV_ARGS(&g_cmdSig));
        if (FAILED(hr)) { LOG("objmv: command signature failed 0x%08lX", (unsigned long)hr); return false; }
    }
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
        HRESULT hr = g_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_heap));
        if (FAILED(hr)) { LOG("objmv: descriptor heap failed 0x%08lX", (unsigned long)hr); return false; }
        const UINT inc = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_heap->GetCPUDescriptorHandleForHeapStart();
        for (int i = 0; i < 2; ++i) {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv = {}; srv.Format = DXGI_FORMAT_UNKNOWN; srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER; srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Buffer.FirstElement = 0; srv.Buffer.NumElements = (UINT)(kSoBytes / 16); srv.Buffer.StructureByteStride = 16;
            g_dev->CreateShaderResourceView(g_soBuf[i], &srv, cpu); cpu.ptr += inc;
        }
    }
    return true;
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
    if (ok && create_pass_resources()) { g_st.ready = true; LOG("objmv: ready (device hooks installed, stream-out buffers %llu MB x2, %u constant slots)", (unsigned long long)(kSoBytes >> 20), kSlots); }
    else LOG("objmv: not available");
}

void shutdown()
{
    g_st.ready = false;
    g_psos.clear();
    for (auto& kv : g_soShared) if (kv.second.pso) kv.second.pso->Release();
    g_soShared.clear();
    for (auto& kv : g_rootSigs) if (kv.second.soVariant) kv.second.soVariant->Release();
    g_rootSigs.clear();
    ID3D12DeviceChild* objs[] = { g_cbUpload, g_soBuf[0], g_soBuf[1], g_counters, g_zero, g_args, g_velRs, g_velPso, g_argsRs, g_argsPso, g_cmdSig, g_heap, g_velCb };
    for (ID3D12DeviceChild* o : objs) if (o) o->Release();
    g_cbUpload = nullptr; g_soBuf[0] = g_soBuf[1] = nullptr; g_counters = nullptr; g_zero = nullptr; g_args = nullptr; g_velRs = nullptr; g_velPso = nullptr; g_argsRs = nullptr; g_argsPso = nullptr; g_cmdSig = nullptr; g_heap = nullptr; g_velCb = nullptr;
    g_cbPtr = nullptr; g_velCbPtr = nullptr;
}

bool ready() { return g_st.ready; }
const Stats& stats() { return g_st; }
bool has_captures() { return g_frameStarted; }

void new_frame(uint32_t frame)
{
    g_st.recordedLast = g_st.recorded; g_st.capturedLast = g_st.captured; g_st.noPrevLast = g_st.noPrev; g_st.skippedLast = g_st.skipped;
    g_st.recorded = g_st.captured = g_st.noPrev = g_st.skipped = 0;
    g_curFrame = frame; g_soBytesThisFrame = 0;
    g_st.slotsUsed = (uint32_t)g_slots.size();
    if (g_slots.size() > kSlots - 64) {   // drop entries not seen recently
        for (auto it = g_slots.begin(); it != g_slots.end();) { if (frame - it->second.lastFrame > 120) it = g_slots.erase(it); else ++it; }
        if (g_slots.size() > kSlots - 64) { g_slots.clear(); g_nextSlot = 0; }
    }
}

int record(uint64_t key, ID3D12Resource* cbRes, uint64_t cbOff, uint32_t frame, bool* havePrev)
{
    *havePrev = false;
    if (!g_st.ready || !cbRes) return -1;
    Slot* s;
    auto it = g_slots.find(key);
    if (it == g_slots.end()) {
        if (g_nextSlot >= kSlots) { g_st.skipped++; return -1; }
        s = &g_slots[key]; s->idx = g_nextSlot++; s->lastFrame = 0; s->size[0] = s->size[1] = 0;
    } else s = &it->second;
    const D3D12_RESOURCE_DESC d = cbRes->GetDesc();
    if (cbOff >= d.Width) return -1;
    const uint32_t size = (uint32_t)((d.Width - cbOff) < kSlotBytes ? (d.Width - cbOff) : kSlotBytes);
    void* p = nullptr; D3D12_RANGE rr = { (SIZE_T)cbOff, (SIZE_T)(cbOff + size) };
    if (FAILED(cbRes->Map(0, &rr, &p)) || !p) { g_st.skipped++; return -1; }
    const uint32_t half = frame & 1;
    memcpy(g_cbPtr + (size_t(s->idx) * 2 + half) * kSlotBytes, static_cast<const char*>(p) + cbOff, size);
    D3D12_RANGE wr = { 0, 0 }; cbRes->Unmap(0, &wr);
    *havePrev = s->lastFrame == frame - 1 && s->size[half ^ 1] > 0;
    s->lastFrame = frame; s->size[half] = size;
    g_st.recorded++;
    if (!*havePrev) g_st.noPrev++;
    return (int)s->idx;
}

static void reset_counters(ID3D12GraphicsCommandList* cl)
{
    D3D12_RESOURCE_BARRIER b[3] = {};
    int n = 0;
    auto add = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) { if (from == to) return; b[n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b[n].Transition = { r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to }; n++; };
    add(g_counters, g_ctrState, D3D12_RESOURCE_STATE_COPY_DEST);
    add(g_soBuf[0], g_soState, D3D12_RESOURCE_STATE_STREAM_OUT); add(g_soBuf[1], g_soState, D3D12_RESOURCE_STATE_STREAM_OUT);
    if (n) cl->ResourceBarrier(n, b);
    cl->CopyBufferRegion(g_counters, 0, g_zero, 0, 256);
    D3D12_RESOURCE_BARRIER c = {}; c.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; c.Transition = { g_counters, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT };
    cl->ResourceBarrier(1, &c);
    g_ctrState = D3D12_RESOURCE_STATE_STREAM_OUT; g_soState = D3D12_RESOURCE_STATE_STREAM_OUT;
}

bool capture(ID3D12GraphicsCommandList* cl, int slot, const RootArgs& ra, const DrawArgs& da)
{
    if (!g_st.ready || slot < 0 || !ra.pso || !ra.rootSig || ra.cbParam < 0 || !ra.cbvSet[ra.cbParam]) return false;
    ID3D12PipelineState* so;
    { std::lock_guard<std::mutex> lock(g_mutex); so = so_pso(ra.pso); }
    if (!so) return false;
    const uint64_t bytes = uint64_t(da.count) * da.instances * 16;   // upper bound (strips emit fewer)
    if (g_soBytesThisFrame + bytes > kSoBytes) { g_st.overflow++; return false; }
    if (!g_frameStarted) { reset_counters(cl); g_frameStarted = true; }
    g_soBytesThisFrame += bytes;

    const uint32_t half = g_curFrame & 1;
    const D3D12_GPU_VIRTUAL_ADDRESS base = g_cbUpload->GetGPUVirtualAddress();
    const D3D12_GPU_VIRTUAL_ADDRESS curVA = base + (uint64_t(slot) * 2 + half) * kSlotBytes;
    const D3D12_GPU_VIRTUAL_ADDRESS prevVA = base + (uint64_t(slot) * 2 + (half ^ 1)) * kSlotBytes;
    ID3D12RootSignature* rs; { std::lock_guard<std::mutex> lock(g_mutex); rs = g_psos[ra.pso].rootSig ? so_root_signature(g_psos[ra.pso].rootSig) : nullptr; }
    if (!rs) return false;
    cl->SetGraphicsRootSignature(rs);
    cl->SetPipelineState(so);
    for (int i = 0; i < 5; ++i) {
        if (ra.tableSet[i]) cl->SetGraphicsRootDescriptorTable(i, ra.tables[i]);
        if (ra.cbvSet[i]) cl->SetGraphicsRootConstantBufferView(i, i == ra.cbParam ? curVA : ra.cbv[i]);
    }
    D3D12_STREAM_OUTPUT_BUFFER_VIEW v = { g_soBuf[0]->GetGPUVirtualAddress(), kSoBytes, g_counters->GetGPUVirtualAddress() + 0 };
    cl->SOSetTargets(0, 1, &v);
    if (da.indexed) cl->DrawIndexedInstanced(da.count, da.instances, da.first, da.vertexOffset, da.firstInstance);
    else cl->DrawInstanced(da.count, da.instances, da.first, da.firstInstance);
    cl->SetGraphicsRootConstantBufferView(ra.cbParam, prevVA);
    v = { g_soBuf[1]->GetGPUVirtualAddress(), kSoBytes, g_counters->GetGPUVirtualAddress() + 64 };
    cl->SOSetTargets(0, 1, &v);
    if (da.indexed) cl->DrawIndexedInstanced(da.count, da.instances, da.first, da.vertexOffset, da.firstInstance);
    else cl->DrawInstanced(da.count, da.instances, da.first, da.firstInstance);
    D3D12_STREAM_OUTPUT_BUFFER_VIEW none = { 0, 0, 0 };
    cl->SOSetTargets(0, 1, &none);
    g_st.captured++;
    return true;
}

void velocity(ID3D12GraphicsCommandList* cl, D3D12_CPU_DESCRIPTOR_HANDLE mvRtv, D3D12_CPU_DESCRIPTOR_HANDLE sceneDsv, uint32_t w, uint32_t h, const D3D12_VIEWPORT& sceneVp)
{
    if (!g_st.ready || !g_frameStarted) return;
    g_frameStarted = false;
    {
        D3D12_RESOURCE_BARRIER b[4] = {};
        b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b[0].Transition = { g_soBuf[0], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE };
        b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b[1].Transition = { g_soBuf[1], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE };
        b[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b[2].Transition = { g_counters, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE };
        b[3].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b[3].Transition = { g_args, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
        cl->ResourceBarrier(4, b);
        g_soState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE; g_ctrState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }
    cl->SetComputeRootSignature(g_argsRs);
    cl->SetPipelineState(g_argsPso);
    cl->SetComputeRootShaderResourceView(0, g_counters->GetGPUVirtualAddress());
    cl->SetComputeRootUnorderedAccessView(1, g_args->GetGPUVirtualAddress());
    cl->Dispatch(1, 1, 1);
    {
        D3D12_RESOURCE_BARRIER b = {}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition = { g_args, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT };
        cl->ResourceBarrier(1, &b);
    }
    const uint32_t cbSlot = g_velCbSlot++ % 4;
    float cb[4] = { sceneVp.Width > 0 ? sceneVp.Width : float(w), sceneVp.Height > 0 ? sceneVp.Height : float(h), 0, 0 };   // pixel scale of the scene viewport
    memcpy(g_velCbPtr + cbSlot * 256, cb, sizeof(cb));
    ID3D12DescriptorHeap* heaps[1] = { g_heap };
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetGraphicsRootSignature(g_velRs);
    cl->SetPipelineState(g_velPso);
    cl->SetGraphicsRootConstantBufferView(0, g_velCb->GetGPUVirtualAddress() + cbSlot * 256);
    cl->SetGraphicsRootDescriptorTable(1, g_heap->GetGPUDescriptorHandleForHeapStart());
    cl->OMSetRenderTargets(1, &mvRtv, FALSE, &sceneDsv);
    D3D12_VIEWPORT vp = sceneVp.Width > 0 ? sceneVp : D3D12_VIEWPORT{ 0, 0, float(w), float(h), 0, 1 }; cl->RSSetViewports(1, &vp);
    D3D12_RECT sc = { 0, 0, (LONG)w, (LONG)h }; cl->RSSetScissorRects(1, &sc);
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->ExecuteIndirect(g_cmdSig, 1, g_args, 0, nullptr, 0);
    {
        D3D12_RESOURCE_BARRIER b[3] = {};
        b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b[0].Transition = { g_soBuf[0], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_STREAM_OUT };
        b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b[1].Transition = { g_soBuf[1], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_STREAM_OUT };
        b[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b[2].Transition = { g_counters, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_STREAM_OUT };
        cl->ResourceBarrier(3, b);
        g_soState = D3D12_RESOURCE_STATE_STREAM_OUT; g_ctrState = D3D12_RESOURCE_STATE_STREAM_OUT;
    }
    g_st.velocityFrames++;
}

} // namespace objmv
