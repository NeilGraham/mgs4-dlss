// Per-object motion vectors (Phase 2) for the MGS4 DLSS add-on.
//
// The game's vertex shaders (converted RSX programs: skinning, per-object transforms, whatever) are reused unchanged:
// for every dynamic draw the add-on runs the same VS twice with stream output capturing SV_Position - once with the
// constants of this frame, once with the constants it recorded for the same draw last frame. One indirect draw then
// rasterises the current positions and writes (previous - current) in pixels into the motion-vector texture,
// depth-tested against the scene depth so only visible object surfaces replace the camera-only vectors.
#pragma once
#include <d3d12.h>
#include <cstdint>

namespace objmv {
    typedef void (*LogFn)(const char* fmt, ...);

    struct Stats {
        bool ready = false;
        uint32_t psosSeen = 0, rootSigsSeen = 0, soPsos = 0, soPsoFailures = 0;
        uint32_t recorded = 0, captured = 0, noPrev = 0, skipped = 0, overflow = 0;   // this frame (copied to *Last at new_frame)
        uint32_t recordedLast = 0, capturedLast = 0, noPrevLast = 0, skippedLast = 0;
        uint32_t slotsUsed = 0, velocityFrames = 0;
        char lastError[160] = "";
    };

    // Install the device hooks (CreateRootSignature / CreateGraphicsPipelineState) and create the pass resources.
    // Must run before the game creates its pipelines (init_device).
    void init(ID3D12Device* device, LogFn log);
    void shutdown();
    bool ready();
    void forget_pso(ID3D12PipelineState* pso);
    void new_frame(uint32_t frame);

    // Step 1 (before the add-on patches the draw constants): copy the draw's root-CBV constants into the slot for
    // `key` and report whether the same draw was recorded last frame. Returns a slot index or -1.
    int record(uint64_t key, ID3D12Resource* cbRes, uint64_t cbOff, uint32_t frame, bool* havePrev);

    struct RootArgs {
        ID3D12RootSignature* rootSig; ID3D12PipelineState* pso;
        D3D12_GPU_DESCRIPTOR_HANDLE tables[5]; bool tableSet[5];
        D3D12_GPU_VIRTUAL_ADDRESS cbv[5]; bool cbvSet[5];
        int cbParam;   // root parameter carrying the vertex-shader constants
    };
    struct DrawArgs { bool indexed; uint32_t count, instances, first; int32_t vertexOffset; uint32_t firstInstance; };
    // Step 2: stream out current + previous clip positions of the draw (needs havePrev). Changes root signature, PSO,
    // root arguments and SO targets on the command list; the caller restores its state afterwards.
    bool capture(ID3D12GraphicsCommandList* cl, int slot, const RootArgs& ra, const DrawArgs& da);

    // Step 3, at the injection point after the camera motion vectors were written: rasterise the captured objects into
    // mvRtv (R16G16_FLOAT, pixels, prev - cur), depth-tested (reversed-Z, greater-equal) against sceneDsv. The caller
    // puts the MV texture in RENDER_TARGET and the depth in DEPTH_WRITE state first and restores all state after.
    // vp = the viewport the game rendered the scene with (the port may render into a sub-viewport of its targets).
    void velocity(ID3D12GraphicsCommandList* cl, D3D12_CPU_DESCRIPTOR_HANDLE mvRtv, D3D12_CPU_DESCRIPTOR_HANDLE sceneDsv, uint32_t w, uint32_t h, const D3D12_VIEWPORT& vp);
    bool has_captures();

    const Stats& stats();
}
