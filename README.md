# mgs4-dlss

**v1.1.1 (2026-08-30)** — DLAA/DLSS with camera jitter, camera + per-object motion vectors, DLSS 5 Neural Rendering compatibility, DLSS-G frame generation (2x/3x/4x/dynamic), correct handling of the port's dynamic resolution, DLSS inserted before the HUD (no HUD ghosting, clean HUD-less/UI layers for frame generation), and the pause-menu / Codec backgrounds kept as the DLSS (+NR) image. Download the add-on and the ini from the [releases](https://github.com/NeilGraham/mgs4-dlss/releases); install steps below.

Real DLSS (DLAA and the upscaling modes) for the PC port of *Metal Gear Solid 4* (Master Collection Vol. 2), built as a ReShade add-on. The NGX feature it creates can be hooked by NGX-based add-ons — **directly compatible with the DLSS 5 Neural Rendering add-on (`renodx-dlss5.addon64`)**, which is auto-detected: with it loaded, DLAA runs on the final image so NR works at full strength.

## Status

| Step | State |
|---|---|
| Route A — run the port on bgfx's built-in Direct3D 12 backend | **Done** — the game has a native renderer option (Options -> Graphics, `api=dx12` in `mgs4_savedata_win\<steamid>\mgs4\mgs4.savedsettings`); `d3d12-switch/` remains as a fallback |
| Phase 0 — map the frame (scene target, depth, composite draw) | **Done** — see docs |
| Phase 1a — NGX DLSS (DLAA) created + evaluated every frame, NGX add-ons can hook it | **Done** — `dlss-addon/` (v1: zero jitter / zero motion vectors) |
| Phase 1b — camera jitter + camera-only motion vectors | **Done** — see below |
| In-overlay controls (ReShade Add-ons tab) | **Done** |
| Frame generation (Streamline DLSS-G: 2x/3x/4x, dynamic target fps, Reflex; live switching) | **Done** — `dlss-addon/src/fg.cpp` |
| Phase 2 — per-object motion vectors (stream-out of the game's vertex shaders) | **Done, on by default** (`ObjectMV=1`) — one stream-out draw per object, ~0.1 ms GPU / ~0.2 ms CPU per frame at 4K, no frame-rate cost |
| Dynamic resolution handling (the port upscales its scene sub-rect before the composite; depth/vectors brought to the full grid) | **Done** — `DRS=1` |
| Phase 3 — real upscaling (internal res < output res) | **Done** — `Mode=Quality/Balanced/Performance/UltraPerformance` (with jitter and object vectors these are real DLSS modes) |

See [docs/renderer-notes.md](docs/renderer-notes.md) for what we know about the port and the full plan.

## dlss-addon (`mgs4_dlss.addon64`)

A ReShade add-on (API 20, D3D12 only) that creates a real NGX DLSS Super Resolution feature (DLAA, preset K) and evaluates it every frame:

1. Tracks what bgfx binds per command list (RT/DS, viewport, root descriptor tables, root CBVs, root signature, PSO).
2. Mirrors bgfx's `CopyDescriptors` traffic (`copy_descriptor_tables` event) into its own slot→resource map, because ReShade does not register copied CBV/SRV/UAV descriptors in its view map. That makes the SRVs of any draw resolvable.
3. At the one draw per frame into the backbuffer-sized target (the 1:1 composite of the finished 3840x2160 frame, UI included), resolves the texture that draw samples — a double-buffered final texture, *not* the render target with the most draws — pairs it with the depth buffer that was bound with it, and calls `NGX_D3D12_EVALUATE_DLSS_EXT` with color/depth/(zero) motion vectors.
4. Copies the DLSS output over the sampled texture, then natively re-applies bgfx's descriptor heaps, root signature, PSO and root parameters so the composite draw is unaffected.
5. `RecreateAfter=N` (default in the ini: 0 = off) can re-create the feature once after N evaluations. It was needed by older builds of renodx-dlss5 that only hooked NGX after our first `CreateFeature`; current builds hook NGX at Streamline/NGX init and capture the first create (ReShade.log: `feature 18 created` right after it), and the re-create costs a ~70 ms stall, one raw frame and a DLSS + NR history reset mid-scene - leave it off.

Because the NGX calls go through the standard `_nvngx.dll` exports, NGX-hooking add-ons see them: `renodx-dlss5` reports `DLSSNR ACTIVE`, creates its NR feature after ours and evaluates it every frame.

Verified 2026-08-28: NGX init OK on RTX 5090 / 616.56, `CreateFeature` OK, ~120 evaluations/s (≈80 with DLSS 5 NR active), HUD/UI intact, and `DebugMode=1` paints the displayed image magenta (proves the insertion path).

### Install (release)

1. In the game: **Options -> Graphics -> API = DirectX 12** (`api=dx12` in `mgs4_savedata_win\<steamid>\mgs4\mgs4.savedsettings`), FXAA off, frame limiter 60 (`fpsLimiter=60`), vsync off.
2. ReShade 6.8 **with add-on support** installed for `MGS4\mgs4.exe` (it becomes `MGS4\dxgi.dll`).
3. Copy `mgs4_dlss.addon64` and `mgs4_dlss.ini` from the release into `MGS4\` (next to `mgs4.exe`). `nvngx_dlss.dll` comes from the NVIDIA app's DLSS override or the [DLSS SDK](https://github.com/NVIDIA/DLSS) (`lib/Windows_x86_64/rel/`).
4. Optional, DLSS 5 Neural Rendering: put `renodx-dlss5.addon64` next to the add-on; it is auto-detected and the add-on then runs DLAA on the final image so NR works at full strength.
5. Optional, frame generation (`FrameGen` other than 0): the Streamline runtime next to `mgs4.exe` — `sl.interposer.dll`, `sl.common.dll`, `sl.dlss_g.dll`, `sl.reflex.dll`, `sl.pcl.dll` and `nvngx_dlssg.dll` from the [Streamline SDK](https://github.com/NVIDIA-RTX/Streamline) (`bin/x64`, 2.12+). Frame generation only helps when the display (or the virtual display you stream from) refreshes faster than the game's 60 fps — set `FGTargetFps` to your refresh rate (the shipped ini uses `FrameGen=4` + `FGTargetFps=240`); on a 60 Hz output set `FrameGen=0`.
6. Start the game; the first run writes the detected `InternalRes` to the ini. `MGS4\logs\mgs4_dlss.log` records the DLSS create/evaluate calls, NR hooking, frame generation and dynamic-resolution state; the ReShade overlay's Add-ons tab has live controls and GPU/CPU timing.

Run **`mgs4-dlss.bat`** at any point: its Install tab says which of those pieces are actually in place, and its Play tab starts the game or any single scene — see below.

## The app (`mgs4-dlss.bat`)

One window for the whole add-on, and the same things as a command line. It needs nothing installed — PowerShell
ships with Windows and the `.bat` handles the execution policy — so it runs straight out of an unzipped release.

| tab | what it is for |
| --- | --- |
| **Play** | start the game, or any one of the 423 launchable scenes, with the automation the tests use |
| **Settings** | `MGS4\mgs4_dlss.ini` as a form |
| **Install** | which files are in place, what the settings say, what the add-on did on its last run |

```bat
mgs4-dlss.bat                         :: the window
mgs4-dlss.bat s02a50l_D1              :: boot that scene and exit
mgs4-dlss.bat --main                  :: MGS4's own menu, past the Master Collection screen
mgs4-dlss.bat --list naomi            :: what can be launched
mgs4-dlss.bat --install               :: the window, opened on the install check
mgs4-dlss.bat --report                :: the install check as text, for pasting into an issue
mgs4-dlss.bat --shortcuts             :: rebuild "Desktop\MGS4 Shortcuts" against this checkout
mgs4-dlss.bat --set FrameGen=0        :: write ini keys without opening anything
mgs4-dlss.bat --help                  :: every option
```

**`launcher.bat`** and **`check-install.bat`** are aliases that open it on the Play and Install tabs; both spellings,
and `check-install.bat --report` / `-GameDir "D:\..."`, keep working. The window opens even when no MGS4 install can
be found — it starts on Install and says which folder it looked in, which is the one case where a tool that needs the
game folder still has to be useful.

### Play

The list is `tools\scenes.csv` (102 cutscenes, 250 gameplay sections, 68 stage entries) with the names from
`tools\labels.json`, filtered by a search box, plus three entries for starting the game itself. Pick one, tick what
should happen while it runs, press Launch. The panel shows the command line that does the same thing, so anything set
up in the window can be pasted into a terminal or put in a shortcut.

What can be ticked (all of it also works from the command line):

- **Skip the boot prompts** (`--advance`, on by default) — taps a button until the add-on log reports the first 3D
  frame, which is what gets a `--stage` boot past the auto-save notice, the "press any button" screen and the load.
- **Keep pressing X** (`--mash-x`) — a virtual DualShock 4 taps **Cross** about six times a second for the whole
  scene. This is what makes MGS4's in-cutscene **flashback** prompts fire; a keyboard Enter gets past the boot
  prompts but does not trigger them. It needs the [ViGEmBus](https://github.com/nefarius/ViGEmBus) driver plus
  `ViGEmClient.dll` in `tools\` (both Nefarius, BSD-3; the DLL also ships inside the `vgamepad` PyPI package, or set
  `VIGEM_CLIENT_DLL`) — the same pair `tools\ds4.py` uses, and the Install tab reports whether both are there.
  Without them the app says so and falls back to Enter.
- **Close the game when gameplay starts** (`--end-on-gameplay`) — for cutscenes. The add-on's `SCENE-STATE` /
  `SCENE-STATE-TICK` lines say whether the frame is a cutscene, gameplay or no 3D at all; the HUD coming up (40+ HUD
  draws in one heartbeat, against the 4-5 a cutscene draws) or a sustained `gameplay` state ends the run, and a
  sustained `no-3d` catches a scene that ended on a loading / continue screen. `--min-seconds` (30) keeps the HUD
  flicker at the start of some cutscenes from ending them immediately.
- **Close it after a fixed time** (`--hold N`), **render resolution** (`--res 3840x2160`, the port's
  `--res_width` / `--res_height`).

Escape, held anywhere, stops an attached run. The app writes `MGS4\logs\launcher.log`.

**Desktop shortcuts** writes `Desktop\MGS4 Shortcuts\` — one `.lnk` per scene under `cutscene`, `gameplay`,
`stage entry` and `notable`, named `<stage id> - <act> - <scene name>` so each folder sorts in story order, plus the
three game-start shortcuts at the top level. They run `tools\mgs4_dlss.ps1` **by absolute path**, which is the one
thing that can break them: move or re-clone the checkout and every shortcut points at a folder that is no longer
there. Re-running `mgs4-dlss.bat --shortcuts` fixes them all.

The port's own command line, for reference (read out of `mgs4.exe`): `--stage <id>`, `--skip-to-main-menu`,
`--res_width` / `--res_height`, `--windowing`, `--screen_index`, `--lang`, `--region`, `--input_device`, `--rumble`,
`--next`, `--forcedlcon`. **`--skip-to-main-menu` is what a bare `mgs4.exe` used to do** — without it the port stops
on the Master Collection screen first, so a plain "run the game" shortcut needs that argument. `--windowing` takes
`windowed` / `full_borderless` but the port has been observed ignoring it.

### Settings

`MGS4\mgs4_dlss.ini` as a form — DLSS mode and preset, frame generation and its target fps, the image keys
(`PostDof`, `ObjectMV`, `DRS`, `UIMask`, ...) and the diagnostics, each row naming its key and what it does.
`mgs4-dlss.bat --settings` prints the same thing; `--set Key=Value` writes without opening a window.

Saving is blocked while the game is running, because the add-on owns that file then: its writes go through the
Windows profile API, whose cache will quietly undo an outside edit. While the game *is* up, the same keys are live in
ReShade's overlay, Add-ons tab.

### Install

Most of the files this add-on needs cannot be shipped here: NVIDIA's DLSS runtimes, the Streamline runtime and
ReShade all have to be fetched from their own projects, so an install is assembled by hand and it is easy to end up
one file short. The Install tab lists every required and optional file, what version it is, and a link to where each
missing one comes from. **Re-check** (or F5) re-runs everything with the window open, so it can be left up on a
second monitor while files are dropped into the game folder, and **Copy report** puts the text form on the clipboard.

What it reports, beyond whether a file exists:

- **Files**, grouped by the feature each one unlocks (required / DLSS 5 NR / frame generation / extras), with the
  version found next to the version this was verified against.
- **Settings** read out of the game's own files — `api=dx12`, FXAA, the frame limiter, `Enabled`, `FrameGen` against
  the display's actual refresh rate, whether ReShade has the add-on disabled, whether a diagnostic key was left on,
  and whether the virtual controller the Play tab wants is there. A file being present is not the same as it being
  switched on.
- **Last run**, parsed from `logs\mgs4_dlss.log` and `ReShade.log`: whether NGX initialised, whether DLSS came from
  the local DLL or the driver override, whether `renodx-dlss5` really loaded, the insertion point, the Streamline
  and driver versions, and how many frames DLSS evaluated. This is the part a file list cannot tell you — a
  ReShade build **without** add-on support looks perfectly correct on disk and silently loads nothing.

The file list, the verified versions and the download links are one data file, `tools\install_manifest.json`; the
checks in `tools\install_checks.ps1` only render it.

The shipped ini is the configuration v1.1.1 was verified with: DLAA preset K at 3840x2160, jitter + camera and object motion vectors, DLSS 5 NR through `renodx-dlss5`, dynamic-resolution handling, depth of field re-applied after NR (`PostDof=1`) and dynamic frame generation to 240 fps. The diagnostic keys at the bottom (`TraceFreeze`, `TraceFrames`, `Probe`, `DumpShaders`) are off; turning them on costs frames.

### The setup this was verified on

The add-on and the ASI come out of this repo, but the rest of the stack lives in the game folder and is worth
recording, because "DLSS 5 NR is on" is not one setting. Versions the v1.1.1 numbers were taken with (the same
versions `tools\install_manifest.json` checks against, so change both together):

| component | file in `MGS4\` | version |
| --- | --- | --- |
| ReShade with add-on support | `dxgi.dll` | 6.8.0 |
| Ultimate ASI Loader | `winmm.dll` | 9.7.4 |
| DLSS / DLSS-G / DLSS NR | `nvngx_dlss.dll`, `nvngx_dlssg.dll`, `nvngx_dlssnr.dll` | 310.8.0 |
| Streamline | `sl.interposer.dll` and the other `sl.*.dll` | 2.13.0 |
| DLSS 5 Neural Rendering add-on | `renodx-dlss5.addon64` | - |
| frame limiter | `scripts\MGSFPSUnlock.asi` + `MGSFPSUnlock.ini` (`TargetFrameRate = 60`) | - |

Settings that are not files this repo installs:

- **Game** (`mgs4_savedata_win\<steamid>\mgs4\mgs4.savedsettings`): `api=dx12`, `vsync=false`, `fpsLimiter=60`,
  `enableFXAA=false`, quality settings at 3.
- **RenoDX DLSS 5 NR**, in `MGS4\ReShade.ini` under `[RenoDX.DLSS5]` — the NR look the screenshots were taken with:
  `NeuralUplift=1`, `NRIntensity=2`, `NRStyle=2`, `NRLocalTone=1`, `NRSkinStructure=-1`, `NREnableUpscaling=0`
  (upscaling off: this add-on already runs DLAA on the final image, so NR only denoises / uplifts it).
- **`steam_appid.txt`** containing `2492670` next to `mgs4.exe`, so `--stage` boots do not bounce through Steam.
- **`scripts\MGS4_D3D12.ini`**: `Enabled = 0` when the native D3D12 option below is used — the ASI is the fallback
  for builds without it, and running both is pointless (see "Native Direct3D 12 option").

### Build / install (from source)

Requirements: MSVC Build Tools, ReShade 6.8 installed as `MGS4\dxgi.dll`, the D3D12 switch above, and `nvngx_dlss.dll` in `MGS4\` (copy `third_party/DLSS/lib/Windows_x86_64/rel/nvngx_dlss.dll` or let the NVIDIA app override supply it). `build.bat` finds the MSVC environment through `vswhere` and `fxc.exe` in the newest Windows 10 SDK; `MGS4_VCVARS` / `MGS4_FXC` in `config.ini` override that. The install script copies to whatever game folder is configured — see [Paths](#paths-configini).

```bat
dlss-addon\build.bat                       :: -> build\mgs4_dlss.addon64
```
```sh
sh dlss-addon/install.sh                    # -> <game>\MGS4\ (keeps an existing mgs4_dlss.ini)
```

`MGS4\mgs4_dlss.ini` (the release configuration):

```ini
[DLSS]
Enabled=1                ; live-reloaded every ~second
Mode=DLAA                ; DLAA (=Native) | Quality | Balanced | Performance | UltraPerformance  - needs a game restart
InternalRes=3840x2160    ; size of the game's render targets = your in-game resolution; auto-detected and written on first run
Preset=11                ; NVSDK_NGX_DLSS_Hint_Render_Preset_K (transformer). 10 = J
Sharpness=0              ; 0..100 (live)
LogEveryN=600
RecreateAfter=0          ; 0 = never re-create the feature (NGX-hooking add-ons are detected and handled automatically)
DebugMode=0              ; live: 1 = magenta path test, 2 = bypass DLSS (A/B), 3 = trace 3 frames, 4 = analyse draw constants, 5 = motion-vector field, 9 = vector field blended over the image (alignment check)
Jitter=1                 ; live: Halton camera jitter patched into scene draw constants
JitterSignX=1            ; NDC sign conventions (defaults follow the DLSS/Unreal convention)
JitterSignY=-1
MotionVectors=1          ; live: camera motion vectors from depth (compute pass)
DynamicZeroMV=0          ; character mask options (superseded by ObjectMV, kept for reference)
DynamicMaskProps=0
DynamicMask=0
PrePost=auto             ; DLAA insertion: auto = on the final image when a DLSS post-processing add-on (DLSS 5 NR) is loaded, else before post/HUD

PostDof=1                ; 1 = skip the game's depth-of-field draws and re-apply the same DoF on the DLSS / NR output (see "Depth of field after NR")
DofStep=2.0              ; PostDof tuning: spiral step scale (2.0 = the game's blur size at the full grid)
DofRadius=1.0            ; PostDof tuning: multiplier on the game's circle of confusion
DofJitterSign=1          ; PostDof: read the depth at the frame's camera-jitter offset (the depth copy is jittered, the DLSS output is not); 0 = off
DofMask=1                ; PostDof: keep title cards / captions drawn after the game's DoF sharp (0 = diagnostics)
DofSubRect=1             ; PostDof also on dynamic-resolution frames (exact per-frame scale from the CoC pass viewport); 0 = leave those frames to the game's DoF
DofStepFreeze=0          ; 1 = on a resolution-step frame hold the previous DLSS output for one frame instead of showing the game's own frame
PreWarm=1                ; create the DLSS feature (+ the NR add-on's) and run warm-up evaluations on no-3D frames (title / loading screens) so the setup stall is not in the first cutscene frames

FrameGen=4               ; 0 off, 1 = 2x, 2 = 3x, 3 = 4x, 4 = dynamic to FGTargetFps (needs the Streamline runtime; restart to load it)
FGTargetFps=240          ; match your display's refresh rate; the game itself runs at 60
Reflex=1
ObjectMV=1               ; per-object motion vectors (stream-out of the game's vertex shaders)
SceneLog=1
DRS=1                    ; dynamic-resolution handling (full grid); 2 = legacy sub-rect evaluation (reference only)
WindowScene=1            ; DLSS on a 3D window's own render target (the Codec caller): the caller's scene gets DLAA/NR, the CRT overlay and the panels around it do not
FrozenBackground=1       ; live: pause menu / Codec: run DLSS before the game captures the still background it shows behind those screens
UIMask=1                 ; live: HUD from the replayed UI layer -> DLSS bias-current-colour mask + zero vectors on bright HUD detail (no HUD ghosting under camera motion)

TraceFreeze=0            ; diagnostics: log the full-size draw chain around the moment the world stops rendering
TraceFrames=0            ; diagnostics: N = trace every full-frame draw for the next N frames (live)
Probe=0                  ; diagnostics: sample the pipeline before / after the insertion and after the post chain
DumpShaders=0            ; 1 = write every pipeline's VS/PS bytecode to logs\shaders\<hash>.{vs,ps}.dxbc (pass identification)
```

Log: `MGS4\logs\mgs4_dlss.log`.

### DLSS modes (upscaling)

`Mode` other than DLAA makes the game render smaller and lets DLSS upscale:

1. At device creation NGX's optimal settings give the render resolution for the mode (e.g. 3840x2160 Quality -> 2560x1440, Performance -> 1920x1080, Ultra Performance -> 1280x720).
2. Every texture the game creates at `InternalRes` is shrunk to the render resolution (`create_resource` event); viewports and scissors of draws into shrunk targets are scaled to match.
3. At the composite draw DLSS upscales the shrunk final texture into a full-size output and the draw's SRV descriptor is rewritten in place to sample that output, so the composite and the backbuffer are untouched.

Because the game's render-target size follows the in-game resolution, `InternalRes` must match it; the add-on writes the detected value to the ini whenever it differs (change resolution in-game -> restart once). Verified in Performance mode (1512x850 -> 3024x1701): correct composition, HUD and pillarboxing; other modes use the same path but were not individually tested.

With the camera jitter and the per-object motion vectors in place, the upscaling modes are real DLSS super-resolution; DLAA remains the reference for image quality. `Mode=Quality` is the first thing to try when the GPU cannot hold the game's native resolution (the port lowers its own dynamic resolution under load, see below).

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
Character animation gets its own motion vectors from Phase 2 below. Use the overlay toggles to compare.

### Phase 2: per-object motion vectors (`ObjectMV`, on by default)

Characters and props get real motion vectors without touching a single shader. For every dynamic draw (skinned
meshes — PSOs whose input layout has `BLENDWEIGHT`/`BLENDINDICES`; props with their own model matrix when
`DynamicMaskProps=1`) the add-on issues **one** extra draw with a stream-out variant of the game's pipeline (same vertex
shader and input layout, no rasterisation) that writes the clip-space position of every emitted vertex into this frame's
buffer. The buffers ping-pong: the capture becomes next frame's "previous positions" for the same draw (same geometry,
same n-th occurrence in the frame). At the injection point one draw per object rasterises the current positions and
writes `previous - current` in pixels into the motion-vector texture, depth-tested (greater-equal, reversed-Z, cull mode
and winding copied from the game pipeline) against the scene depth, so only visible object surfaces replace the
camera-only vectors. The captured positions carry the add-on's sub-pixel jitter; the velocity shader removes it.

What made the first version slow, and what this one does instead:

- v1 streamed out twice per draw (current + recorded previous constants) under a cloned root signature; a root signature
  switch invalidates every root argument, so each draw also paid a full state restore, and the CPU copied 8 KB of
  constants per draw. 60 fps -> 33 fps.
- v2 adds `ALLOW_STREAM_OUTPUT` to the game's own root signatures when they are created (`CreateRootSignature` is hooked
  and the blob re-serialised), so the stream-out pipeline binds under the game's root signature with the game's root
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
- Overlay: "Object motion: N captured (N with history, ...)". The MV visualiser (`DebugMode=5`, live) shows object
  motion as colour differing from the camera field — it can be flipped on for a few seconds during a recording.
- With real object vectors the character mask (`DynamicMask`) is no longer needed and is off by default.

### Dynamic resolution in the port (`DRS`, on by default)

On the native D3D12 path the port renders the 3D scene into a **variable sub-viewport** of its full-size targets
(observed anywhere from 100 % down to 50 %: 3840x2160 -> 3712x2088 -> ... -> 1920x1080, changing every second or so
when the GPU is loaded — e.g. with frame generation at 4K120). **Its post chain upscales that sub-rect to the full-size
final image before the composite; the composite samples the whole texture.** So at the add-on's insertion point the
colour is always full-size, while the scene depth (and anything derived from it) is on the sub-rect grid. Verified
with the vector overlay (`DebugMode=9`): with the earlier assumption that the composite stretches the sub-rect, the
overlay covered only the top-left (k x k) part of the screen.

`DRS=1` therefore puts depth and vectors on the full grid: the scene depth is stretched (nearest) into a full-size R32
copy for DLSS and frame generation, the camera vectors are computed per full-grid pixel from the sub-res depth, the
object vectors are rasterised with the full viewport (their clip positions are viewport-independent) and depth-tested
manually against the stretched depth, the jitter is expressed in full-grid pixels, and DLSS / NR / DLSS-G all see a
full-size contract. Getting this wrong showed up as: a DLSS-G ghost of moving characters displaced ~(1-k) toward the
top-left, asymmetric ghosting in DLSS itself, and (in the old sub-rect mode) DLSS 5 NR covering only the top-left
rectangle while the game was scaled.

`DRS=2` keeps the legacy behaviour (DLSS evaluated on the sub-rect, output resampled back into it) for reference; it is
wrong for this port's composite and breaks NR's coverage.

The sub-rect is detected per frame from the viewport most depth-tested draws into the frame's geometry target use
(at least half the target); the 20-frame hysteresis copy is only a fallback before the first scene draw of a frame.

### HUD: DLSS before the HUD, on the final texture

The port draws its HUD into the final texture **before** the composite. In the DLSS 5 NR configuration (DLAA on the
final image) that used to put the HUD inside the image DLSS reprojected - HUD ghosting opposite to camera turns - and
the HUD-less colour for frame generation had to be patched together from a pre-HUD capture (raw, no DLAA/NR under
the HUD elements: visible rectangles and blur/flicker around the HUD in generated frames).

Since v1.0.1 the add-on runs DLSS at the **first HUD draw of the frame, on the final texture**: the scene has been
upscaled/tonemapped into it, the HUD has not been drawn yet. DLSS and the inline NR add-on never see the HUD, the
game then draws the HUD on top of the DLSS output, and that output *is* the HUD-less colour for DLSS-G (no patching);
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
DLSS's bias-current-colour mask is set and the motion vector zeroed. It has no effect while the pre-HUD insertion
is active (the HUD is not in DLSS's input there).

### 3D windows: the Codec caller (`WindowScene`, on by default)

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

### Frozen screens: the pause menu and Codec backgrounds (`FrozenBackground`, on by default)

Behind the pause menu and the Codec the game shows a **still image of the world**, and until v1.1 that image was
the raw (non-DLSS, non-NR) frame, which broke the illusion the moment you paused. The mechanism, from the freeze
trace (`TraceFreeze=1`): on the last live frame the game draws its upscaled scene into the final texture, then a
6-vertex draw **downsamples that final texture into a 1920x1080 seed texture**, and only then come the HUD draws -
so the capture ran *before* the pre-HUD DLSS insertion. From the next frame on the world is no longer rendered; every
frame of the pause menu and of the Codec begins with a full-screen blit of that seed into the final texture, then
the panels (and the 3D window with the model / the caller, which `WindowScene` handles). Nothing ever rewrites the
seed, so whatever it captured is the background for the whole time the screen stays frozen.

With `FrozenBackground=1` the add-on recognises that capture (a few-vertex draw into a smaller, non-final target
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

### Resuming from a frozen screen keeps the history

Leaving the pause menu (or a dismissed dialog) used to reset DLSS and NR: a visible drop in detail that re-converged
over about a second (measured: -15 % background sharpness at the unpause). The world has not moved during the freeze,
so the history from the last live frame is still valid. The add-on remembers the camera of the last live evaluation
and, when the first evaluation after a frozen screen sees the same camera (the cut heuristic's thresholds), it keeps
the history: no reset, motion computed relative to that last live frame rather than to the pause menu's model camera,
and only the rectangle the 3D window (the Snake model) occupied meanwhile is excluded for that one frame (its history
belongs to the model, not the world). Log: `RESUME f…: same camera as before the freeze -> DLSS history kept`. A
different camera still resets as before. Two details that mattered: the frozen state is only cleared by a colour scene
write from a geometry target (the pause menu's closing frame samples the *depth* texture full-screen into the final
texture, which looked like fresh content and reset the history one frame early), and the pause menu's own hundreds of
depth-tested panel quads never count as a live scene.

### Frame generation on frozen screens and at Codec transitions

DLSS-G interpolates between consecutive game frames; on a frozen screen that gains nothing, and across the Codec's
transitions (the panel collapse when a call starts, the caller window appearing) it produced single torn frames -
displaced copies of the panel lines and of the caller window flashing across the centre. Note that an NVIDIA-app
frame-generation preset override disables DLSS-G's UI recomposition ("Preset A selected, disabling UIR" in sl.log),
so the UI-layer / HUD-less tags cannot protect moving UI in that configuration. The add-on therefore reports a cut
(`reset`) to DLSS-G on every frozen pass-through frame and for the first 8 evaluations after a transition (a
pass-through frame ending, the insertion moving between the final texture and a 3D window), and also forces a DLSS
history reset on the first evaluation after such a transition (the caller's first frame used to come out warped from
seconds-old history). In window mode it now also tags a HUD-less image (the pre-HUD capture, valid until present) and
the UI layer for DLSS-G, which helps where UI recomposition is available.

### v1.1.1: the v1.1 freeze logic misfired inside live cutscenes

v1.1's frozen-screen logic could trigger inside a running cutscene, and every misfire cost a raw frame (no DLSS, no
NR), a history reset and - with frame generation on - an eight-frame DLSS-G cut: visible as flicker and stutter that
v1.0 did not have. Three causes, all fixed: the pass-through fired on any frame whose camera matrix or scene write was
missed (now only from the seed-freeze state and only from the second consecutive frame without a scene write); the
seed-capture insertion matched a cutscene's own 2048x2048 screen capture (now only the half-resolution seed), moving the
insertion point on a few percent of frames; and a picture-in-picture pass (the Mk. II's monitor, 1024x1024 in a corner
of the scene target, hundreds of draws) took the frame's "3D target" slot, so the scene itself was never recognised and
the 3D-window insertion ran on the picture-in-picture instead (now a 3D target needs a full-frame viewport, and the
window path needs the previous frame to have had no scene either). Verified on the Mk. II intro and the Meryl garage
scenes: no window switches, no mid-scene seed insertions, no pass-through frames, resets only at real camera cuts.

### Known limitations

- Alpha-tested surfaces (hair cards) get object vectors over their transparent texels too (the velocity pass has no alpha test); not visible in practice.
- `Mode` changes need a restart (render targets are created at startup).
- GPU load: DLAA + NR + frame generation at 4K120 can push the port's own dynamic resolution down to 50 %; the add-on renders correctly at any scale, but sharpness follows the game's choice — `Mode=Quality`, a fixed `FrameGen=1` or a lighter streaming encode keep it at native.
- Frame generation on a 60 Hz output only adds real/generated alternation; use it with a 120 Hz (or faster) display or virtual display.
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
sh d3d12-switch/install.sh    # copies asi + ini to MGS4/scripts/ (game folder: see Paths below)
```

Revert to stock D3D11: set `Enabled = 0` in `MGS4/scripts/MGS4_D3D12.ini`. Log: `MGS4/logs/MGS4_D3D12.log`.

## Layout

```
mgs4-dlss.bat          the app: Play / Settings / Install (launcher.bat and check-install.bat alias it)
config.example.ini     machine-local paths; copy to config.ini (git-ignored)
d3d12-switch/          mgs4_d3d12.c, MGS4_D3D12.ini, build.sh, install.sh
dlss-addon/            src/mgs4_dlss.cpp, build.bat, install.sh, mgs4_dlss.ini (sample)
tools/paths.py|ps1|sh  where the game / the output folder live on this machine
tools/mgs4_dlss.ps1    the app itself; tools/install_checks.ps1 the checks behind its Install tab
tools/install_manifest.json  the file list, verified versions and download links the checks render
tools/scenes.csv       every launchable scene; tools/labels.json the curated names
third_party/minhook/   MinHook (BSD-2), vendored
third_party/reshade/   ReShade add-on API headers (v6.8.0, BSD-3)
third_party/DLSS/      NVIDIA DLSS SDK headers + nvsdk_ngx_s.lib (DLLs git-ignored)
docs/                  reverse-engineering notes and the DLSS plan
```

## Paths (`config.ini`)

The checkout can live anywhere; nothing in it assumes a path. Every script asks `tools/paths.py` (Python),
`tools/paths.ps1` (PowerShell) or `tools/paths.sh` (Git Bash) for the machine's paths, and all three resolve each
value the same way: **environment variable > `config.ini` in the repo root > auto-detection**.

| key | what | detected as |
| --- | --- | --- |
| `MGS4_DIR` | the folder holding `mgs4.exe` (the install root also works) | the Steam library folder that has app 2492670, on any drive |
| `MGS4_OUT` | recordings, gold clips, screenshots, analysis output | `<repo>\work` |
| `MGS4_FFMPEG` / `MGS4_FFPROBE` | video tools used by the capture / analysis scripts | PATH |
| `MGS4_PYTHON` | interpreter the detached gold worker starts | PATH |
| `MGS4_VCVARS` / `MGS4_FXC` | MSVC environment and shader compiler for `build.bat` | `vswhere`, newest Windows 10 SDK |

```
copy config.example.ini config.ini    :: then edit; every key is optional
python tools\paths.py                 :: what resolved to what, and where each value came from
```

A second install (a different drive, a different machine, a copy of the game) needs only `MGS4_DIR` in `config.ini`
or in the environment: `MGS4_DIR="D:\SteamLibrary\steamapps\common\METAL GEAR SOLID 4\MGS4" sh dlss-addon/install.sh`.
The PowerShell scripts also still take `-GameDir` for a one-off run.

### Phase 2 (first step): character mask

Skinned meshes (PSOs whose input layout has `BLENDWEIGHT`/`BLENDINDICES`, reported by ReShade at pipeline creation)
are replayed once into a private depth buffer (same PSO, same jittered constants, no colour target). The motion-vector
pass turns that depth into DLSS's **bias-current-colour mask**, so DLSS leans on the current frame for character
pixels instead of reprojected history that camera-only vectors cannot describe. Result: no halo/ghosting around
characters, at the cost of a little temporal accumulation on them. Options (panel / ini): `DynamicMask` (default off),
`DynamicMaskProps` (also mask props with their own model matrix — off; static props are correct with camera vectors),
`DynamicZeroMV` (zero motion on masked pixels — off; useful for third-person camera turns where the player stays
centred). The MV visualiser (`DebugMode=5`) shows the mask in blue. Superseded by the per-object motion vectors above; kept as an option (`DynamicMask=1`).

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

### Depth of field after NR (`PostDof=1`)

The port applies its depth of field inside the scene target (three passes: a half-resolution circle-of-confusion pass
that samples the linear depth copy, a golden-angle spiral bokeh gather, and an alpha blend of the blurred layer over the
sharp image) before the tonemap / upscale into the final texture. DLSS and the DLSS 5 NR add-on therefore only ever see
the defocused image: an out-of-focus character carries no NR detail and the NR look "pops in" on every rack focus.
With `PostDof=1` (live key, panel checkbox) the three draws are skipped - identified by the FNV-1a hash of their pixel
shader bytecode (`733f4efc`, `92bbc108`, `bca9c941`) - the CoC constants (`cb0[8..17]`) and the depth copy are taken
from the skipped CoC pass, and an exact HLSL transcription of the three passes (`dof_coc_cs`, `dof_gather_cs`,
`dof_composite_cs`) runs on the DLSS output before it is copied back, so the blur is applied to the NR-processed image.
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
and without that every blur boundary wobbled by a sub-pixel per frame.

### Pre-warm (`PreWarm=1`)

The DLSS feature is normally created at the first 3D frame, and the DLSS 5 NR add-on creates its own feature inside
that call and initialises its model on the first evaluations - about half a second of stalls and dropped resolution
right at the start of the first cutscene. With `PreWarm=1` the add-on creates the feature at the swapchain size (DLAA)
and runs 12 evaluations on its own scratch textures during frames without a 3D scene (the title / loading screens,
after 30 such frames), restoring the game's state after each; the NGX-hooking add-on's one re-create happens there too.
Log lines `pre-warm: ...` show the timings; the stats line counts `pre-warm evaluations`.
`DumpShaders=1` writes every pipeline's bytecode to `logs\shaders` (with `TraceFreeze=1` + `DebugMode=3` the freeze
trace lists each full-frame draw with its `ps=` hash) - that is how the three passes were found.

### Direct stage boot

`mgs4.exe --stage <name>` skips the Master Collection screen and the menus: `s00title_1` (OTC intro), `s00a00l` (cemetery opening),
`s01a00l` (Act 1 start), ... (names listed in the exe). `steam_appid.txt` next to the exe keeps Steam from
relaunching. A desktop shortcut "MGS4 (stage s00a00l)" boots straight into the cemetery for quick tests.

`mgs4-dlss.bat` (see [The app](#the-app-mgs4-dlssbat)) does this and the rest of it — a scene list, the
Cross tapping the flashback prompts want, ending a scene when gameplay starts — and it is what the desktop shortcuts
and `tools\test_stages.ps1` call. `tools\launch_stage.ps1` is still there as a shim over it, so existing shortcuts
and notes keep working:

```bat
mgs4-dlss.bat s00a00l                                 :: boot it, press through the prompts, exit
mgs4-dlss.bat s00a00l --keys "5,ENTER,4,ENTER"        :: an explicit key sequence instead (menus)
mgs4-dlss.bat s00a00l --mash-x --end-on-gameplay      :: play the whole cutscene, then close the game
```

Keys go through `keybd_event` with the window forced to the foreground — this port ignores scan-code `SendInput`
events, and only accepts input while it is the foreground window. Log: `MGS4\logs\launcher.log`.

### Recording the in-game cutscenes (4K60 AV1)

`tools\record_cutscenes.py` records every one of the in-game cutscenes unattended (102 entries in `tools\scenes.csv`):

- boots each `<stage>_D<n>` entry in chronological order (`tools/scenes.csv`),
- drives OBS over obs-websocket (`tools/obs_control.py`): a dedicated scene with a Game Capture source cropped to the
  game's 16:9 image and fitted to a 3840x2160 / 60 fps canvas, NVENC **AV1** at CQP 22 into `MGS4_OUT`,
- taps **Cross** on a virtual DualShock 4 (`tools/ds4.py`, ViGEmBus through ctypes - no installer) about once a second:
  it gets past the auto-save notice and "press any button" screens and triggers MGS4's in-cutscene **flashback**
  prompts. The game only accepts input while it is the foreground window, so `tools/winfocus.py` re-focuses it,
- decides that a cutscene is over from the add-on's `SCENE-STATE` log (HUD appearing = gameplay) or from a static
  screen - the recording's byte rate separates a static continue/act-end screen from a prerecorded video playing
  inside the cutscene, so long Bink segments are not cut off,
- checks the free space on the output drive after every recording and stops below the configured floor (default 100 GB).

`tools/label_recordings.py` then pulls thumbnails out of the recordings, applies semantic names from `labels.json`
(`23_act2-south-america_naomi-lab-rose-garden_s02a50l_D1.mkv`) and writes `index.csv` / `index.md`.

### Stage rotation for testing

`tools\test_stages.ps1 -Stages "s00a00l,s02a50l_D1,s03a30l_D1" -HoldSeconds 40 [-MvVis] [-ObjectMV]` boots each stage/cutscene in turn (`tools\stages.md` lists the 75 stage ids from the executable and their `_D<n>` cutscene / `_<n>` section variants; `s02a50l_D1` is the Naomi lab scene), waits for the first 3D frame, holds, takes screenshots (and the motion-vector visualiser with `-MvVis`), and writes a per-stage log digest plus `summary.txt` (evaluations, DRS frames, last scene viewport, crashes) to `<MGS4_OUT>\stage_tests\<timestamp>\` (`-OutDir` overrides).

### Robustness

Level transitions destroy and recreate render targets; every cross-frame handle (busiest/geometry/final targets, last
depth, RT->depth map, descriptor-copy map) is validated against the set of live textures and dropped on destruction.
