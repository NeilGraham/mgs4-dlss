# How the add-on works

Reverse-engineering notes on the port itself are in [renderer-notes.md](renderer-notes.md); this page is what the add-on does with them, feature by feature. Configuration keys are in [configuration.md](configuration.md).

## Status

| Step | State |
|---|---|
| Route A — run the port on bgfx's built-in Direct3D 12 backend | **Done** — the game has a native renderer option (Options -> Graphics, `api=dx12` in `mgs4_savedata_win\<steamid>\mgs4\mgs4.savedsettings`) |
| Phase 0 — map the frame (scene target, depth, composite draw) | **Done** — see docs |
| Phase 1a — NGX DLSS (DLAA) created + evaluated every frame, NGX add-ons can hook it | **Done** — `dlss-addon/` (v1: zero jitter / zero motion vectors) |
| Phase 1b — camera jitter + camera-only motion vectors | **Done** — see below |
| In-overlay controls (ReShade Add-ons tab) | **Done** |
| Frame generation (Streamline DLSS-G: 2x/3x/4x, dynamic target fps, Reflex; live switching) | **Done** — `dlss-addon/src/fg.cpp` |
| Phase 2 — per-object motion vectors (stream-out of the game's vertex shaders) | **Done, on by default** (`ObjectMV=1`) — one stream-out draw per object, ~0.1 ms GPU / ~0.2 ms CPU per frame at 4K, no frame-rate cost |
| Dynamic resolution handling (the port upscales its scene sub-rect before the composite; depth/vectors brought to the full grid) | **Done** — `DRS=1` |
| Phase 3 — real upscaling (internal res < output res) | **Done** — `Mode=Quality/Balanced/Performance/UltraPerformance` (with jitter and object vectors these are real DLSS modes) |

## The insertion

A ReShade add-on (API 20, D3D12 only) that creates a real NGX DLSS Super Resolution feature (DLAA, preset K) and evaluates it every frame:

1. Tracks what bgfx binds per command list (RT/DS, viewport, root descriptor tables, root CBVs, root signature, PSO).
2. Mirrors bgfx's `CopyDescriptors` traffic (`copy_descriptor_tables` event) into its own slot→resource map, because ReShade does not register copied CBV/SRV/UAV descriptors in its view map. That makes the SRVs of any draw resolvable.
3. At the one draw per frame into the backbuffer-sized target (the 1:1 composite of the finished 3840x2160 frame, UI included), resolves the texture that draw samples — a double-buffered final texture, *not* the render target with the most draws — pairs it with the depth buffer that was bound with it, and calls `NGX_D3D12_EVALUATE_DLSS_EXT` with color/depth/(zero) motion vectors.
4. Copies the DLSS output over the sampled texture, then natively re-applies bgfx's descriptor heaps, root signature, PSO and root parameters so the composite draw is unaffected.
5. `RecreateAfter=N` (default in the ini: 0 = off) can re-create the feature once after N evaluations. It was needed by older builds of renodx-dlss5 that only hooked NGX after our first `CreateFeature`; current builds hook NGX at Streamline/NGX init and capture the first create (ReShade.log: `feature 18 created` right after it), and the re-create costs a ~70 ms stall, one raw frame and a DLSS + NR history reset mid-scene - leave it off.

Because the NGX calls go through the standard `_nvngx.dll` exports, NGX-hooking add-ons see them: `renodx-dlss5` reports `DLSSNR ACTIVE`, creates its NR feature after ours and evaluates it every frame.

Verified 2026-08-28: NGX init OK on RTX 5090 / 616.56, `CreateFeature` OK, ~120 evaluations/s (≈80 with DLSS 5 NR active), HUD/UI intact, and `DebugMode=1` paints the displayed image magenta (proves the insertion path).

## Direct3D 12

The port has its own renderer setting: **Options -> Graphics -> API = DirectX 12**, stored as `api=dx12` in `mgs4_savedata_win\<steamid>\mgs4\mgs4.savedsettings`. With it bgfx creates the D3D12 device directly, which is all this add-on needs — it is a D3D12 add-on and does nothing on the D3D11 backend. Setting it is step 1 of the install, and the Setup tab checks it.

- The same settings file has `enableFXAA` (turn it off with DLAA — it only blurs the DLSS input) and `vsync` / `fpsLimiter` (vsync off is better for frame-generation latency).

## Camera jitter and camera-only motion vectors

Scene draws carry a row-major clip matrix (rows = clip x, y, z, w) in their vertex constants — `c[0..3]` for the main
geometry shaders, `c[1..4]` for others. It is recognizable without knowing the shader: the w-row's xyz is a unit vector
(view-space depth direction) and the z-row has no x/y (reversed-Z, `z_clip = near`). Per frame the add-on:

1. Patches every scene draw's matrix in bgfx's upload heap once per constant region: `row_x += ox·row_w`,
   `row_y += oy·row_w` with `ox = +2·jx/W`, `oy = −2·jy/H` (Halton 2,3; 8 phases at DLAA), and reports `(jx, jy)` to
   DLSS. This is the same convention as Unreal's DLSS integration. `JitterSignX/Y` in the ini flip it if needed.
2. Picks the frame's view-projection by majority vote over the `c[0]` blocks (identity-model geometry), keeps the
   previous frame's, and runs a compute pass (`src/mv_cs.hlsl`) that reprojects each depth pixel through
   `prevVP · inv(VP)` into camera-only motion vectors (pixels, pointing to the previous position).
3. Detects camera cuts (view direction or offset jumps) and raises `InReset`.

State of tuning: ~60–75% of scene draws expose a recognizable matrix; the rest (HUD/orthographic draws, one shader family
with a different constant layout) render unjittered, so overlay elements can look slightly softer with jitter on.
Character animation gets its own motion vectors from Phase 2 below. Use the overlay toggles to compare.

## Per-object motion vectors (`ObjectMV`, on by default)

Characters and props get real motion vectors without touching a single shader. For every dynamic draw (skinned
meshes — PSOs whose input layout has `BLENDWEIGHT`/`BLENDINDICES`; props with their own model matrix when
`DynamicMaskProps=1`) the add-on issues **one** extra draw with a stream-out variant of the game's pipeline (same vertex
shader and input layout, no rasterization) that writes the clip-space position of every emitted vertex into this frame's
buffer. The buffers ping-pong: the capture becomes next frame's "previous positions" for the same draw (same geometry,
same n-th occurrence in the frame). At the injection point one draw per object rasterizes the current positions and
writes `previous - current` in pixels into the motion-vector texture, depth-tested (greater-equal, reversed-Z, cull mode
and winding copied from the game pipeline) against the scene depth, so only visible object surfaces replace the
camera-only vectors. The captured positions carry the add-on's sub-pixel jitter; the velocity shader removes it.

