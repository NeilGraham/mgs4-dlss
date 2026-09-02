# Releases

Every release carries three assets: `mgs4-dlss-launcher.exe`, `mgs4_dlss.addon64` and `mgs4_dlss.ini`. The exe is
the launcher with the same add-on and ini built into it, so it is the only download most people need; the loose pair
is for a manual install or for updating an add-on that is already in place. Install steps are in
[install.md](install.md). Releases before v1.3.0 carried `mgs4_dlss_launcher.zip` instead, a source copy of the
launcher that built itself on first run.

## v1.3.0 (2026-09-01)

The launcher is one file, and it is the release.

### One exe

- **`mgs4-dlss-launcher.exe` replaces `mgs4_dlss_launcher.zip`.** `mgs4_dlss.addon64` and `mgs4_dlss.ini` are built
  into the exe as resources, next to the XAML, the scene table and the install file list that already were, so
  **Install the add-on** on the Setup tab (and `--install-addon`) writes them from inside the launcher. Nothing to
  unzip, no first-run build, no `.bat`. The add-on is written to a temporary name and moved into place, so a copy
  that fails part way never leaves a truncated `.addon64` for ReShade to load.
- **The launcher's own icon.** The exe a release carries cannot wear the icon a local build takes from `mgs4.exe`,
  so `launcher\build.ps1 -Release` embeds one drawn by `tools\launcher_icon.ps1` instead: a coarse block of pixels
  and a chevron out of it. A local build still takes the game's, and falls back to this one when there is no
  `mgs4.exe` to read.
