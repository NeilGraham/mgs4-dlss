# MGS4 PC port — renderer notes and DLSS plan

Gathered 2026-08-28 from the Steam build (`mgs4.exe` 1.0.0.1, `version.id` = `Pela_[MPA]_x64_BGFX_0.0.3_Release_ww_[Code]a84606af_[DataNew]d06ab525_2026_0825`). Test machine: RTX 5090, driver 616.56, 5120x1440 display (game swapchain 7680x2160 via DSR).

## What the port is

- A PS3 translation layer ("Pela"/"MPA", Virtuos) replaying the original RSX/SPURS work through **bgfx**. `MGSFPSUnlock` patches SPURS task timing, which confirms the emulated PS3 job system.
- bgfx backends compiled in: D3D9, D3D11, D3D12, OpenGL, Vulkan, Noop (RTTI names `RendererContextD3D12@d3d12@bgfx`, build paths `E:\Bola\source-code\module\bgfx.cmake\bgfx\src\renderer_*.cpp`). Default pick is **Direct3D 11**.
- `mgs4.exe` has **no static graphics imports** (only winmm, bink2w64, steam_api64, CRT, xinput…). bgfx `LoadLibrary`s `d3d11.dll` / `d3d12.dll` / `dxgi.dll` / `vulkan-1.dll` at runtime — that is why the game-folder proxies work: `d3d11.dll` = Ultimate ASI Loader 9.7.4, `dxgi.dll` = ReShade 6.8.
- bgfx's D3D11 init calls `D3D11CreateDevice(adapter, HARDWARE, flags=0x21, 6 feature levels)` and retries with fewer levels, then WARP. On total failure bgfx tries D3D12 next (score 10 vs 20 on Windows ≥ 8), then Vulkan/GL.

## Shaders and passes

- Shaders are pre-converted RSX programs packed in `MGS4/common/shaders/vfp_PC.1.pak` (4 MB) and `vfp_PC.2.pak` (11 MB), magic `VPAK`, compressed DXBC (SM5, same blobs serve D3D11 and D3D12). `config/samplerSlotInfo.windows.csv` lists ~978 programs by hash/name.
- Constants are raw PS3 register arrays, not named matrices. Only bgfx-style uniform found: `u_alphaRef4`.
- No TAA, no jitter, no motion-vector pass, no DLSS/FSR/XeSS. Relevant named passes: `cam_blur_shader`, `dmapack_cam_blur[_copy_fb]` (camera motion blur → the engine has a previous-frame camera matrix somewhere), `dg_copy_depth`, `dg_copy_stretch[_ff]`, `dg_depthdraw`, `dg_depthmask`, `dmapack_font`, `dmapack_movie`.
- ReShade Generic Depth sees: 7680x2160 D24S8 (1 draw — clear/UI level), **3840x2160 D24S8, ~2770 draws, reversed-Z (the scene)**, 2048x4096 D32 (shadow map). So the scene RT is not the swapchain: there is an existing stretch/composite step, which is the DLSS insertion point.

## Frame map on D3D12 (Phase 0 result, traced with the v0 add-on)

bgfx records ~4 command lists per frame. Per frame, in order:

1. Backbuffer-sized RT (7680x2160 RGBA8 + 7680x2160 D24S8) bound with 0 draws (bgfx view 0 clear).
2. Shadow map: 2048x4096 D32, ~54 draws.
3. Several 3840x2160 RGBA8 render targets share the one 3840x2160 R24G8 depth. One of them receives the bulk of the
   geometry (hundreds of draws on the title screen, ~2770 in cutscenes). Fullscreen 1-draw passes ping-pong between
   them, some at R16G16B16A16F (HDR/bloom chain), one R32F (depth copy), plus a 1920x1080 downsample.
4. The backbuffer-sized RT is bound with **viewport x=1920 y=0 w=3840 h=2160**: the 4K scene is composited 1:1 into the
   32:9 backbuffer (pillarboxed, no scaling), followed by ~200+ UI draws into the same target.