What made the first version slow, and what this one does instead:

- v1 streamed out twice per draw (current + recorded previous constants) under a cloned root signature; a root signature
  switch invalidates every root argument, so each draw also paid a full state restore, and the CPU copied 8 KB of
  constants per draw. 60 fps -> 33 fps.
- v2 adds `ALLOW_STREAM_OUTPUT` to the game's own root signatures when they are created (`CreateRootSignature` is hooked
  and the blob re-serialized), so the stream-out pipeline binds under the game's root signature with the game's root
  arguments, IA buffers and topology untouched: per draw it is a PSO swap, `SOSetTargets`, the draw, and the swap back.
  Per-draw ranges and buffer-filled-size counters (16 B apart) make the captures independent; the velocity pass is
  plain draws with root constants and a vertex shader that collapses the vertices past the counter.
- Measured on the Act 1 garage cutscene (85-290 captured draws per frame, 4K DLAA + NR + FG): stream-out 0.02-0.15 ms
  GPU, velocity 0.02-0.04 ms GPU, 0.1-0.4 ms CPU per frame, locked 60 fps.

Notes:

- bgfx creates its pipelines through `ID3D12Device2::CreatePipelineState` (the subobject stream) and hands the *same draw
  a different pipeline object every frame*, so the draw key deliberately excludes the PSO; stream-out pipelines are
  shared by vertex-shader hash (about 20 per scene).
- The velocity pass uses the viewport the scene was rendered with (see dynamic resolution below).
- GPU timing: the add-on records timestamps around the scene (first scene draw -> after DLSS), the stream-out draws and
  the velocity pass; the 10-second stats line and the overlay show `GPU ms: scene / stream-out / velocity; CPU ms`.
