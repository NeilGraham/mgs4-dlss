// DLSS Frame Generation (Streamline sl.dlssg) for the MGS4 DLSS add-on.
#pragma once
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdint>

namespace fg {
    typedef void (*LogFn)(const char* fmt, ...);

    struct Settings {
        int mode = 0;              // 0 off, 1 = 2x, 2 = 3x, 3 = 4x, 4 = dynamic (target fps)
        float targetFps = 0.0f;    // dynamic mode: 0 = monitor refresh rate
        int reflex = 1;            // 0 off, 1 low latency, 2 low latency + boost
    };
    struct Status {
        bool loaded = false, initialised = false, supported = false, swapchainProxied = false, active = false;
        bool dynamicSupported = false, vsyncSupported = false;
        uint32_t maxFrames = 1, framesPresented = 0, statusFlags = 0;
        uint64_t vramBytes = 0;
        uint32_t generatedPresents = 0;   // presents seen that were not game frames
        uint32_t adaptiveFrames = 0;      // multiplier chosen by the adaptive controller (fallback when dynamic MFG is unsupported)
        char lastError[160] = "";
        char slVersion[32] = "";
    };
    struct CameraInput {
        const float* vp;           // current unjittered view-projection, row-major rows = clip x,y,z,w (column-vector convention)
        const float* prevVp;       // previous frame's
        float jitterX, jitterY;    // pixels
        bool reset;
        uint32_t renderW, renderH; // size of depth / motion vectors
    };
    struct FrameInputs {
        ID3D12GraphicsCommandList* cmd;
        ID3D12Resource* depth; DXGI_FORMAT depthFormat; uint32_t depthState;
        ID3D12Resource* mv;    uint32_t mvState;         // R16G16_FLOAT, pixels
        ID3D12Resource* hudless; DXGI_FORMAT hudlessFormat; uint32_t hudlessState;   // optional
        ID3D12Resource* ui; DXGI_FORMAT uiFormat; uint32_t uiState;                  // optional UI colour + alpha layer (same size as hudless)
        uint32_t renderW, renderH;                        // valid region of depth / vectors (dynamic resolution sub-rect)
        uint32_t texW, texH;                              // real size of the depth / vector textures (0 = renderW/H)
        uint32_t hudlessW, hudlessH;                      // real size of the HUD-less / UI textures (0 = renderW/H)
        bool hudlessSubrect;                              // HUD-less content occupies the renderW x renderH sub-rect of its texture
        uint32_t bbW, bbH;                                // backbuffer size
        int32_t vpX, vpY; uint32_t vpW, vpH;              // game image rectangle inside the backbuffer (subrect), 0 size = full
    };

    // Call once the D3D12 device exists and BEFORE the game creates its swapchain. Loads sl.interposer.dll from gameDir,
    // initialises Streamline in manual-hooking mode and hooks IDXGIFactory::CreateSwapChain* so the game's swapchain is
    // created through Streamline's proxy (that is how generated frames get presented).
    void init(ID3D12Device* device, const wchar_t* gameDirW, LogFn log);
    void shutdown();

    void set_settings(const Settings& s);      // live
    const Settings& settings();
    const Status& status();                    // refreshed by poll()
    void poll();                               // once per game frame: state refresh + adaptive controller

    // Frame flow (game frames only):
    void frame_begin(uint32_t frameIndex);     // first scene draw: Reflex sleep + simulation/render-submit-start markers
    void frame_inputs(uint32_t frameIndex, const FrameInputs& in, const CameraInput& cam);   // at the DLSS insertion point
    void frame_end(uint32_t frameIndex);       // at the game's present (from the swapchain Present hook, before SL runs)

    // Called on the game's own Present (render thread, before Streamline runs) - the add-on's per-frame rollover.
    // Streamline presents generated frames itself, below this hook, so those never reach the callback.
    void set_frame_callback(void (*fn)());
    // With DLSS-G loaded the game renders into Streamline's off-screen buffers (handed out by the proxy swapchain's
    // GetBuffer) instead of the real backbuffers; this reports whether a resource is one of them.
    bool is_app_backbuffer(uint64_t handle);
    // True while Streamline code may be creating resources or recording copies through ReShade's device proxy (inside
    // one of our Streamline calls, or on a thread other than the game's render thread). The add-on must not treat
    // those as game work (texture shrinking, copy-to-backbuffer insertion).
    bool inside_streamline();
    // The device object Streamline was given (the one the game's queue reports; ReShade's proxy), or nullptr.
    ID3D12Device* sl_device();
}