So "composite draw" = first draw into the backbuffer-sized RT after the scene draws. Its SRV table descriptor 0 is the
final scene texture (confirmed: equals the RT with the most draws that frame).

bgfx D3D12 root layout (graphics): `[0]` sampler table, `[1]` SRV table, `[2]` CBV (vertex uniforms), `[3]` CBF
(fragment uniforms), `[4]` UAV table. Heaps + root signature are set once per command list; PSO on change.
Textures bound as SRV are in `PIXEL_SHADER_RESOURCE`, depth in `DEPTH_WRITE` (or `DEPTH_READ` when sampled).

ReShade 6.8 specifics that make native restore possible: the app's GPU descriptor handles are forwarded unmodified
(`descriptor_table.handle` is the real GPU handle), `device::get_descriptor_heap_offset()` returns the original heap,
`pipeline.handle` / `pipeline_layout.handle` are the real `ID3D12PipelineState*` / `ID3D12RootSignature*`.

`renodx-dlss5` hooks `_nvngx.dll` exports (`NVSDK_NGX_D3D12_CreateFeature/EvaluateFeature/ReleaseFeature`) when the
driver's NGX runtime loads; `EvaluateFeature_C` is not exported by driver 616.56 (harmless error line).

## What DLSS Super Resolution needs (and what exists)

| Input | Exists? |
|---|---|
| Color at internal res, pre-UI | yes (scene RT) |
| Depth | yes (reversed-Z D24S8) |
| Motion vectors | **no** — must be synthesized |
| Sub-pixel jitter in the projection + reported offsets | **no** — must be injected |
| Exposure / reset-on-cut | trivial |

## Plan