- Overlay: "Object motion: N captured (N with history, ...)". The MV visualizer (`DebugMode=5`, live) shows object
  motion as color differing from the camera field — it can be flipped on for a few seconds during a recording.
- With real object vectors the character mask (`DynamicMask`) is no longer needed and is off by default.

v1.2.0 changed how the captures are paired and validated. Instances sharing one mesh (every PMC soldier) are paired across frames by the head of their vertex constants (the per-instance transform / first bones) instead of by draw order, so a change in sort order no longer hands a soldier another soldier's previous positions. And the velocity pixel shader discards a fragment whose vector exceeds `ObjectMVMaxPixels` (200) or whose vector field changes by more than `ObjectMVMaxGradient` (4 px per screen pixel across the surface): a wrong pairing - another instance metres away, or the same mesh captured in another projection by one of the game's multi-pass character draws - fails one of the two and the pixel keeps its camera vector. Both showed as a one-frame flash of saturated vectors on a character body that frame generation turned into a visible pulsing. `ObjectMVProps=1` extends the capture to rigid props with their own model matrix.

## Character mask (`DynamicMask`, superseded)

Skinned meshes (PSOs whose input layout has `BLENDWEIGHT`/`BLENDINDICES`, reported by ReShade at pipeline creation)
are replayed once into a private depth buffer (same PSO, same jittered constants, no color target). The motion-vector
pass turns that depth into DLSS's **bias-current-color mask**, so DLSS leans on the current frame for character
pixels instead of reprojected history that camera-only vectors cannot describe. Result: no halo/ghosting around
characters, at the cost of a little temporal accumulation on them. Options (panel / ini): `DynamicMask` (default off),
`DynamicMaskProps` (also mask props with their own model matrix — off; static props are correct with camera vectors),
`DynamicZeroMV` (zero motion on masked pixels — off; useful for third-person camera turns where the player stays
centered). The MV visualizer (`DebugMode=5`) shows the mask in blue. Superseded by the per-object motion vectors above; kept as an option (`DynamicMask=1`).

## Dynamic resolution in the port (`DRS`, on by default)

On the native D3D12 path the port renders the 3D scene into a **variable sub-viewport** of its full-size targets
(observed anywhere from 100 % down to 50 %: 3840x2160 -> 3712x2088 -> ... -> 1920x1080, changing every second or so
when the GPU is loaded — e.g. with frame generation at 4K120). **Its post chain upscales that sub-rect to the full-size
final image before the composite; the composite samples the whole texture.** So at the add-on's insertion point the
color is always full-size, while the scene depth (and anything derived from it) is on the sub-rect grid. Verified
with the vector overlay (`DebugMode=9`): with the earlier assumption that the composite stretches the sub-rect, the
overlay covered only the top-left (k x k) part of the screen.

`DRS=1` therefore puts depth and vectors on the full grid: the scene depth is stretched (nearest) into a full-size R32
copy for DLSS and frame generation, the camera vectors are computed per full-grid pixel from the sub-res depth, the
object vectors are rasterized with the full viewport (their clip positions are viewport-independent) and depth-tested
manually against the stretched depth, the jitter is expressed in full-grid pixels, and DLSS / NR / DLSS-G all see a
full-size contract. Getting this wrong showed up as: a DLSS-G ghost of moving characters displaced ~(1-k) toward the
top-left, asymmetric ghosting in DLSS itself, and (in the old sub-rect mode) DLSS 5 NR covering only the top-left
rectangle while the game was scaled.

`DRS=2` keeps the legacy behavior (DLSS evaluated on the sub-rect, output resampled back into it) for reference; it is
wrong for this port's composite and breaks NR's coverage.

The sub-rect is detected per frame from the viewport most depth-tested draws into the frame's geometry target use
(at least half the target); the 20-frame hysteresis copy is only a fallback before the first scene draw of a frame.

## DLAA insertion point (pre-HUD)

In DLAA mode DLSS runs *before* post-processing and the HUD: the frame's geometry target is detected in-frame (first
depth-bound RT reaching 40% of last frame's peak draw count — the port multi-buffers these targets, so "last frame's"
never matches) and DLSS is inserted at the first draw that samples it, or at the first 2D draw into it, whichever
comes first. Draws issued after the insertion (transparents, particles, HUD) are left unjittered. Upscaling modes keep
the composite insertion (they need the SRV redirect).

