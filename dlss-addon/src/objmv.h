// Per-object motion vectors (Phase 2) for the MGS4 DLSS add-on.
//
// The game's vertex shaders (converted RSX programs: skinning, per-object transforms) are reused unchanged. For every
// dynamic draw the add-on issues ONE extra draw with a stream-out variant of the pipeline (same vertex shader, no
// rasterisation) that captures the clip-space position of every emitted vertex into this frame's buffer. The buffers
// ping-pong: this frame's capture is next frame's "previous positions" for the same draw (same geometry, same n-th
// occurrence in the frame), so no constants are recorded or replayed. At the injection point one draw per object
// rasterises the current positions and writes (previous - current) in pixels into the motion-vector texture, depth-tested
// against the scene depth so only visible object surfaces replace the camera-only vectors.
//
// Cost model (what made v1 slow and what v2 does instead): v1 streamed out twice per draw (current + recorded previous
// constants) under a cloned root signature, which invalidates every root argument and forced a full state restore per
// draw. v2 adds ALLOW_STREAM_OUTPUT to the game's own root signatures when they are created, so the stream-out pipeline
// binds under the game's root signature with the game's root arguments untouched: per draw it is one PSO swap, one
// SOSetTargets, the draw, and the swap back.
#pragma once
#include <d3d12.h>
#include <cstdint>

namespace objmv {
    typedef void (*LogFn)(const char* fmt, ...);

    struct Stats {
        bool ready = false;
        uint32_t psosSeen = 0, rootSigsSeen = 0, rootSigsSoEnabled = 0, soPsos = 0, soPsoFailures = 0, velPsos = 0;
        uint32_t captured = 0, withPrev = 0, skipped = 0, overflow = 0;      // this frame (copied to *Last at new_frame)
        uint32_t capturedLast = 0, withPrevLast = 0, skippedLast = 0, overflowLast = 0;
        uint32_t slotsUsed = 0, velocityFrames = 0;
        // timing, averaged over the last second: GPU ms of the stream-out draws, of the velocity pass, of the whole
        // scene (first scene draw -> after DLSS), and CPU ms spent in capture()/velocity()
        float soGpuMs = 0, velGpuMs = 0, frameGpuMs = 0, cpuMs = 0;
        char lastError[160] = "";
    };

    // Install the device hooks (CreateRootSignature / CreateGraphicsPipelineState / CreatePipelineState) and create the
    // pass resources. Must run before the game creates its root signatures and pipelines (init_device).
    void init(ID3D12Device* device, LogFn log);
    void shutdown();
    bool ready();
    void forget_pso(ID3D12PipelineState* pso);
    void new_frame(uint32_t frame);
    void set_timestamp_frequency(uint64_t hz);

    struct DrawArgs { bool indexed; uint32_t count, instances, first; int32_t vertexOffset; uint32_t firstInstance; };
    // Stream out the clip positions of the draw about to be issued (the command list carries the game's complete state
    // for it). `topology` = the D3D primitive topology set by the game, `jittered` = its clip matrix carries this
    // frame's sub-pixel jitter. Leaves the game's PSO bound again. Returns true if the draw was captured.
    // ownVp: the draw's own viewport when it is not the frame's scene viewport (a 3D window such as the pause-menu
    // model); the velocity pass then rasterises this object into that rectangle instead of the pass viewport.
    bool capture(ID3D12GraphicsCommandList* cl, uint64_t key, ID3D12PipelineState* gamePso, uint32_t topology, const DrawArgs& da, bool jittered, const D3D12_VIEWPORT* ownVp);

    // At the injection point after the camera motion vectors were written: rasterise every captured object that was
    // also captured last frame into mvRtv (R16G16_FLOAT, pixels, prev - cur), depth-tested (reversed-Z, greater-equal)
    // against sceneDsv. The caller puts the MV texture in RENDER_TARGET and the depth in DEPTH_WRITE state first and
    // restores all state after. vp = the viewport the game rendered the scene with. jitterCur/jitterPrev = the NDC
    // offsets the add-on added to the clip matrices this frame and last frame (removed from the captured positions);
    // prevSize = last frame's scene viewport size (dynamic resolution) so previous positions are taken in that scale.
    // manualDepth: a full-size R32 depth (pixel-shader readable) to depth-test against instead of sceneDsv (which may be 0 then).
    void velocity(ID3D12GraphicsCommandList* cl, D3D12_CPU_DESCRIPTOR_HANDLE mvRtv, D3D12_CPU_DESCRIPTOR_HANDLE sceneDsv, uint32_t w, uint32_t h, const D3D12_VIEWPORT& vp,
                  const float jitterCur[2], const float jitterPrev[2], const float prevSize[2], ID3D12Resource* manualDepth);
    bool has_captures();

    // GPU timing of the scene: call at the first scene draw of the frame and after the DLSS evaluation (the latter also
    // resolves this frame's queries). Cheap; works with object motion off (frame time only).
    void mark_frame_begin(ID3D12GraphicsCommandList* cl);
    void mark_frame_end(ID3D12GraphicsCommandList* cl);

    const Stats& stats();

    // Shader identification: FNV-1a hash of the pixel / vertex shader bytecode of a recorded pipeline (0 if unknown).
    uint64_t pso_ps_hash(ID3D12PipelineState* pso);
    uint64_t pso_vs_hash(ID3D12PipelineState* pso);
    // Dump every new VS/PS bytecode to <dir>\<hash>.{vs,ps}.dxbc (to identify the game's post passes); "" = off.
    void set_shader_dump_dir(const char* dir);
}