**Route A (done):** get bgfx onto D3D12. It was first forced with an ASI (`MGS4_D3D12.asi`, hooking `D3D11CreateDevice` to fail bgfx's D3D11 attempt); the port turned out to have the backend as its own option - Options -> Graphics -> API = DirectX 12 - so the ASI was removed on 2026-09-01 and this is now a setting, not a patch. Unlocks NGX D3D12, DLSS-FG, and DX12-only ReShade add-ons. Route B (D3D11On12 proxy) was the fallback and was not needed.

**Phase 0 — map the frame.** RenderDoc capture (run without ReShade) plus a diagnostic ReShade add-on (API v18, D3D12) that logs pipelines/draws per frame. Find: the scene pass RTV/DSV, the stretch draw that consumes it, the UI boundary, and the constant buffer + offset holding view-projection (spot the projection pattern in a 4x4; confirm by perturbing it). Dump DXBC by hash with RenoDX's dev build.

**Phase 1 — DLAA.**
- Jitter: at the scene pass's constant upload (`update_buffer_region` / `map_buffer_region`), add the Halton(2,3) offset to proj[2][0], proj[2][1] in NDC units (2*jx/W, 2*jy/H). Alternative: shader replacement injecting a jitter cbuffer.
- Motion vectors: compute shader — reconstruct position from depth + inverse current VP, reproject with previous VP, write delta to R16G16_FLOAT. Camera-only.
- NGX: `NVSDK_NGX_D3D12_Init` → `CreateFeature(DLSS, DLAA)` → at the stretch draw, `Evaluate` with color/depth/MV/jitter/exposure=1 → rebind output as the stretch pass input. Ship `nvngx_dlss.dll` beside the exe. Reset on large camera deltas / cut transitions.
- Ghosting mitigation before Phase 2: stencil of skinned draws as DLSS's bias-current-color mask.
- Calling the standard NGX entry points is what lets `renodx-dlss5` hook in ("Insertion: immediately after the game's NGX DLSS output").

**Phase 2 — per-object motion vectors.** Key each scene draw by (VB, IB, shader hash), stash transform/bone constants, replay a velocity pass next frame with replaced VS outputting current − previous clip position. Needs per-vertex-program templates and previous-frame bone palettes. Long.

**Phase 3 — real upscaling.** Only if the UI is drawn after the stretch pass (Phase 0 answers this). On an RTX 5090 the game is bound by the translation layer, not the GPU, so DLAA is the real payoff.

## Alternatives that work today (no code)

- NVIDIA Smooth Motion (driver frame-gen, DX11/12/Vulkan): NVIDIA App → Graphics → mgs4.exe → Driver Settings.
- DLDSR 2.25x for AI supersampling.
- ReShade CAS/SMAA; depth-based effects have a fair chance since Generic Depth finds the scene buffer.

## Useful references

- OptiScaler source — canonical example of wiring NGX/FSR inputs.
- MGSPatriotFix (ShizCalev) — SafetyHook + pattern-scan ASI style that this port tolerates.
- NVIDIA DLSS SDK (GitHub) — D3D11/D3D12/Vulkan NGX API and programming guide.

## Corrections from the DLSS add-on work (2026-08-28, later)

- The composite is exactly **one** draw per frame into the backbuffer-sized RT (viewport 1920,0 3840x2160); the UI is
  already part of the 3840x2160 image it samples. That sampled texture is a **double-buffered final texture** that
  alternates every frame - it is *not* the render target with the most draws (which is where DLSS was first, wrongly,
  applied: everything "worked" but nothing reached the screen).
- bgfx (this build) graphics root layout as observed: `[0]` sampler table, `[1]` CBV/SRV/UAV table (73727-descriptor
  shader-visible heap, double-buffered), `[2]` root CBV; `[3]`/`[4]` unused by the composite draw.
- ReShade 6.8 registers views under their original CPU handle only for `Create*View` (and RTV/DSV copies). For
  CBV/SRV/UAV `CopyDescriptors*` it only fires `copy_descriptor_tables` - so an add-on must mirror those copies itself to
  resolve SRVs bound from a heap that is filled by copying (bgfx: ~700 copies per frame, ~0-6 direct view creations).
- `get_descriptor_heap_offset()` must only be called with GPU handles or genuine ReShade virtual CPU handles;
  decoding a raw CPU pointer indexes ReShade's heap table out of bounds and crashes the game.
- renodx-dlss5 hooks `_nvngx.dll` when `nvngx_dlss.dll` loads (inside the first CreateFeature) and needs to observe a
  CreateFeature to capture its "DLSS contract"; re-creating the feature once after ~1 s solves it.

## Upscaling modes (2026-08-28, evening)

- The game's render targets follow the in-game resolution (observed 3840x2160 and, after the user changed it, 3024x1701
  with a 3024x1890 swapchain and a composite viewport of (0,95 3024x1701)). Every texture at that size (12 of them:
  RGBA8 targets, R16G16B16A16F, R32F, two R24G8 depths) can be shrunk at creation without breaking the frame; viewports
  and scissors of draws into shrunk targets must be scaled (bgfx sets them per view, after OMSetRenderTargets).
- `NGX_DLSS_GET_OPTIMAL_SETTINGS` must be called with the *capability* parameters (`GetCapabilityParameters`), not an
  `AllocateParameters` block - otherwise it fails with `NVSDK_NGX_Result_FAIL_OutOfDate` (0xBAD0000C).
- Rewriting the composite draw's SRV descriptor in place (`CreateShaderResourceView` at the slot's original CPU handle)
  is enough to make it sample the full-size DLSS output; bgfx rewrites the slot next frame anyway.
- Frame generation: DLSS-G/MFG is Streamline-only and needs real motion vectors; deferred until Phase 1b.

## Phase 1b findings (2026-08-28, night)

- Main geometry shaders: vertex constants c[0..3] = row-major view-projection (rows = clip x,y,z,w); c[4..7] = bare
  projection (x-scale 1.73, y-scale -3.08, z-row (0,0,0,near), w-row (0,0,1,0)); c[8..10] look like a view/texture
  matrix; c[12] = a world position (camera). Reversed-Z with infinite far: z_clip == near (48.3 in one scene, 49.4 in another).
