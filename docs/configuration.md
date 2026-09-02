# Configuration

Every setting of the add-on lives in `MGS4\mgs4_dlss.ini`, next to `mgs4.exe`. Keys marked *live* are re-read about once a second while the game runs; the others need a restart. The ReShade overlay (Home key, Add-ons tab) edits the same file, and the launcher's Settings tab shows every key as a form - it also holds the game's own settings and RenoDX's, so it is the one place to change all three. Do not edit the ini by hand while the game runs: the add-on rewrites it through the Windows profile API and an outside edit is lost.

## The shipped ini

This is the configuration v1.2.0 ships with and was verified on:

```ini
[DLSS]
Enabled=1                ; live-reloaded every ~second
Mode=DLAA                ; DLAA (=Native) | Quality | Balanced | Performance | UltraPerformance  - needs a game restart
RenderRes=               ; empty = Mode decides; 2560x1440 / 1920x1080 / 1280x720 = upscale from exactly that resolution (DLAA when it is not below the target) - needs a game restart
InternalRes=3840x2160    ; size of the game's render targets = your in-game resolution; auto-detected and written on first run
Preset=11                ; NVSDK_NGX_DLSS_Hint_Render_Preset_K (transformer). 10 = J
Sharpness=0              ; 0..100 (live)
LogEveryN=600
RecreateAfter=0          ; 0 = never re-create the feature (NGX-hooking add-ons are detected and handled automatically)
DebugMode=0              ; live: 1 = magenta path test, 2 = bypass DLSS (A/B), 3 = trace 3 frames, 4 = analyze draw constants, 5 = motion-vector field, 9 = vector field blended over the image (alignment check)
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
ObjectMVProps=0          ; live: 1 = object vectors also for rigid props with their own model matrix (vehicles, the Mk. II, doors), not only skinned meshes; one extra stream-out draw per such prop
CutPosLimit=6000         ; live: camera-cut heuristic - a position jump above this many game units (millimeters) in one frame resets the DLSS history; 1500 fired on every 2 m aim / cover snap
ObjectMVMaxPixels=200    ; live: an object vector longer than this (pixels per frame) is dropped for the camera vector - a wrong capture pairing gives hundreds; 0 = no limit
ObjectMVMaxGradient=4    ; live: an object vector field changing by more than this many pixels per screen pixel across one surface is dropped (the same mesh paired with a capture in another projection); 0 = no limit
FGHintRescale=0          ; live: 1 = in a window that is not the render size, rescale the HUD-less / UI hints to the backbuffer for DLSS-G (two 4K passes + DLSS-G's UI work; benefit not shown in testing)
SceneLog=1
DRS=1                    ; dynamic-resolution handling (full grid); 2 = legacy sub-rect evaluation (reference only)
WindowScene=1            ; DLSS on a 3D window's own render target (the Codec caller): the caller's scene gets DLAA/NR, the CRT overlay and the panels around it do not
FrozenBackground=1       ; live: pause menu / Codec: run DLSS before the game captures the still background it shows behind those screens
UIMask=1                 ; live: HUD from the replayed UI layer -> DLSS bias-current-color mask + zero vectors on bright HUD detail (no HUD ghosting under camera motion)

TraceFreeze=0            ; diagnostics: log the full-size draw chain around the moment the world stops rendering
TraceFrames=0            ; diagnostics: N = trace every full-frame draw for the next N frames (live)
Probe=0                  ; diagnostics: sample the pipeline before / after the insertion and after the post chain
DumpShaders=0            ; 1 = write every pipeline's VS/PS bytecode to logs\shaders\<hash>.{vs,ps}.dxbc (pass identification)
```

Log: `MGS4\logs\mgs4_dlss.log`.

The diagnostic keys at the bottom (`TraceFreeze`, `TraceFrames`, `Probe`, `DumpShaders`) and the debug views cost frames; leave them off for normal play.

## Overlay controls

The add-on has its own panel in ReShade's **Add-ons** tab (Home key): Enable, DLSS mode (applies on restart — the game creates its render targets once at startup; the panel says so when the selection differs from the active mode), DLSS preset (J/K, applied live by re-creating the feature), sharpness, camera jitter, camera motion vectors, debug modes, frame generation (Off/2x/3x/4x/Dynamic + target fps, Reflex; applied live), and live status (feature size, evaluations/s, jitter, VP detection, MV resets). Every control writes `mgs4_dlss.ini`.

## DLSS modes (`Mode`)

`Mode` other than DLAA makes the game render smaller and lets DLSS upscale:

