# mgs4-dlss

Real DLSS (DLAA and the upscaling modes) for the PC port of *Metal Gear Solid 4* (Master Collection Vol. 2), built as a ReShade add-on. The NGX feature it creates can be hooked by NGX-based add-ons — **directly compatible with the DLSS 5 Neural Rendering add-on (`renodx-dlss5.addon64`)**, which is auto-detected: with it loaded, DLAA runs on the final image so NR works at full strength.

## Status

| Step | State |
|---|---|
| Route A — run the port on bgfx's built-in Direct3D 12 backend | **Done** — the game has a native renderer option (Options -> Graphics, `api=dx12` in `mgs4_savedata_win\<steamid>\mgs4\mgs4.savedsettings`); `d3d12-switch/` remains as a fallback |
| Phase 0 — map the frame (scene target, depth, composite draw) | **Done** — see docs |
| Phase 1a — NGX DLSS (DLAA) created + evaluated every frame, NGX add-ons can hook it | **Done** — `dlss-addon/` (v1: zero jitter / zero motion vectors) |
| Phase 1b — camera jitter + camera-only motion vectors | **Implemented** (needs visual tuning) — see below |
| In-overlay controls (ReShade Add-ons tab) | **Done** |
| Frame generation (Streamline DLSS-G: 2x/3x/4x, dynamic target fps, Reflex; live switching) | **Done** — `dlss-addon/src/fg.cpp` |
| Phase 2 — per-object motion vectors (stream-out of the game's vertex shaders) | **Implemented, experimental** (`ObjectMV=1`; off by default: correct output, but currently halves the frame rate — needs the single-pass/ping-pong design) |
| Dynamic resolution handling (DLSS on the scene sub-rect) | **Done** — `DRS=1` |
| Phase 3 — real upscaling (internal res < output res) | maybe |

See [docs/renderer-notes.md](docs/renderer-notes.md) for what we know about the port and the full plan.

## dlss-addon (`mgs4_dlss.addon64`)

A ReShade add-on (API 20, D3D12 only) that creates a real NGX DLSS Super Resolution feature (DLAA, preset K) and evaluates it every frame:

1. Tracks what bgfx binds per command list (RT/DS, viewport, root descriptor tables, root CBVs, root signature, PSO).
2. Mirrors bgfx's `CopyDescriptors` traffic (`copy_descriptor_tables` event) into its own slot→resource map, because ReShade does not register copied CBV/SRV/UAV descriptors in its view map. That makes the SRVs of any draw resolvable.
3. At the one draw per frame into the backbuffer-sized target (the 1:1 composite of the finished 3840x2160 frame, UI included), resolves the texture that draw samples — a double-buffered final texture, *not* the render target with the most draws — pairs it with the depth buffer that was bound with it, and calls `NGX_D3D12_EVALUATE_DLSS_EXT` with color/depth/(zero) motion vectors.
4. Copies the DLSS output over the sampled texture, then natively re-applies bgfx's descriptor heaps, root signature, PSO and root parameters so the composite draw is unaffected.
5. Re-creates the feature once after 120 evaluations: NGX-hooking add-ons install their hooks when `nvngx_dlss.dll` loads (inside our first `CreateFeature`), so they would otherwise never see the create call that carries the "DLSS contract".

Because the NGX calls go through the standard `_nvngx.dll` exports, NGX-hooking add-ons see them: `renodx-dlss5` reports `DLSSNR ACTIVE`, creates its NR feature after ours and evaluates it every frame.

Verified 2026-08-28: NGX init OK on RTX 5090 / 616.56, `CreateFeature` OK, ~120 evaluations/s (≈80 with DLSS 5 NR active), HUD/UI intact, and `DebugMode=1` paints the displayed image magenta (proves the insertion path).

### Build / install

Requirements: MSVC Build Tools (the `build.bat` calls `vcvars64.bat` from VS 18 BuildTools — adjust the path if yours differs), ReShade 6.8 installed as `MGS4\dxgi.dll`, the D3D12 switch above, and `nvngx_dlss.dll` in `MGS4\` (copy `third_party/DLSS/lib/Windows_x86_64/rel/nvngx_dlss.dll` or let the NVIDIA app override supply it).

```bat
dlss-addon\build.bat                       :: -> build\mgs4_dlss.addon64
copy build\mgs4_dlss.addon64 "<game>\MGS4\"
```

`MGS4\mgs4_dlss.ini`:

```ini
[DLSS]
Enabled=1                ; live-reloaded every ~second
Mode=DLAA                ; DLAA (=Native) | Quality | Balanced | Performance | UltraPerformance  - needs a game restart
InternalRes=3840x2160    ; size of the game's render targets = your in-game resolution; auto-detected and written on first run
Preset=11                ; NVSDK_NGX_DLSS_Hint_Render_Preset_K (transformer). 10 = J
Sharpness=0              ; 0..100 (live)
LogEveryN=600
RecreateAfter=120
DebugMode=0              ; live: 1 = magenta path test, 2 = bypass DLSS (A/B), 3 = trace 3 frames, 4 = analyse draw constants
Jitter=1                 ; live: Halton camera jitter patched into scene draw constants
JitterSignX=1            ; NDC sign conventions (defaults follow the DLSS/Unreal convention)
JitterSignY=-1
MotionVectors=1          ; live: camera-only motion vectors from depth (compute pass)
```

Log: `MGS4\logs\mgs4_dlss.log`.

### DLSS modes (upscaling)

`Mode` other than DLAA makes the game render smaller and lets DLSS upscale:

1. At device creation NGX's optimal settings give the render resolution for the mode (e.g. 3840x2160 Quality -> 2560x1440, Performance -> 1920x1080, Ultra Performance -> 1280x720).
2. Every texture the game creates at `InternalRes` is shrunk to the render resolution (`create_resource` event); viewports and scissors of draws into shrunk targets are scaled to match.
3. At the composite draw DLSS upscales the shrunk final texture into a full-size output and the draw's SRV descriptor is rewritten in place to sample that output, so the composite and the backbuffer are untouched.

Because the game's render-target size follows the in-game resolution, `InternalRes` must match it; the add-on writes the detected value to the ini whenever it differs (change resolution in-game -> restart once). Verified in Performance mode (1512x850 -> 3024x1701): correct composition, HUD and pillarboxing; other modes use the same path but were not individually tested.

**Expect it to look soft.** Without sub-pixel jitter DLSS has no extra information to reconstruct detail from, so the upscaling modes currently behave like a good temporal upscaler with no supersampling. They are useful for testing NR/DLSS at lower cost, not for image quality yet - DLAA is the quality mode until jitter lands. On an RTX 5090 the game is not GPU-bound anyway.

### Frame generation (DLSS-G / Multi-Frame Generation via Streamline)

The add-on drives **NVIDIA Streamline** (`sl.interposer.dll`, `sl.dlss_g.dll`, `sl.reflex.dll`, `sl.pcl.dll`, `sl.common.dll` + `nvngx_dlssg.dll`, which must sit next to `mgs4.exe`) in *manual hooking* mode from inside the ReShade add-on:

1. `slInit` runs in the `init_device` event, i.e. after the D3D12 device exists but before bgfx creates its swapchain. The add-on then hooks `IDXGIFactory::CreateSwapChain`/`CreateSwapChainForHwnd` in front of ReShade and creates the game's swapchain through Streamline's proxy factory. The result is `game -> Streamline proxy swapchain -> ReShade -> DXGI`: Streamline intercepts `Present` and inserts the generated frames, ReShade (and this add-on's `present` event) run for every presented frame, so the add-on counts a game frame only when scene draws happened.
2. Every game frame it feeds Streamline what DLSS-G needs: a frame token, Reflex sleep + PCL markers (simulation, render-submit, present), the camera constants derived from the same clip matrix the DLAA path uses (projection, clip<->prev clip, camera position/axes, reversed-Z near, jitter, cut/reset flag), and tags for **depth**, the add-on's **motion vectors** (camera-only, pixels) and — in pre-post insertion — the anti-aliased **HUD-less colour**. When the game image is letter/pillar-boxed, the backbuffer tag carries the game-image rectangle so only that region is interpolated.
3. `slDLSSGSetOptions` is applied live from the overlay / ini: **Off, 2x, 3x, 4x** or **Dynamic** with a **target frame rate** (`DLSSGMode::eDynamic` + `dynamicTargetFrameRate`, 0 = monitor refresh). If the driver does not report dynamic multi-frame generation, the add-on falls back to its own controller: it measures the game frame rate every second and picks the 2x/3x/4x multiplier that lands closest to the target. Reflex (Off / On / On + Boost) is set through `slReflexSetOptions`.

Ini keys: `FrameGen` (0 off, 1 = 2x, 2 = 3x, 3 = 4x, 4 = dynamic), `FGTargetFps`, `Reflex` (0/1/2). Status (Streamline/DLSS-G version, DLSS-G status flags, max multiplier, dynamic-MFG and vsync support, VRAM, presented/generated frames) is shown in the overlay. Streamline's own log goes to `logs\sl.log`.

Notes:
- Streamline is only loaded when `FrameGen` is non-zero **at startup** (it has to wrap the swapchain when the game creates it), so the first switch from Off needs a restart; after that Off/2x/3x/4x/Dynamic and the target frame rate change live. With `FrameGen=0` the add-on behaves exactly as without frame generation.
- Verified 2026-08-28 (RTX 5090, driver 616.56, Streamline 2.12.129 runtime via the NVIDIA app override, DLSS-G 310.8): ReShade keeps its overlay and add-ons (the DLSS-G present queue is created through ReShade's device proxy on purpose), DLAA + DLSS 5 NR keep working, `DLSS-G interpolation state changed ... enabled`, dynamic mode reported as supported and accepted (`eDynamic` disables vsync/RSync by itself).
- The game runs with vsync on (sync interval 1); DLSS-G works with it (driver reports vsync support) but latency is lower with the game's vsync off. `MGSFPSUnlock`'s limiter caps the *game* frame rate — generated frames come on top.
- **HUD on generated frames.** In pre-post insertion DLSS-G gets the anti-aliased image before the HUD as HUD-less colour. In composite insertion (DLSS 5 NR loaded) the HUD is inside the image, so the add-on (1) replays the game's HUD draws (depth-off draws into the final texture that sample no scene-sized input) into its own RGBA layer, tagged as `UIColorAndAlpha`, and (2) captures the final texture right before its first HUD draw and builds a true HUD-less colour: the DLAA output with the pre-HUD capture under the UI layer's pixels. DLSS-G uses the UI layer when its UI recomposition is available; when it is not (the NVIDIA app's frame-generation preset override disables it — `Disabling bUIRecompositionSupported due to preset override` in `logs\sl.log`) it derives the HUD from backbuffer minus HUD-less colour, which is why the HUD-less image must really lack the HUD. Debug modes "Visualise UI layer" and "Visualise HUD-less colour" show both inputs; verified in Act 1: the HUD-less view has no HUD elements while the frame does. Full-screen menus (Mk.II menu, credits) are rendered through scene-sized layers and are not treated as HUD (they are static anyway).
- Known: on the two frames where the DLSS SR feature is (re)created (`RecreateAfter`), no inputs are tagged, so Streamline logs "Unable to find common constants" once and DLSS-G skips interpolation for that frame.
- Alternative without this add-on: NVIDIA Smooth Motion (driver-level 2x, NVIDIA App -> Graphics -> `mgs4.exe` -> Smooth Motion).

### Overlay controls

The add-on has its own panel in ReShade's **Add-ons** tab (Home key): Enable, DLSS mode (applies on restart — the game creates its render targets once at startup; the panel says so when the selection differs from the active mode), DLSS preset (J/K, applied live by re-creating the feature), sharpness, camera jitter, camera motion vectors, debug modes, frame generation (Off/2x/3x/4x/Dynamic + target fps, Reflex; applied live), and live status (feature size, evaluations/s, jitter, VP detection, MV resets). Every control writes `mgs4_dlss.ini`.

### Phase 1b: camera jitter and camera-only motion vectors

Scene draws carry a row-major clip matrix (rows = clip x, y, z, w) in their vertex constants — `c[0..3]` for the main
geometry shaders, `c[1..4]` for others. It is recognisable without knowing the shader: the w-row's xyz is a unit vector
(view-space depth direction) and the z-row has no x/y (reversed-Z, `z_clip = near`). Per frame the add-on:

1. Patches every scene draw's matrix in bgfx's upload heap once per constant region: `row_x += ox·row_w`,
   `row_y += oy·row_w` with `ox = +2·jx/W`, `oy = −2·jy/H` (Halton 2,3; 8 phases at DLAA), and reports `(jx, jy)` to
   DLSS. This is the same convention as Unreal's DLSS integration. `JitterSignX/Y` in the ini flip it if needed.
2. Picks the frame's view-projection by majority vote over the `c[0]` blocks (identity-model geometry), keeps the
   previous frame's, and runs a compute pass (`src/mv_cs.hlsl`) that reprojects each depth pixel through
   `prevVP · inv(VP)` into camera-only motion vectors (pixels, pointing to the previous position).
3. Detects camera cuts (view direction or offset jumps) and raises `InReset`.

State of tuning: ~60–75% of scene draws expose a recognisable matrix; the rest (HUD/orthographic draws, one shader family
with a different constant layout) render unjittered, so overlay elements can look slightly softer with jitter on.
Character animation still has no motion vectors (Phase 2). Use the overlay toggles to compare.

### Phase 2: per-object motion vectors (`ObjectMV`, experimental)

Characters and props get real motion vectors without touching a single shader: for every dynamic draw (skinned meshes; props with `DynamicMaskProps`) the add-on records the draw's vertex-shader constants (before the jitter patch) keyed by geometry + occurrence, and — when the same draw was seen last frame — runs the game's own vertex shader twice with **stream output** capturing `SV_Position` (this frame's constants, then last frame's), on a clone of bgfx's root signature with the stream-output flag and a stream-out variant of the draw's PSO (VS + input layout, no rasterisation). At the DLSS insertion an indirect draw rasterises the current positions with a small VS/PS pair and writes `previous − current` in pixels over the camera vectors, depth-tested (reversed-Z, greater-equal, small bias) against the scene depth so only visible object surfaces are replaced. Notes:
- bgfx creates its pipelines through `ID3D12Device2::CreatePipelineState` (the subobject stream) and hands the *same draw a different pipeline object every frame*, so the draw key deliberately excludes the PSO.
- The velocity pass uses the viewport the scene was rendered with (see dynamic resolution below).
- Overlay: "Object motion: N draws recorded, N streamed out, N without history, ..."; the MV visualiser (Debug mode 5) shows object motion as colour differing from the camera field.

### Dynamic resolution in the port (`DRS=1`)

On the native D3D12 path the port renders the 3D scene into a **variable sub-viewport** of its full-size targets (observed 2624x1474, 2208x1240, 1536x862 inside 3024x1701; the composite stretches it to the screen) — dynamic resolution scaling driven by GPU load (loading stretches, heavy cutscene shots, anything the add-on adds). Left alone this breaks any temporal technique: the picture zooms in texture space on every step. The add-on handles it: the scene viewport is detected per frame (the most common viewport among the scene's depth-tested draws — a few full-size depth-tested quads exist too), the DLSS feature is created in a scalable mode (DLAA becomes a Quality-mode feature: same model and preset, but NGX accepts render sub-rects down to ~50 %), each frame is evaluated on the sub-rect (`InRenderSubrectDimensions`), jitter/motion vectors/FG inputs use viewport units, and the full-size DLSS output is resampled (4-tap bilinear) back into the sub-rect so the game's composite and post chain stay untouched. The overlay shows the current viewport and whether the sub-rect path is active. Next quality step: sample the full-size output directly in the composite (true super-resolution from the sub-rect) — needs the composite's scale constant or a replacement blit.

### Known limitations

- Per-object motion (characters) has no motion vectors yet; expect ghosting on fast character movement.
- `Mode` changes need a restart (render targets are created at startup).
- `steam_appid.txt` (2492670) is placed next to `mgs4.exe` so the exe can be launched directly for testing; harmless for Steam launches.

## Native Direct3D 12 option (preferred)

The port has its own renderer setting: **Options -> Graphics -> API = DirectX 12**, stored as `api=dx12` in `mgs4_savedata_win\<steamid>\mgs4\mgs4.savedsettings`. With it bgfx creates the D3D12 device directly, so the ASI switch below is unnecessary (set `Enabled = 0` in `scripts\MGS4_D3D12.ini` or remove it). Two things to keep in mind on that path:

- bgfx never loads `d3d11.dll`, so an ASI loader installed under that name would not load any more. Install Ultimate ASI Loader as **`winmm.dll`** (imported by `mgs4.exe` at startup) instead — that is what this setup uses now, and `MGSFPSUnlock.asi` keeps working.
- The same settings file has `enableFXAA` (turn it off with DLAA — it only blurs the DLSS input) and `vsync` / `fpsLimiter` (vsync off is better for frame-generation latency).

## d3d12-switch (`MGS4_D3D12.asi`) — fallback

The port renders through [bgfx](https://github.com/bkaradzic/bgfx) and picks Direct3D 11. bgfx tries backends in score order (D3D11, then D3D12, …) and moves on when one fails to create a device. The ASI hooks the system `d3d11.dll`'s `D3D11CreateDevice` / `D3D11CreateDeviceAndSwapChain` and returns `E_FAIL` for calls that carry `D3D11_CREATE_DEVICE_SINGLETHREADED` (0x1 — bgfx passes 0x21), so bgfx falls through to its D3D12 backend. Nothing else is patched.

Verified 2026-08-28: game boots and renders the title/intro on D3D12 (`ReShade.log` shows `D3D12CreateDevice`, `D3D12Core.dll` loaded); MGSFPSUnlock, ReShade and `renodx-dlss5.addon64` load alongside.

### Build / install

Requirements: mingw-w64 `gcc` on PATH (Git Bash), [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) as `MGS4/d3d11.dll`.

```sh
sh d3d12-switch/build.sh      # -> build/MGS4_D3D12.asi
sh d3d12-switch/install.sh    # copies asi + ini to MGS4/scripts/ (override with MGS4_DIR=...)
```

Revert to stock D3D11: set `Enabled = 0` in `MGS4/scripts/MGS4_D3D12.ini`. Log: `MGS4/logs/MGS4_D3D12.log`.

## Layout

```
d3d12-switch/          mgs4_d3d12.c, MGS4_D3D12.ini, build.sh, install.sh
dlss-addon/            src/mgs4_dlss.cpp, build.bat, mgs4_dlss.ini (sample)
third_party/minhook/   MinHook (BSD-2), vendored
third_party/reshade/   ReShade add-on API headers (v6.8.0, BSD-3)
third_party/DLSS/      NVIDIA DLSS SDK headers + nvsdk_ngx_s.lib (DLLs git-ignored)
docs/                  reverse-engineering notes and the DLSS plan
```

### Phase 2 (first step): character mask

Skinned meshes (PSOs whose input layout has `BLENDWEIGHT`/`BLENDINDICES`, reported by ReShade at pipeline creation)
are replayed once into a private depth buffer (same PSO, same jittered constants, no colour target). The motion-vector
pass turns that depth into DLSS's **bias-current-colour mask**, so DLSS leans on the current frame for character
pixels instead of reprojected history that camera-only vectors cannot describe. Result: no halo/ghosting around
characters, at the cost of a little temporal accumulation on them. Options (panel / ini): `DynamicMask` (default on),
`DynamicMaskProps` (also mask props with their own model matrix — off; static props are correct with camera vectors),
`DynamicZeroMV` (zero motion on masked pixels — off; useful for third-person camera turns where the player stays
centred). The MV visualiser (`DebugMode=5`) shows the mask in blue. True per-object velocity remains future work.

### DLAA insertion point (pre-HUD)

In DLAA mode DLSS runs *before* post-processing and the HUD: the frame's geometry target is detected in-frame (first
depth-bound RT reaching 40% of last frame's peak draw count — the port multi-buffers these targets, so "last frame's"
never matches) and DLSS is inserted at the first draw that samples it, or at the first 2D draw into it, whichever
comes first. Draws issued after the insertion (transparents, particles, HUD) are left unjittered. Upscaling modes keep
the composite insertion (they need the SRV redirect).

### DLSS 5 NR vs. pre-HUD insertion (auto)

`renodx-dlss5` runs its NR pass on whatever DLSS evaluates. With the pre-HUD insertion that is the raw scene, and the
game's DOF, colour grading and vignette are applied afterwards, which flattens the NR effect (faces in particular).
So the insertion point is **automatic** (`PrePost=auto`): if `renodx-dlss5.addon64` is loaded in the process, DLAA runs
at the composite so NR gets the final image; without it, DLAA runs pre-post for the cleanest AA (vignette/HUD outside
DLSS). Override with the "Insertion point" combo in the panel or `PrePost=1` / `PrePost=0`.

### Direct stage boot

`mgs4.exe --stage <name>` skips the launcher and menus: `s00title_1` (OTC intro), `s00a00l` (cemetery opening),
`s01a00l` (Act 1 start), ... (names listed in the exe). `steam_appid.txt` next to the exe keeps Steam from
relaunching. A desktop shortcut "MGS4 (stage s00a00l)" boots straight into the cemetery for quick tests.

`tools\launch_stage.ps1` automates a test setup: boots a stage, waits for the game window, then taps Enter every
0.5 s until the add-on log reports the first 3D frame (auto-save notice and title are gone, the cutscene is running),
for at most 60 s. Keys go through `keybd_event` with the window forced to the foreground - this port ignores
scan-code `SendInput` events. Explicit sequences also work:
`powershell -ExecutionPolicy Bypass -File tools\launch_stage.ps1 -Stage s00a00l -Keys "5,ENTER,4,ENTER"`
(`-NoRestart` sends the keys to the running game; log in `MGS4\logs\launch_stage.log`).

### Stage rotation for testing

`tools	est_stages.ps1 -Stages "s00a00l,s02a50l_D1,s03a30l_D1" -HoldSeconds 40 [-MvVis] [-ObjectMV]` boots each stage/cutscene in turn (`tools\stages.md` lists the 75 stage ids from the executable and their `_D<n>` cutscene / `_<n>` section variants; `s02a50l_D1` is the Naomi lab scene), waits for the first 3D frame, holds, takes screenshots (and the motion-vector visualiser with `-MvVis`), and writes a per-stage log digest plus `summary.txt` (evaluations, DRS frames, last scene viewport, crashes) to `MGS4\stage_tests\<timestamp>\`.

### Robustness

Level transitions destroy and recreate render targets; every cross-frame handle (busiest/geometry/final targets, last
depth, RT->depth map, descriptor-copy map) is validated against the set of live textures and dropped on destruction.