- 445 of ~600 scene draws share the c[0..3] block exactly (world geometry with identity model matrix) - that block is
  the VP. Rotating props carry model*VP at c[1..4] (same translation column, rotated x/z rows). Another family has
  c[0] = packed NaN + a view matrix (rotation rows + position) at c[1..4] and no clip matrix in the first 5 registers.
- bgfx reuses a constant region for consecutive draws with unchanged constants, so patch once per (buffer, offset).
- Upload-heap constant regions can be patched from the CPU after bgfx wrote them and before the list executes.
- Laplacian-variance sharpness metrics cannot judge jitter correctness: proper supersampling lowers them (fewer jaggies).
- Launching mgs4.exe directly bounces through Steam (exit 53) and fails when the Master Collection launcher is already
  open; steam_appid.txt next to the exe avoids the relaunch entirely.

## Frame generation via Streamline (2026-08-28, late)

Design of `dlss-addon/src/fg.cpp` and the facts it rests on (Streamline 2.12 headers / 2.13 runtime, sources of
`sl.interposer` read):

- **Hook order.** ReShade vtable-hooks `IDXGIFactory::CreateSwapChain` (its function sits in the factory vtable). The
  add-on MinHooks that same entry, so it runs first and calls Streamline's proxy factory (`slUpgradeInterface` on the
  real factory). Streamline's proxy then calls the base factory, which hits ReShade. Resulting chain:
  `game -> SL proxy swapchain -> ReShade proxy -> DXGI`. This is the same topology as a native Streamline game with
  ReShade's `dxgi.dll` underneath (there SL's own D3D12 proxies wrap ReShade's), so it is a supported configuration.
- **Queue/device.** Streamline's `queryDevice` passes a plain `ID3D12CommandQueue` through to the base factory
  (warning "expecting SL proxy", then the "AMD AGS / other SDK" path). The game holds ReShade's queue proxy, so ReShade
  still recognizes the queue and wraps the real swapchain. `slSetD3DDevice` receives `queue->GetDevice()` (ReShade's
  device proxy) so Streamline/DLSS-G create their resources and queues through the same object SL's swapchain proxy
  reports; the add-on's native device pointer is only used for the adapter LUID. Routing `CreateCommandQueue` through
  an SL device proxy was rejected: SL would unwrap to the native queue and ReShade would then not wrap the swapchain.
- **Off-screen rendering.** With `sl.dlss_g` loaded the proxy swapchain's `GetBuffer` returns DLSS-G's off-screen
  buffers, not the real backbuffers, so the composite-draw detection accepts those too (`fg::is_app_backbuffer`,
  recorded from a hook on the proxy's `GetBuffer`; cleared on `ResizeBuffers`).
- **Threads.** DLSS-G presents from its own thread through ReShade's proxy, so ReShade's `present` event (and ReShade's
  effects) run for every presented frame, real and generated, off the render thread. The add-on's per-frame rollover
  therefore moved to a hook on the proxy swapchain's `Present`/`Present1` (the game's call, render thread); the ReShade
  `present` event is ignored while the swapchain is proxied.
- **Per frame:** first scene draw -> frame token for `g_frame`, `slReflexSleep`, PCL markers SimulationStart/End +
  RenderSubmitStart; at the DLSS insertion (after the NGX evaluate, depth/color/MV in NPSR, output in UAV) ->
  `slSetConstants` (P and clip<->prev clip from the same unjittered VP the MV pass uses, transposed for SL's row-vector
  convention, `depthInverted`, `cameraMotionIncluded`, pixel `mvecScale = 1/size`, jitter, reset) and
  `slSetTagForFrame` (depth + MVs `eOnlyValidNow` with the command list; HUD-less color = DLSS output in pre-post
  insertion; backbuffer tag with the game-image extent when letter/pillar-boxed); game Present hook -> RenderSubmitEnd +
  PresentStart, rollover, SL present, PresentEnd.