1. At device creation NGX's optimal settings give the render resolution for the mode (e.g. 3840x2160 Quality -> 2560x1440, Performance -> 1920x1080, Ultra Performance -> 1280x720).
2. Every texture the game creates at `InternalRes` is shrunk to the render resolution (`create_resource` event); viewports and scissors of draws into shrunk targets are scaled to match.
3. At the composite draw DLSS upscales the shrunk final texture into a full-size output and the draw's SRV descriptor is rewritten in place to sample that output, so the composite and the backbuffer are untouched.

Because the game's render-target size follows the in-game resolution, `InternalRes` must match it; the add-on writes the detected value to the ini whenever it differs (change resolution in-game -> restart once). Verified in Performance mode (1512x850 -> 3024x1701): correct composition, HUD and pillarboxing; other modes use the same path but were not individually tested.

With the camera jitter and the per-object motion vectors in place, the upscaling modes are real DLSS super-resolution; DLAA remains the reference for image quality. `Mode=Quality` is the first thing to try when the GPU cannot hold the game's native resolution (the port lowers its own dynamic resolution under load, see [Dynamic resolution](dlss-pipeline.md#dynamic-resolution-in-the-port-drs-on-by-default)).

### An explicit source resolution (`RenderRes`)

`Mode` implies a ratio: at a 4K target NVIDIA's Quality is 2560x1440, Performance 1920x1080 and Ultra Performance
1280x720 (Balanced is the odd one at 58 %). `RenderRes=WxH` picks the source resolution directly instead: the add-on
keeps the target's aspect (the requested height rules and the width follows, so an ultrawide is not squashed), treats a
request at or above the target as DLAA, and chooses the NGX mode whose optimal size is closest among those whose accepted
range holds the request - the mode selects the model preset and the size range the driver enforces. Ultra Performance is
pinned at a third of the target, so a request no range holds (720p at a 3784x2128 target) is clamped into the closest
range and the log says so (`RenderRes ... is outside every mode's accepted range; the closest, ...`). The launcher's
Settings tab offers it as a list: the mode decides, 2160p, 1440p, 1080p, 720p. Like `Mode` it needs a restart.

## Frame generation (`FrameGen`, `FGTargetFps`, `Reflex`)

`FrameGen`: 0 off, 1 = 2x, 2 = 3x, 3 = 4x, 4 = dynamic to `FGTargetFps` (0 = the monitor's refresh rate). Frame generation only helps when the display (or the virtual display you stream from) refreshes faster than the game's 60 fps - set `FGTargetFps` to your refresh rate; on a 60 Hz output set `FrameGen=0`. Streamline is only loaded when `FrameGen` is non-zero at startup, so the first switch from Off needs a restart; after that every value changes live. The technical side is in [the pipeline notes](dlss-pipeline.md#frame-generation-dlss-g--multi-frame-generation-via-streamline).

## Dynamic resolution and what the overlay says about it

**Why the overlay may say the scene is 1920x1080 with DLAA on.** The game decides its own render scale from its GPU load,
and everything the add-on runs inside its frame (DLAA, DLSS 5 NR, the vector passes) counts toward that budget. Under load
it renders the 3D scene at 50 % (1920x1080 of 3840x2160) and upscales it itself before DLSS sees the image; DLAA then
runs on the full-size image but cannot add detail the game never rendered. There is no game-side switch
(`mgs4.savedsettings` only has the quality tiers). `Mode=Quality` is the way out: the add-on shrinks the game's targets to
the DLSS render resolution, the game has nothing left to scale down, and DLSS super-resolves properly jittered samples -
far better than DLAA over the game's bilinear 1080p upscale. Test the cause live with the overlay's "Enable DLSS"
checkbox: with it off the "Game dynamic resolution" line should climb back to full size within seconds.

## Debug views (`DebugMode`)

| value | what is shown |
| --- | --- |
| 0 | off |
| 1 | magenta path test: the displayed image is painted magenta (proves the insertion path) |
| 2 | bypass DLSS (A/B against the native image) |
| 3 | trace three frames to the log |
| 4 | analyze draw constants (log) |
| 5 | the motion-vector field on its own |
| 6 | the replayed UI layer (frame generation input) |
| 7 | the HUD-less color (frame generation input) |
| 9 | the motion-vector field blended over the image - a character's vector silhouette must sit on the character |
| 10 | PostDof: the blurred layer only |
| 11 | PostDof: blur coverage |
| 12 | PostDof: the overlay mask |

All of them are in the overlay's Debug combo.