## DLSS 5 NR vs. pre-HUD insertion (auto)

`renodx-dlss5` runs its NR pass on whatever DLSS evaluates. With the pre-HUD insertion that is the raw scene, and the
game's DOF, color grading and vignette are applied afterwards, which flattens the NR effect (faces in particular).
So the insertion point is **automatic** (`PrePost=auto`): if `renodx-dlss5.addon64` is loaded in the process, DLAA runs
at the composite so NR gets the final image; without it, DLAA runs pre-post for the cleanest AA (vignette/HUD outside
DLSS). Override with the "Insertion point" combo in the panel or `PrePost=1` / `PrePost=0`.

## HUD: DLSS before the HUD, on the final texture

The port draws its HUD into the final texture **before** the composite. In the DLSS 5 NR configuration (DLAA on the
final image) that used to put the HUD inside the image DLSS reprojected - HUD ghosting opposite to camera turns - and
the HUD-less color for frame generation had to be patched together from a pre-HUD capture (raw, no DLAA/NR under
the HUD elements: visible rectangles and blur/flicker around the HUD in generated frames).

Since v1.0.1 the add-on runs DLSS at the **first HUD draw of the frame, on the final texture**: the scene has been
upscaled/tonemapped into it, the HUD has not been drawn yet. DLSS and the inline NR add-on never see the HUD, the
game then draws the HUD on top of the DLSS output, and that output *is* the HUD-less color for DLSS-G (no patching);
the replayed UI layer is tagged valid-until-present. Frames without a HUD (cutscenes, menus) fall back to the
composite insertion as before. `DebugMode=7` drops the HUD draws in this mode (true HUD-less view); `DebugMode=6`
shows the UI layer of the previous frame (it is replayed after the insertion); the magenta test (`DebugMode=1`) is
skipped at this insertion point.