- **Options** live via `slDLSSGSetOptions`: Off / 2x / 3x / 4x / Dynamic(`eDynamic`, `dynamicTargetFrameRate`). If
  `DLSSGState::bIsDynamicMFGSupported` is false the add-on picks the multiplier itself once per second from the measured
  game frame rate. Reflex through `slReflexSetOptions`.
- **Pixel-format note:** depth is tagged with its R24G8 typeless format; MVs are R16G16_FLOAT in pixels (top-left
  origin, `prev - cur`), which matches DLSS-G's expectation with `mvecScale = (1/w, 1/h)`.

### What the testing showed (same night)
- bgfx resizes with `ResizeBuffers1` right after creation; through Streamline that crashed inside the plugin /
  ReShade / DXGI chain, so the proxy's `ResizeBuffers1` is turned into `ResizeBuffers`.
- Streamline must get the **native** device: with ReShade's proxy device NGX (process-wide, initialized first by
  sl.common) made the DLSS SR DLL crash in D3D12Core at `CreateFeature`.
- DLSS-G creates its own high-priority present queue *inside its CreateSwapChain hook* and creates the real swapchain
  on it. ReShade only wraps swapchains whose queue is one of its proxies, so the add-on hooks the native device's
  `CreateCommandQueue`/`CreateCommandQueue1` and routes calls that do not come from ReShade's own module through
  ReShade's device proxy (return-address check). Result: ReShade overlay/add-ons keep working under DLSS-G.
- Streamline is loaded only when `FrameGen != 0` at startup; the default build is the pre-FG behavior.

## PostDof findings (2026-09-01)

- The DoF passes' constant buffer struct is 400 bytes; it was written into 256-byte ring slots of a 1 KB upload buffer.
  Frame N+1's write overlapped bytes 256..400 of frame N's slot - `halfSize/fullSize/depthScale/depthSize/stepUV/
  depthJitter/hasMask/maskScale`, i.e. every layout-critical parameter - and slot 3 ran 144 bytes past the resource.
  With the GPU a frame or two behind the CPU (frame generation, DRS stepping under load) a frame could run its DoF
  with the next frame's depth scale and jitter: the blur layer flickering into the top-left sub-rect. The DoF descriptor
  heap (12 descriptors) was likewise rewritten every frame with no ring. Both are ring-buffered now (8 frames).
- The circle of confusion is evaluated at the game's own CoC draw (the draw is still skipped), from the depth copy and
  constants of that draw, into a half-res R16F texture on the full grid; the insertion only adds the DLSS output's
  color (`dof_pack_cs`) before the gather and the blend. Any missing input at the CoC draw (constants, depth copy,
  dispatch) leaves the game's own DoF in place for that whole frame.
- The depth de-jitter offset is in depth-copy texels (sub-rect pixels): scaled by k on dynamic-resolution frames.
- The game's upload-heap constant buffers are mapped once and kept (a map per scene draw before); `read_cbv` and the
  jitter patch share the cache, dropped when the resource is destroyed.
- Still open: on resolution-step frames the game's own chain disagrees with itself for a frame (measured before: the
  native frame flashes too); `DofStepFreeze=1` holds the previous DLSS output for that frame. If flashes remain at
  steps, the next experiment is to read the depth at the *scene viewport's* scale on those frames rather than the CoC
  viewport's (the `PostDof: fN depth copy ...` log lines show whether the two disagree).

## Main-path pass (2026-09-01)

Reviewed: jitter (Halton 2,3, 8 phases at DLAA, added to the clip rows in full-grid pixels, reported to DLSS in the same
units; sign tuned empirically), camera vectors (full-grid pixels from the sub-res depth, unjittered VP, direction at
infinity for the far plane), object vectors (stream-out, de-jittered with this and last frame's offsets, manual depth
test against the stretched depth), the NGX contract (MVLowRes + DepthInverted, MVs in pixels, exposure 1, LDR input,
sub-rect = the full texture on the full grid) and DLSS-G's constants. Nothing structurally wrong. Changed:
- `InFrameTimeDeltaInMsec` is now filled from the game-frame interval measured at the rollover (was 0 = unknown); the
  programming guide asks for it - the model uses it to relate vector magnitudes to speed.
