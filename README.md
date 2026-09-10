# MGS4 DLSS

Real DLSS for the PC port of *Metal Gear Solid 4* (Master Collection Vol. 2): DLAA and the upscaling modes, camera and
per-object motion vectors, frame generation, and full compatibility with **DLSS 5 Neural Rendering** - as a ReShade
add-on, with a launcher that installs it, checks it, edits every setting as a form, and starts the game at any of
its 400+ scenes.

**Current release: v1.3.3** - [download](https://github.com/NeilGraham/mgs4-dlss/releases) ·
[release notes](docs/releases.md) · [install](docs/install.md)

Supported DLSS features:

- **Super Resolution** - DLAA, Quality, Balanced, Performance, Ultra Performance (presets K and J)
- **Frame Generation** - DLSS-G / Multi-Frame Generation, 2x, 3x, 4x or dynamic to a target frame rate
- **NVIDIA Reflex** - low latency, with boost, through Streamline
- **DLSS 5 Neural Rendering** - through RenoDX's `renodx-dlss` add-on, which this add-on detects and feeds the final image

## How it works

MGS4 was never built for any of this - it is a Direct3D 12 port of a 2008 PS3 game with no motion vectors, no jitter and
no upscaling hooks. Everything above is reconstructed from the outside, without touching a single shader of the game:

- **Motion vectors the game never had.** Camera vectors are reprojected from the depth buffer; per-object vectors for
  every animated character and prop come from a stream-out of the game's own vertex shaders, run a second time.
- **Sub-pixel jitter patched into the game's own draw constants**, so DLSS gets the varying samples it needs to
  reconstruct detail rather than just smoothing what it is handed.
- **Depth of field re-applied after DLSS.** DLAA runs on the finished image so DLSS 5 Neural Rendering sees the whole
  picture, then the game's own DoF is re-run on top - out-of-focus characters keep the detail NR gave them.
- **A HUD-less color buffer and a separate UI layer** for frame generation, so the interpolator never sees the HUD and
  the HUD is never warped on a generated frame.
- **The port's quirks handled**: its dynamic resolution, the HUD drawn before the composite, the frozen backgrounds of
  the pause menu and the Codec, the Codec caller's 3D window - DLSS, NR and frame generation stay consistent through
  all of them.

Each stage is taken apart, feature by feature, in **[docs/dlss-pipeline.md](docs/dlss-pipeline.md)**.

## Get started

1. Download `mgs4-dlss-launcher.exe` from the [latest release](https://github.com/NeilGraham/mgs4-dlss/releases) and
   put it anywhere. It is the whole mod in one file: the launcher, with the add-on and its settings built in.
2. Run it (nothing to install - it runs on the .NET Framework that is part of Windows). It opens on **Setup**.
3. Follow the cards in order: put the game on DirectX 12 with one button, drop the ReShade setup, the Streamline zip
   and `renodx-dlss.addon64` on the tab, press **Install the add-on**, re-check, play.

**Play** opens on the three ways to start the game and nothing else. The scene list — every cutscene in MGS4, named
and with a frame of itself, in the order the story tells them — is behind **Show all scenes** in the bottom bar,
which asks first, once; the same button switches back, and Settings › **Launcher** › **Play tab** is the same
switch. Nothing on the default view spoils anything.

The full walk-through, the manual route and the versions this was verified on are in **[docs/install.md](docs/install.md)**.

## Requirements

- The Steam version of Metal Gear Solid 4 (Master Collection Vol. 2), set to **DirectX 12** in its graphics options.
- An NVIDIA RTX GPU. Frame generation needs an RTX 40 series or newer and a display faster than 60 Hz.
- [ReShade](https://reshade.me/) **with add-on support**.
- The DLSS / Streamline runtimes (the Streamline zip, `DLSS310.8.0-Streamline2.13.zip` at the time of writing) and,
  optionally, `renodx-dlss.addon64`, both from the
  [RenoDX Discord](https://discord.gg/renodx). They are NVIDIA's and RenoDX's files and are not redistributed here.
  The current RenoDX build (2026-09-05) needs its **Hook Method set to Upscaled** (the launcher's Settings tab,
  Neural Rendering, or RenoDX's own tab in the ReShade overlay); its default, Auto, puts NR on the whole backbuffer
  inside frame generation rather than on this add-on's DLAA output.

Verified on an RTX 5090 at 3840x2160, DLAA preset K, DLSS 5 NR, dynamic frame generation to 240 fps.

## Settings

Everything is in `MGS4\mgs4_dlss.ini`, edited live from the ReShade overlay (Home key, Add-ons tab) or as a form on
the launcher's Settings tab. The shipped ini is the verified configuration; the keys most people touch are
`Mode` (DLAA or an upscaling mode), `RenderRes` (upscale from exactly 1440p, 1080p or 720p instead) and `FrameGen` /
`FGTargetFps` (off, or dynamic to your refresh rate). Every key
is documented in **[docs/configuration.md](docs/configuration.md)**.

## Known limitations

- The game decides its own render scale from GPU load and can drop its 3D scene to 50 % under DLAA + NR + frame
  generation; the add-on renders correctly at any scale, but detail follows the game's choice. `Mode=Quality` removes
  the game's room to scale down and gives DLSS real samples to upscale.
- `Mode` changes need a restart (the game creates its render targets at startup).
- Frame generation on a 60 Hz output only alternates real and generated frames; use a faster display or virtual display.
- Aiming in third and first person is jittery with mouse look in the native game too; DLSS reconstructs what it is
  given and frame generation interpolates it faithfully.

## Known issues

Things that are wrong today and are being worked on. If you hit something not listed here, please
[open an issue](https://github.com/NeilGraham/mgs4-dlss/issues) with the scene and your `MGS4\logs\mgs4_dlss.log`.

- **Depth-of-field blur can flicker.** The separated blur layer occasionally flickers for a frame or two where the
  game's own focus changes.
- **HUD and menu elements can smear under frame generation.** With `FrameGen` on, UI drawn over the scene sometimes
  leaves artifacts while the camera or the player is moving. Frame generation off removes it.
- **Mission Briefing: the camera window flickers.** Geometry in the Nomad's CCTV / Metal Gear Mk. II window can
  flicker between frames.
- **Mission Briefing: vectors off the monitor.** The video call's 3D scene (Naomi's message, Campbell's) is rendered
  as its own view and shown on the Nomad's monitor; its motion vectors are still mapped across the whole main view
  rather than only where the monitor is on screen, so the picture around it can reproject wrongly while the call is up.
- **Cutscene to gameplay: the camera can end wider than gameplay's.** On some transitions the view stays at the
  cutscene's wider framing for a moment after control returns. This may be the port's own behaviour rather than the
  add-on's; it is still being checked.
- **Motion vectors from hidden geometry.** The per-object motion vectors are written for every vertex the game
  draws, including ones behind other surfaces of the same model. Where a hidden part moves differently from the
  surface in front of it - the gun models in first-person view are the clearest case - DLSS can be handed the
  hidden vertex's vector, and the surface can ghost or shimmer. `DebugMode=9` (or the debug key, below) shows it.
- **DLSS 5 Neural Rendering is not applied to pre-recorded video yet.** Some of the game's most intense sequences
  are played back as video rather than rendered by the engine (the launcher's scene list marks them *Video*). NR
  runs on what the engine renders, so those play as the port ships them.

## Documentation

| page | what it covers |
| --- | --- |
| [docs/install.md](docs/install.md) | installing with the launcher or by hand, verified versions, troubleshooting |
| [docs/configuration.md](docs/configuration.md) | every ini key, the overlay, DLSS modes, frame generation, debug views |
| [docs/launcher.md](docs/launcher.md) | the launcher: Play, Settings, Setup, the command line, how it is built |
| [docs/dlss-pipeline.md](docs/dlss-pipeline.md) | how the add-on works, feature by feature |
| [docs/renderer-notes.md](docs/renderer-notes.md) | what was reverse-engineered about the port, and the running log of findings |
| [docs/development.md](docs/development.md) | building from source, repo layout, paths, tools, packaging a release |
| [docs/releases.md](docs/releases.md) | release notes |

## Credits and licences

This project is released under the [MIT licence](LICENSE). The vendored pieces keep their own licences, listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

The add-on is built on the [ReShade](https://reshade.me/) add-on API (BSD-3), [MinHook](https://github.com/TsudaKageyu/minhook)
(BSD-2, vendored), the NVIDIA DLSS and Streamline SDK headers (NVIDIA's licences, in `third_party/`), and works alongside
[RenoDX](https://github.com/clshortfuse/renodx)'s DLSS 5 add-on. The launcher reads the game's own artwork, icon and
music from the machine it runs on; no file from the game is copied into this repository or its releases. The scene
thumbnails and the banner behind the header are screenshots taken while playing.
