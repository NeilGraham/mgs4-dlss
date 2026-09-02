# Installing

Two ways to the same result. The launcher does the whole thing and tells you what is missing; the manual route is the
same steps by hand. Either way the game must be on **DirectX 12** and you need **ReShade with add-on support**, plus
two downloads from the RenoDX Discord that carry NVIDIA's DLSS and Streamline runtimes and the DLSS 5 Neural Rendering
add-on. None of those can be redistributed here.

## With the launcher (recommended)

1. Download **`mgs4_dlss_launcher.zip`** from the [latest release](https://github.com/NeilGraham/mgs4-dlss/releases)
   and unzip it anywhere (not inside the game folder).
2. Run **`mgs4-dlss-launcher.bat`**. The first run builds the app with the C# compiler that ships with Windows - a few
   seconds, nothing to install - and opens on the **Setup** tab.
3. Setup finds the game folder in your Steam libraries (Browse if it cannot) and lists the install in order, one card per
   download, each with a source button and a drop area:
   - **The game**: press **Set them for me** to put the game on DirectX 12 with FXAA off, vsync off and the frame
     limiter at 60 (it writes `mgs4.savedsettings`; the game must be closed).
   - **ReShade with add-on support**: download the setup marked *with full add-on support* from
     [reshade.me](https://reshade.me/) and drop it on the tab. It is run headless against `mgs4.exe`, installs as
     `dxgi.dll`, and takes no shader packs.
   - **`streamline.zip`** from the [RenoDX Discord](https://discord.gg/renodx), Pinned Messages: drop it on the tab. It
     carries `nvngx_dlss.dll`, `nvngx_dlssg.dll`, `nvngx_dlssnr.dll` and the `sl.*.dll` set, matched to each other.
   - **`renodx-dlss5.addon64`** from the same Pinned Messages: drop it on the tab. Optional, and the reason to bother -
     with it present DLAA runs on the final image so DLSS 5 Neural Rendering works at full strength.
   - **This add-on**: press **Install the add-on**. It copies the `mgs4_dlss.addon64` and `mgs4_dlss.ini` that ship in
     the zip next to `mgs4.exe` (an ini already there is kept).
4. Press **Re-check** (F5): every row should be green. Then start the game from the **Play** tab.

Setup's **Last run** card reads the logs of the previous game run and says whether NGX initialised, whether Neural
Rendering ran, and how many frames DLSS evaluated - the part a file list cannot tell you. `mgs4-dlss-launcher --report`
prints the same check as text for an issue.

## By hand

In order, with the game closed:

1. **The game on DirectX 12.** In the game: Options -> Graphics -> API = DirectX 12, FXAA off, vsync off, frame limiter
   60. On disk that is `api=dx12`, `enableFXAA=false`, `vsync=false`, `fpsLimiter=60` in
   `mgs4_savedata_win\<steamid>\mgs4\mgs4.savedsettings`. The add-on is a D3D12 add-on and does nothing on the D3D11
   backend; FXAA only blurs the DLSS input; the game's physics are tied to 60 fps and frame generation puts more frames
   on screen on top of that.
2. **ReShade with add-on support** from [reshade.me](https://reshade.me/): the setup marked *with full add-on
   support*. Run it, point it at `MGS4\mgs4.exe`, choose the Direct3D 10/11/12 renderer, tick no shader packs. It
   installs itself as `MGS4\dxgi.dll`. A ReShade build without add-on support looks correct on disk and silently loads
   nothing.
3. **`streamline.zip`** from the [RenoDX Discord](https://discord.gg/renodx) (Pinned Messages): extract everything in it
   straight into `MGS4\` (the folder with `mgs4.exe`). If the NVIDIA app's DLSS override is on for this game it supplies
   `nvngx_dlss.dll` instead, which is fine.
4. **`renodx-dlss5.addon64`** from the same Pinned Messages, next to `mgs4.exe`. Optional; auto-detected.
5. **`mgs4_dlss.addon64`** and **`mgs4_dlss.ini`** from the release, next to `mgs4.exe`, last, so the add-on loads with
   the rest in place. Keep an ini you already have: it holds your settings and the `InternalRes` the first run wrote.
6. Start the game. The first run writes the detected `InternalRes` to the ini. `MGS4\logs\mgs4_dlss.log` records the
   DLSS create / evaluate calls, NR hooking, frame generation and dynamic-resolution state; the ReShade overlay (Home
   key), Add-ons tab, has the live controls and GPU / CPU timing.

`steam_appid.txt` containing `2492670` next to `mgs4.exe` lets `mgs4.exe` be started directly (the launcher writes it
before a scene boot); it is harmless for Steam launches.

## Frame generation and your display

Frame generation only helps when the display (or the virtual display you stream from) refreshes faster than the game's
60 fps. The shipped ini has `FrameGen=4` (dynamic) with `FGTargetFps=240`: set `FGTargetFps` to your refresh rate, or
`FrameGen=0` on a 60 Hz output. Streamline is loaded only when `FrameGen` is non-zero at startup, so the first switch
from Off needs a restart.

## The setup this was verified on

| component | file in `MGS4\` | version |
| --- | --- | --- |
| ReShade with add-on support | `dxgi.dll` | 6.8.0 |
| DLSS / DLSS-G / DLSS NR | `nvngx_dlss.dll`, `nvngx_dlssg.dll`, `nvngx_dlssnr.dll` | 310.8.0 |
| Streamline | `sl.interposer.dll` and the other `sl.*.dll` | 2.13.0 (2.12.129 through the NVIDIA app's override) |
| DLSS 5 Neural Rendering add-on | `renodx-dlss5.addon64` | the RenoDX Discord's current build |

These are also the versions `tools\install_manifest.json` checks against, so change both together.

Settings that are not files this repo installs:

- **Game** (`mgs4.savedsettings`): `api=dx12`, `vsync=false`, `fpsLimiter=60`, `enableFXAA=false`, quality settings at 3.
- **RenoDX DLSS 5 NR**, in `MGS4\ReShade.ini` under `[RenoDX.DLSS5]` - the NR look the screenshots were taken with:
  `NeuralUplift=1`, `NRIntensity=2`, `NRStyle=2`, `NRLocalTone=1`, `NRSkinStructure=-1`, `NREnableUpscaling=0`
  (upscaling off: this add-on already runs DLAA on the final image, so NR only denoises / uplifts it). The launcher's
  Settings tab edits these too.

## If something is off

- **The scene looks soft, and the overlay says the game renders its 3D scene at 1920x1080.** That is the game's own
  dynamic resolution reacting to GPU load (DLAA + NR count toward it). `Mode=Quality` in the ini removes its room to
  scale down and gives DLSS real samples to upscale; see [configuration.md](configuration.md).
- **Nothing happens.** Check `MGS4\logs\mgs4_dlss.log` for `device created: api=...` - if it says the API is not D3D12
  the game is still on DirectX 11 - and for `NGX ... Init` and `CreateFeature ... -> 0x00000001`. `ReShade.log` must
  list `mgs4_dlss.addon64` as loaded; a ReShade without add-on support does not.
- **Neural Rendering does not show.** `ReShade.log` should contain `signed DLSSNR ... runtime initialized` followed by
  `feature 18 created ... for NR input`. Streamline's `DLSS-NR feature is not supported` warning in `mgs4_dlss.log`
  is about a Streamline plugin nothing here uses and says nothing about whether NR works.
- **A frame limiter mod is present.** Above 60 fps it works against this port; use frame generation instead. Setup
  warns about one.
