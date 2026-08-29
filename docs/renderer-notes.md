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

**Route A (done):** force bgfx onto D3D12 with `MGS4_D3D12.asi` (see repo README). Unlocks NGX D3D12, DLSS-FG, and DX12-only ReShade add-ons. Route B (D3D11On12 proxy) is the fallback and was not needed.

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