HUD draws are told apart from the post-process passes by shape and place: HUD elements are 6-vertex quads (and
2-vertex lines) drawn with the full viewport, into the final texture that received this frame's scene write (the
3/4-vertex fullscreen pass at the full viewport sampling a scene-sized input - the game's upscale/tonemap), after
it. Not HUD: anything drawn with the scene's (dynamic-resolution) viewport, 3/4-vertex fullscreen passes, quads
whose primary input is scene-sized, and anything drawn into the *other* final texture - the final image is
double-buffered and during camera motion the port draws a full-viewport quad that samples the scene into the other
one (a motion feedback effect). "Scene-sized" means at least half the frame with the frame's aspect (HUD atlases are
2048x4096). (Earlier heuristics mis-filed a third of the HUD draws - stale scene-sized descriptors in unused slots -
and, once, that feedback quad: DLSS then ran on the wrong texture for the frame, the real one was presented raw,
and the quad's scene copy faded into the UI layer whenever the camera moved.)

`UIMask=1` (live) is the fallback for the composite insertion: where the replayed UI layer holds bright HUD detail,
DLSS's bias-current-color mask is set and the motion vector zeroed. It has no effect while the pre-HUD insertion
is active (the HUD is not in DLSS's input there).

## 3D windows: the Codec caller (`WindowScene`, on by default)

A Codec call renders no world at all - the previous frame simply stays in the final texture - and the caller's scene
is rendered **with depth into its own render target** at a window viewport (1866x1032 at 987,564 on this setup),
then post-processed (that is where the CRT/scanline look is applied) and blitted into the codec frame; ~400 panel
quads follow. The pause menu is different: the live world still renders at the full viewport, with the Snake model in
a 960x552 window.

With `WindowScene=1` the add-on runs DLSS on that window target **at its first reader**, i.e. after the caller's scene
is finished and before the game's own post-process. The consequences are exactly what a Codec call needs:

- the caller's face and room go through DLAA and, with `renodx-dlss5` loaded, DLSS 5 Neural Rendering;
- the CRT overlay is applied by the game *to the DLSS output*, so it is never part of DLSS's input, and neither are
  the frame, the text or any panel - they are drawn afterwards and classified HUD;
- the DLSS result is copied back only into the window rectangle, and the camera vectors are computed relative to that
  rectangle and forced to zero outside it, so nothing can bleed out of the window into the frozen background.

Verified in the Act 2 Codec cutscene `s02a10l_D2` (Campbell/Rosemary): the magenta path test (`DebugMode=1`) marks
exactly (1680,296) 1864x1024 - the caller's box on screen - 99 % filled with none outside; the motion-vector view
(`DebugMode=5`) shows the field only inside that box with the character silhouettes on it, in every pan direction;
DLSS and NR evaluate on every frame of the call. In gameplay the insertion never fires (a full-frame 3D viewport is
present), and the pause menu keeps the normal path for the same reason.

## Frozen screens: the pause menu and Codec backgrounds (`FrozenBackground`, on by default)

Behind the pause menu and the Codec the game shows a **still image of the world**, and until v1.1 that image was
the raw (non-DLSS, non-NR) frame, which broke the illusion the moment you paused. The mechanism, from the freeze
trace (`TraceFreeze=1`): on the last live frame the game draws its upscaled scene into the final texture, then a
6-vertex draw **downsamples that final texture into a 1920x1080 seed texture**, and only then come the HUD draws -
so the capture ran *before* the pre-HUD DLSS insertion. From the next frame on the world is no longer rendered; every
frame of the pause menu and of the Codec begins with a full-screen blit of that seed into the final texture, then
the panels (and the 3D window with the model / the caller, which `WindowScene` handles). Nothing ever rewrites the
seed, so whatever it captured is the background for the whole time the screen stays frozen.

With `FrozenBackground=1` the add-on recognizes that capture (a few-vertex draw into a smaller, non-final target
that samples this frame's final scene texture, after the scene write and before any HUD draw) and runs the normal
pre-HUD insertion on the final texture right before it. The seed is then the DLSS (+NR) image and the frozen
background matches the live picture. No game texture is written out of band - the game's own capture does the copy
(the earlier attempt to copy a kept frame into the final texture crashed the device with DLSS-G active).

Verified with the vector view: leave `DebugMode=5` on and pause - the frozen background is now the vector-view image
(before, it stayed the plain scene); same for the Codec list and a Codec call. The insertion fires once per freeze
(`frozen-background insertions` in the stats line).

The frozen frames themselves are **passed through**: a frame with no 3D scene whose final texture holds a recycled
image (the seed blit, a copy of the other final texture, or no rewrite at all) is not evaluated by DLSS at all - the
image already went through DLSS and NR on the live frame it came from, and evaluating it again applied NR a second
time. That second pass was visible as a sharpness jump right after pausing (the ~0.3 s until the pause menu's 3D
model window appears, after which the insertion moves to that window), for the whole Codec frequency list, and
behind blocking dialogs such as the controller connect / disconnect notice. Measured on a 120 fps capture: the
background band went from 4.4 to 5.4 (+40 %) in that window before, and stays flat now. Frames with fresh 2D content
(prerecorded videos, menus rendered from atlases) are still evaluated as before. Counters: `frozen pass-through frames`
in the stats line and the overlay.

Finally, the seed is only a 1920x1080 downsample, so even with DLSS in it the frozen background was a touch softer
than the live frame. The add-on keeps a full-size copy of the DLSS output at the capture and rewrites the SRV of the
game's seed blit (the full-viewport draw that samples the seed) to point at that copy - the same in-place descriptor
rewrite the upscaling modes use for the composite - so the frozen background is the live frame, pixel for pixel. The
kept copy is invalidated as soon as the game writes into the seed again without the add-on's insertion.

## Resuming from a frozen screen keeps the history

Leaving the pause menu (or a dismissed dialog) used to reset DLSS and NR: a visible drop in detail that re-converged
over about a second (measured: -15 % background sharpness at the unpause). The world has not moved during the freeze,
so the history from the last live frame is still valid. The add-on remembers the camera of the last live evaluation
and, when the first evaluation after a frozen screen sees the same camera (the cut heuristic's thresholds), it keeps
the history: no reset, motion computed relative to that last live frame rather than to the pause menu's model camera,
and only the rectangle the 3D window (the Snake model) occupied meanwhile is excluded for that one frame (its history
belongs to the model, not the world). Log: `RESUME f…: same camera as before the freeze -> DLSS history kept`. A
different camera still resets as before. Two details that mattered: the frozen state is only cleared by a color scene
write from a geometry target (the pause menu's closing frame samples the *depth* texture full-screen into the final
texture, which looked like fresh content and reset the history one frame early), and the pause menu's own hundreds of
depth-tested panel quads never count as a live scene.

## Frame generation (DLSS-G / Multi-Frame Generation via Streamline)

The add-on drives **NVIDIA Streamline** (`sl.interposer.dll`, `sl.dlss_g.dll`, `sl.reflex.dll`, `sl.pcl.dll`, `sl.common.dll` + `nvngx_dlssg.dll`, which must sit next to `mgs4.exe`) in *manual hooking* mode from inside the ReShade add-on:

1. `slInit` runs in the `init_device` event, i.e. after the D3D12 device exists but before bgfx creates its swapchain. The add-on then hooks `IDXGIFactory::CreateSwapChain`/`CreateSwapChainForHwnd` in front of ReShade and creates the game's swapchain through Streamline's proxy factory. The result is `game -> Streamline proxy swapchain -> ReShade -> DXGI`: Streamline intercepts `Present` and inserts the generated frames, ReShade (and this add-on's `present` event) run for every presented frame, so the add-on counts a game frame only when scene draws happened.
2. Every game frame it feeds Streamline what DLSS-G needs: a frame token, Reflex sleep + PCL markers (simulation, render-submit, present), the camera constants derived from the same clip matrix the DLAA path uses (projection, clip<->prev clip, camera position/axes, reversed-Z near, jitter, cut/reset flag), and tags for **depth**, the add-on's **motion vectors** (camera-only, pixels) and — in pre-post insertion — the anti-aliased **HUD-less color**. When the game image is letter/pillar-boxed, the backbuffer tag carries the game-image rectangle so only that region is interpolated.
3. `slDLSSGSetOptions` is applied live from the overlay / ini: **Off, 2x, 3x, 4x** or **Dynamic** with a **target frame rate** (`DLSSGMode::eDynamic` + `dynamicTargetFrameRate`, 0 = monitor refresh). If the driver does not report dynamic multi-frame generation, the add-on falls back to its own controller: it measures the game frame rate every second and picks the 2x/3x/4x multiplier that lands closest to the target. Reflex (Off / On / On + Boost) is set through `slReflexSetOptions`.

Ini keys: `FrameGen` (0 off, 1 = 2x, 2 = 3x, 3 = 4x, 4 = dynamic), `FGTargetFps`, `Reflex` (0/1/2). Status (Streamline/DLSS-G version, DLSS-G status flags, max multiplier, dynamic-MFG and vsync support, VRAM, presented/generated frames) is shown in the overlay. Streamline's own log goes to `logs\sl.log`.

Notes:
- Streamline is only loaded when `FrameGen` is non-zero **at startup** (it has to wrap the swapchain when the game creates it), so the first switch from Off needs a restart; after that Off/2x/3x/4x/Dynamic and the target frame rate change live. With `FrameGen=0` the add-on behaves exactly as without frame generation.
- Verified 2026-08-28 (RTX 5090, driver 616.56, Streamline 2.12.129 runtime via the NVIDIA app override, DLSS-G 310.8): ReShade keeps its overlay and add-ons (the DLSS-G present queue is created through ReShade's device proxy on purpose), DLAA + DLSS 5 NR keep working, `DLSS-G interpolation state changed ... enabled`, dynamic mode reported as supported and accepted (`eDynamic` disables vsync/RSync by itself).
- The game runs with vsync on (sync interval 1); DLSS-G works with it (driver reports vsync support) but latency is lower with the game's vsync off. The game's own limiter (`fpsLimiter=60`) caps the *game* frame rate — generated frames come on top of that, which is the whole point: the simulation stays at the 60 fps its physics are tied to.
- **HUD on generated frames.** In pre-post insertion DLSS-G gets the anti-aliased image before the HUD as HUD-less color. In composite insertion (DLSS 5 NR loaded) the HUD is inside the image, so the add-on (1) replays the game's HUD draws (depth-off draws into the final texture that sample no scene-sized input) into its own RGBA layer, tagged as `UIColorAndAlpha`, and (2) captures the final texture right before its first HUD draw and builds a true HUD-less color: the DLAA output with the pre-HUD capture under the UI layer's pixels. DLSS-G uses the UI layer when its UI recomposition is available; when it is not (the NVIDIA app's frame-generation preset override disables it — `Disabling bUIRecompositionSupported due to preset override` in `logs\sl.log`) it derives the HUD from backbuffer minus HUD-less color, which is why the HUD-less image must really lack the HUD. Debug modes "Visualize UI layer" and "Visualize HUD-less color" show both inputs; verified in Act 1: the HUD-less view has no HUD elements while the frame does. Full-screen menus (Mk.II menu, credits) are rendered through scene-sized layers and are not treated as HUD (they are static anyway).
- Known: on the two frames where the DLSS SR feature is (re)created (`RecreateAfter`), no inputs are tagged, so Streamline logs "Unable to find common constants" once and DLSS-G skips interpolation for that frame.
- Alternative without this add-on: NVIDIA Smooth Motion (driver-level 2x, NVIDIA App -> Graphics -> `mgs4.exe` -> Smooth Motion).

## Frame generation on frozen screens and at Codec transitions

DLSS-G interpolates between consecutive game frames; on a frozen screen that gains nothing, and across the Codec's
transitions (the panel collapse when a call starts, the caller window appearing) it produced single torn frames -
displaced copies of the panel lines and of the caller window flashing across the center. Note that an NVIDIA-app
frame-generation preset override disables DLSS-G's UI recomposition ("Preset A selected, disabling UIR" in sl.log),
so the UI-layer / HUD-less tags cannot protect moving UI in that configuration. The add-on therefore reports a cut
(`reset`) to DLSS-G on every frozen pass-through frame and for the first 8 evaluations after a transition (a
pass-through frame ending, the insertion moving between the final texture and a 3D window), and also forces a DLSS
history reset on the first evaluation after such a transition (the caller's first frame used to come out warped from
seconds-old history). In window mode it now also tags a HUD-less image (the pre-HUD capture, valid until present) and
the UI layer for DLSS-G, which helps where UI recomposition is available.

## Depth of field after NR (`PostDof=1`)

The port applies its depth of field inside the scene target (three passes: a half-resolution circle-of-confusion pass
that samples the linear depth copy, a golden-angle spiral bokeh gather, and an alpha blend of the blurred layer over the
sharp image) before the tonemap / upscale into the final texture. DLSS and the DLSS 5 NR add-on therefore only ever see
the defocused image: an out-of-focus character carries no NR detail and the NR look "pops in" on every rack focus.
With `PostDof=1` (live key, panel checkbox) the three draws are skipped - identified by the FNV-1a hash of their pixel
shader bytecode (`733f4efc`, `92bbc108`, `bca9c941`) - and an exact HLSL transcription of the three passes runs on the
DLSS output instead, so the blur is applied to the NR-processed image. The circle of confusion (`dof_coc_cs`) is
evaluated **at the game's CoC draw**, from the depth copy that draw was about to sample and with that draw's constants
(`cb0[8..17]`), into a half-resolution R16F texture on the full grid; at the DLSS insertion the half-res color of the
DLSS output is packed with it (`dof_pack_cs`), the spiral gather (`dof_gather_cs`) and the blend (`dof_composite_cs`)
run, and the result is copied back before the HUD. Evaluating the CoC at the draw means the depth copy is exactly what
the game's pass would have read - whatever the game does to that texture later in the frame cannot reach the blur -
and if any input is missing at that draw (constants unreadable, depth copy unresolved) the game's own DoF runs for
that frame, consistently for all three passes (`input fallbacks` in the stats line).
Three things the game does around its DoF are handled explicitly: (1) dynamic-resolution sub-rect frames (scene
starts, heavy load) are handled at the exact per-frame scale (see below); a frame where the scale *steps* keeps the
game's DoF for that one frame (measured on the cemetery-entry ramp: the fallback flashes at +0.30 relative sharpness
on step frames vs +0.75 when the step frame is handled at the new scale - parts of the chain can lag the step), as
does a frame where the scene's viewport genuinely disagrees with the post chain's scale (a safety net); (2) overlays the game draws
*after* its DoF combine and before the upscale into the final texture (title cards such as "Three Days Earlier",
captions: 5-8-vertex quads into the graded scene texture whose input is not the scene) are replayed into a mask layer,
and the composite keeps those pixels sharp - otherwise they would be blurred with the surface behind them; (3) the
2048x2048 capture of the final texture the cutscene WIPE transitions slide over the next shot at every cut is taken
*before* the pre-HUD insertion - with PostDof the final texture has no blur yet at that point, so every cut flashed a
sharp, DoF-less copy of the previous shot across the screen for the wipe's ~4 frames (found by scanning 4K60 display
captures for high-frequency-energy jumps: every flash lined up with a camera-cut history reset in the log). The
capture draw's SRV is redirected in place to the previous frame's DLSS+DoF output - same size, one frame stale, and
the wipe shows the previous shot anyway. The stats line reports `PostDof: frames re-applied N,
draws skipped M, skipped without re-apply K, sub-rect frames left to the game S, overlay draws masked O, wipe captures
redirected W`; K should stay 0.
Debug views: `DebugMode=10` the blurred layer, `11` its coverage, `12` the overlay mask.
Dynamic resolution: the CoC pass's viewport is exactly half the scene sub-rect, so the scale is known per frame
without the add-on's 20-frame viewport hysteresis; the depth sample, the spiral step and the overlay mask are scaled by
it and PostDof stays on through sub-rect frames (`DofSubRect=0` leaves them to the game's DoF instead). The CoC depth
sample is read at the frame's camera-jitter offset (`DofJitterSign`): the depth copy is jittered, the DLSS output is not,
and without that every blur boundary wobbled by a sub-pixel per frame (the offset is in depth-copy texels, i.e. scaled
by k on sub-rect frames).

v1.2.0 fixed the blur layer flickering into the top-left sub-rect. The DoF passes' constant buffer (400 bytes) was
written into 256-byte ring slots of a 1 KB buffer: frame N+1's write overlapped the tail of frame N's constants - the
half/full sizes, **the depth scale**, the spiral step, the depth de-jitter and the overlay-mask flags - while the GPU
could still be running frame N's passes (with frame generation the CPU records one or two frames ahead), and the
fourth slot ran past the end of the buffer. The single set of descriptors was rewritten every frame for the same reason.
Every per-frame GPU input of the DoF passes is now ring-buffered over 8 frames (constants at a 512-byte stride,
16 descriptors per frame), the CoC is evaluated at the game's CoC draw as described above, and the stats line reports
the depth copy's identity and addressing whenever they change (`PostDof: fN depth copy ...`).

## Pre-warm (`PreWarm=1`)

The DLSS feature is normally created at the first 3D frame, and the DLSS 5 NR add-on creates its own feature inside
that call and initializes its model on the first evaluations - about half a second of stalls and dropped resolution
right at the start of the first cutscene. With `PreWarm=1` the add-on creates the feature at the swapchain size (DLAA)
and runs 12 evaluations on its own scratch textures during frames without a 3D scene (the title / loading screens,
after 30 such frames), restoring the game's state after each; the NGX-hooking add-on's one re-create happens there too.
Log lines `pre-warm: ...` show the timings; the stats line counts `pre-warm evaluations`.
`DumpShaders=1` writes every pipeline's bytecode to `logs\shaders` (with `TraceFreeze=1` + `DebugMode=3` the freeze
trace lists each full-frame draw with its `ps=` hash) - that is how the three passes were found.

## Robustness

Level transitions destroy and recreate render targets; every cross-frame handle (busiest/geometry/final targets, last
depth, RT->depth map, descriptor-copy map) is validated against the set of live textures and dropped on destruction.

## Known limitations

- Alpha-tested surfaces (hair cards) get object vectors over their transparent texels too (the velocity pass has no alpha test); not visible in practice.
- `Mode` changes need a restart (render targets are created at startup).
- GPU load: DLAA + NR + frame generation at 4K120 can push the port's own dynamic resolution down to 50 %; the add-on renders correctly at any scale, but sharpness follows the game's choice — `Mode=Quality`, a fixed `FrameGen=1` or a lighter streaming encode keep it at native.
- Frame generation on a 60 Hz output only adds real/generated alternation; use it with a 120 Hz (or faster) display or virtual display.
- `steam_appid.txt` (2492670) is placed next to `mgs4.exe` — written by the app before any scene boot — so the exe can be launched directly for testing; harmless for Steam launches.