- **A lone exe writes nothing beside itself.** Outside a checkout, `config.ini` and the `work\` folder go under
  `%LOCALAPPDATA%\mgs4-dlss-launcher`, where the window's own preferences already were. In a checkout they stay in
  the repo root where every script reads them, and a `config.ini` already next to the exe is used wherever it is.
- **Releases from a tag.** `tools\package_release.py` now builds the release exe, and `.github\workflows\release.yml`
  does the same on a push of `v<version>`: the add-on, the exe, and a release with that version's section of this
  file as its body.

## v1.2.0 (2026-09-01)

The depth-of-field layer is separated from the scene for DLSS 5 rendering, the flicker in scenes is gone, and the
launcher is part of the release.

### Depth of field after Neural Rendering

The port applies its depth of field inside the scene target, before the image reaches DLSS, so DLSS 5 Neural Rendering
only ever saw the defocused picture: an out-of-focus character carried no NR detail and the NR look popped in on every
rack focus. With `PostDof=1` (on in the shipped ini) the game's three DoF passes are skipped and an exact transcription
of them runs on the DLSS + NR output instead, so the blur is applied to the neural-rendered image and the layer behind
it is what NR worked on. The pieces that make it hold up:

- the circle of confusion is evaluated at the game's own CoC draw, from the depth copy that draw was about to sample
  and with its constants, so nothing the game does to that texture later in the frame can reach the blur;
- dynamic-resolution frames are handled at the exact per-frame scale; a frame where the scale steps keeps the game's
  DoF for that one frame;
- title cards and captions the game draws after its DoF stay sharp (a replayed mask layer);
- the cutscene WIPE transitions' capture of the previous shot is redirected to the DoF'd output, so cuts no longer
  flash a sharp copy of the previous shot;
- the depth read is de-jittered (the depth copy is jittered, the DLSS output is not).

### Flicker in scenes

- **The blur layer no longer flickers into the top-left of the screen.** The DoF passes' constant buffer was written
  400 bytes into 256-byte ring slots, so a frame still running on the GPU could pick up the next frame's depth scale,
  jitter and mask flags; the descriptors were rewritten every frame the same way. Every per-frame GPU input of the DoF
  passes is ring-buffered over eight frames now.
- **No more pulsing on characters with frame generation.** Instances sharing one mesh (every PMC soldier) were paired
  across frames by draw order, so a change in sort order handed a soldier another soldier's previous positions, and the
  game's multi-pass character draws could pair the same mesh with a capture in another projection. Both produced a frame
  of saturated motion vectors on the body that DLSS-G warped into a visible pulse. Captures are now paired by their
  per-instance constant signature, and the velocity shader discards vectors that are implausibly long or vary wildly
  across one surface (`ObjectMVMaxPixels`, `ObjectMVMaxGradient`).
- **Aiming no longer resets the DLSS history.** The game's units are millimeters; the old camera-cut limit of 1500
  units fired on every 2 m aim / cover snap, a third of all history resets in gameplay, each a frame of raw aliasing on
  the characters. The limit is 6000 (`CutPosLimit`).
- Skinned character shaders whose view-projection sits past the first registers of their constants are now jittered
  too (the offset is learned once per vertex shader); those characters used to render unjittered under a jittered frame.

### The launcher

`mgs4-dlss-launcher` is one window for the whole add-on, and the same things as a command line: **Play** starts the
game or any one of the 400+ launchable scenes; **Settings** edits every setting as a form - the add-on's, the game's
own (`mgs4.savedsettings`) and RenoDX's NR settings; **Setup** finds the game folder, walks through the install in
order with drag-and-drop for every download, installs the add-on that ships with it, and validates the whole install
including what the last run's logs say actually happened. It is a C# WPF program built on first run by the compiler
that ships with Windows, so nothing has to be installed. Details in [launcher.md](launcher.md).

### Also

- DLSS receives the frame-time hint (`InFrameTimeDeltaInMsec`); every add-on pass shares one descriptor heap; the game's
  constant buffers are mapped once instead of per draw; descriptor-heap metadata is cached on the per-copy hot path.
- The overlay's Debug combo lists every debug view, including the motion-vector overlay and the DoF views, and the
  dynamic-resolution line says what the game is doing and why.
- `RenderRes=WxH` upscales from an explicit source resolution (1440p, 1080p, 720p; DLAA when it is not below the target),
  keeping the target's aspect and choosing the NGX mode whose range holds it; the launcher's Settings tab offers it as a list.
- Resolutions in the launcher are lists of the game's 16:9 sizes (720p, 1080p, 1440p, 2160p) - the render resolution on
  the Settings tab, the launch resolution on the Settings and Play tabs - instead of free width and height boxes.
- `ObjectMVProps=1` extends object motion vectors to rigid props; `FGHintRescale=1` rescales the frame-generation
  HUD-less / UI hints to the window size when it is not the render size (both off by default).
- The add-on works in a window smaller than the render resolution (the scene textures exceed the backbuffer there).

### Verified on

RTX 5090, driver 616.56, ReShade 6.8.0 with add-on support, DLSS / DLSS-G / DLSS NR 310.8.0, Streamline 2.13.0
(2.12.129 through the NVIDIA app's override), `renodx-dlss5` from the RenoDX Discord, at 3840x2160 DLAA preset K with
dynamic frame generation to 240 fps.

## v1.1.1 (2026-08-30, withdrawn)

The first release of the depth-of-field separation and of the fixes to the v1.1 freeze logic (its frozen-screen
handling could trigger inside a running cutscene, costing a raw frame, a history reset and an eight-frame DLSS-G cut).
The DoF work in it carried the ring-buffer bug described under v1.2.0 - the blur layer flickered into the top-left
sub-rect - so the release was withdrawn; v1.2.0 supersedes it.

## v1.1 (2026-08-30) - the illusion survives the menus

DLSS 5 Neural Rendering (via `renodx-dlss5`) and DLSS-G stay consistent through the pause menu, the Codec and back into
gameplay.

- **Frozen backgrounds are the DLSS (+NR) frame.** The game captures a 1920x1080 seed of the screen one frame before it
  freezes the world (pause menu, Codec) and recycles it; the capture ran before the DLSS insertion, so the still image
  was the raw render. DLSS now runs right before that capture, and the game's seed blit is redirected to a full-size kept
  copy of the DLSS output - the frozen frame is the live frame, pixel for pixel (`FrozenBackground=1`).
- **No second NR pass on frozen frames.** Frames that only recycle a frozen image are passed through instead of evaluated
  again (the sharpness jump right after pausing, the whole Codec list, blocking dialogs).
- **Codec call start without torn frames.** DLSS-G is told "cut" on frozen frames and for the first evaluations after a
  transition; window mode tags a HUD-less image and the UI layer; the caller's first frame no longer reprojects stale
  history.
- **Leaving the pause menu no longer resets DLSS / NR.** The history of the last live frame is kept when the camera is
  unchanged (only the model window's rectangle is excluded for one frame).
- Diagnostics: `TraceFreeze=1` ring-buffer trace of the draw chain around a freeze; `frozen state` / `RESUME` / `RESET`
  log lines; new overlay counters.

## v1.0.1 (2026-08-30) - HUD release

DLSS runs before the game's HUD, and the HUD / UI handling is stable under any input.

- **DLSS inserted before the HUD, on the final texture.** In the DLSS 5 NR configuration the HUD used to be inside the
  image DLSS reprojected (HUD ghosting opposite to camera turns) and the HUD-less color for frame generation was patched
  together from a raw pre-HUD capture. DLSS (and the inline NR add-on) now run at the frame's first HUD draw, after the
  game's upscale / tonemap wrote the scene into the final texture: the game draws the HUD on top of the DLSS output,
  that output is the HUD-less color for DLSS-G, and the replayed UI layer is tagged valid-until-present.
- **HUD classifier rewritten** around the shape and place of the draws, replacing heuristics that mis-filed a third of
  the HUD every frame and failed under stress input.
- `UIMask=1`: bias-current-color mask + zero vectors on bright HUD detail, as the fallback for the composite insertion.
- Debug views 6 (UI layer), 7 (HUD-less), 9 (vector field over the image).

## v1.0 (2026-08-29)

Real DLSS for the PC port of Metal Gear Solid 4 (Master Collection Vol. 2), as a ReShade add-on - first release.

- **DLAA / DLSS super-resolution** (preset K) created and evaluated every frame on the game's D3D12 path, with Halton
  camera jitter patched into the game's clip matrices.
- **Camera motion vectors** from depth and **per-object motion vectors** for animated characters and props via one
  stream-out draw per object (about 0.1 ms GPU).
- **DLSS 5 Neural Rendering compatible**: NGX-hooking add-ons (`renodx-dlss5.addon64`) are auto-detected; the add-on
  runs DLAA on the final image so NR works at full strength.
- **Frame generation** (Streamline DLSS-G: 2x / 3x / 4x or dynamic to a target fps, Reflex) with depth, vectors,
  HUD-less color and a replayed UI layer.
- **Dynamic resolution handled correctly**: the port renders its scene into a variable sub-rect and upscales it before
  the composite; the add-on brings depth and vectors to the full grid so DLSS, NR and DLSS-G stay aligned.
- Stable temporal history: resets only on real camera cuts. Overlay controls with GPU / CPU timing; debug views.