- One shader-visible descriptor heap for every add-on pass (the DoF ring follows the utility slots in `g_mvHeap`):
  the insertion switches heaps once each way instead of per pass. objmv keeps its own (SRVs fixed per parity).
- `table_to_cpu` caches heap type / increment / CPU start per heap: it runs for each of bgfx's ~700 descriptor copies
  per frame and for every SRV resolved from a draw's table.
- Upload constant buffers mapped once (jitter patch + `read_cbv`), one pipeline-state lookup per draw.
- `ObjectMVProps=1`: object vectors also for draws whose clip matrix is not the camera's (own model matrix).
Not changed, worth trying: verify the jitter sign with a static camera by phase-correlating consecutive raw DLSS inputs
against the reported offsets; the 33 MB copy-back per frame could go if the DoF blend wrote the game's texture via an
RTV draw instead of a UAV (the game's targets lack UAV access).

## Gameplay flicker on PMC soldiers (2026-09-01, afternoon)

Read from a 13-minute gameplay log (Act 1, Snake among PMC soldiers, FrameGen dynamic to 240, PostDof switched off from
the overlay mid-run to test):
- 94 history resets; 31 of them with rotation delta 0.0000-0.007 and a position jump of 1600-2600 units, one every
  ~30 s in gameplay. The game's units are millimeters (near plane ~49, camera ~30 m from the origin), so these are 2 m
  camera snaps - aiming in/out, cover - not cuts. Every one cleared the DLSS + NR history for a frame. `CutPosLimit`
  (default 6000) replaces the hard-coded 1500; real cuts in the same log had rotation deltas of 0.5-2.0 and are still caught.
- Object vectors looked healthy (captured == with history, no overflow), but the pairing across frames was geometry key +
  n-th occurrence. Every PMC soldier is the same mesh; as the camera walks around them their draw order changes and a
  soldier inherits another soldier's previous positions: a frame of bogus vectors exactly on the body. Occurrences now
  carry the head of their vertex constants (32 floats: per-instance transform / first bones) and pair with the nearest
  unclaimed previous occurrence of the same geometry and vertex count. The stats line reports `re-paired by signature N`.
- 165-174 scene draws per frame have no clip matrix in their first 9 registers; the logged samples into the geometry
  target are screen-space effects (color constants, 3784x2128 / 1892x1064 sizes), not characters.
- Remaining suspects if the on-body flicker persists: DLSS-G's interpolated frames (switch Frame generation to Off in
  the overlay - live - and compare), and `DebugMode=9` to see whether the vector silhouette of a soldier sits on the
  soldier every frame.
- Follow-up the same afternoon: the on-body flicker only shows with frame generation on. Since the windowed state
  (3619x2036 backbuffer, 3784x2128 render) fg.cpp had been *dropping* the HUD-less and UI hints because DLSS-G only takes
  them at the color size - so DLSS-G was guessing the HUD from the backbuffer every frame, and its UI heuristics act on
  any high-contrast detail (a soldier's gear). The hints are now rescaled (resample_cs, bilinear supersample) into
  backbuffer-sized copies at the composite draw - when the frame's HUD layer is complete - and tagged valid-until-present
  (`FG: HUD-less / UI hints rescaled to the backbuffer size` in the log). DebugMode 9 (vector field over the image) and
  the DoF views are in the overlay's Debug combo now.
- Same afternoon, from the live log's `no clip matrix` samples: one PSO family carries **bone matrices from register 0 on**
  (three-row affine blocks, translation rows with w = 1) - the skinned character shaders keep their view-projection
  after the palette, far past the 9 registers the jitter patch searched. Those draws were never jittered: characters sat
  still while DLSS assumed the frame's jitter (a sub-pixel wobble on every character), and with frame generation the
  interpolated frames (exact object vectors) alternate with the wobbling real frames at 240 Hz - the FG-only flicker on
  the PMC soldiers. The patch now learns the matrix offset once per vertex shader (scan of up to 1024 floats for the
  clip-matrix signature, validated as the camera against last frame's VP, retried every 600 frames) and jitters at that
  offset. Log: `clip matrix of vertex shader ... found at c[N]`; stats: `patched N (M at a learned deep offset)`.
- Recording `gold/raw/rec_20260901_134224.mkv` with DebugMode=9 (vector field over the image), 4K60: frame 776 Snake's
  suit saturated green, 777 saturated red (a sign flip = two captures of one geometry swapping roles between frames:
  the same mesh drawn twice a frame in two spaces, with the order or the count changing); frame 779 the lying soldier's
  torso + arm saturated magenta for one frame (previous positions from another instance of the same mesh or another
  space). DLSS SR hides such a frame behind its color validation; DLSS-G warps geometry with it - the FG-only flicker.
  Two nets now: (a) objmv pairs occurrences of one geometry by the head of their vertex constants (instances), and
  (b) object vectors are rasterized into their own texture over a sentinel and merged into the camera vectors only
  where |object - camera| <= ObjectMVMaxDelta (64 px; wrong pairings are tens to hundreds of pixels) - mvmerge_cs.
  objmv logs `geometry X drawn N+ times this frame (M last frame)` with VS/PS hashes to identify the second pass.
- The first build with the merge pass (14:00) was broken: an appended comment had swallowed the tail of the shared
  constant buffer's one-line resource description, `MV: constant buffer creation failed 0x80070057`, the motion-vector
  pass never initialized and every evaluation ran with reset = 1 - raw jitter visibly shaking on a paused screen. Fixed
  14:13. From that run's log: the bone-first vertex shaders (3f0517db..., 975b7a68..., c9e1924f..., f00558961...) have
  **no camera matrix in their first 256 registers** either - the VP is deeper than 1 KB or those shaders get the view
  and projection some other way (a projection at a fixed register with bones pre-multiplied by the view?). Only 2-4
  draws per frame pick up a learned deep offset. The objmv diagnostics show many skinned geometries drawn 2-4 times per
  frame under different vertex/pixel shaders (e.g. vs b3c51eb9... with ps cf56ac1f / c6a19156 / d777cc29, vs
  f143b014..., 918379677..., 29fd7f25...): multi-pass character rendering, which is what the merge's plausibility bound
  is there for.
- Performance vs 9d87c29 (user report, 15:00): game pacing unchanged at 16.6 ms but the scene GPU window rose (12.4-12.9
  -> 13.1-15.3 ms, different scene content included) and generated frames live in what is left of the 16.7 ms. Cut back:
  the FG hint rescale is opt-in (`FGHintRescale=0`), `ObjectMVMaxDelta=0` restores the direct velocity path (no extra
  texture, clear or merge pass), and the pairing signature comes from the constants the jitter patch already read per
  region (no second write-combined read per capture). Default-on GPU delta vs 9d87c29 is now the merge pass alone
  (~0.1 ms); DebugMode=9 itself adds a 4K blend pass every frame and must be off for any A/B.
- The merge pass's camera-relative bound (|object - camera| <= 64 px) was wrong for the followed third-person player:
  Snake's screen motion is ~0 while the camera vector at 2 m carries the full parallax (60-150 px a frame when moving),
  so his legitimate vectors were dropped and his edges reprojected from the background - "low-resolution" edges on near
  objects while moving. Replaced by two absolute tests in velocity_ps.hlsl: |mv| <= ObjectMVMaxPixels (200) and the
  screen-space gradient of the vector field (ddx/ddy, taken before any discard) <= ObjectMVMaxGradient (4 px/px). The
  first catches another-instance pairings (hundreds of px, smooth), the second the same-mesh-other-projection pairings
  (wild across the surface). The separate object-vector texture, its clear and the merge compute pass are gone again:
  GPU work equals 9d87c29.
