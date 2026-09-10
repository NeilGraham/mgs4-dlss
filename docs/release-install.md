## Install

You need the Steam version of *Metal Gear Solid 4* (Master Collection Vol. 2) on **DirectX 12**, an NVIDIA RTX GPU
(RTX 40 series or newer for frame generation), **ReShade with add-on support** from [reshade.me](https://reshade.me/),
and two files from the [RenoDX Discord](https://discord.gg/renodx)'s Pinned Messages: the Streamline zip
(`DLSS310.8.0-Streamline2.13.zip` at the time of writing) with NVIDIA's DLSS / DLSS-G / DLSS-NR runtimes, and
`renodx-dlss.addon64` for DLSS 5 Neural Rendering. NVIDIA's and RenoDX's files are theirs and are not part of this
release. There are two ways to the same result; each ends with the game on DirectX 12 and these files next to `mgs4.exe`:

```
MGS4\
  dxgi.dll                ReShade with add-on support
  nvngx_dlss.dll, nvngx_dlssg.dll, nvngx_dlssnr.dll, sl.*.dll   from the Streamline zip
  renodx-dlss.addon64     DLSS 5 Neural Rendering (optional)
  mgs4_dlss.addon64       this add-on
  mgs4_dlss.ini           its settings
```

### With the launcher (`mgs4-dlss-launcher.exe`)

The exe below is the whole mod in one file: the launcher, with this release's `mgs4_dlss.addon64` and
`mgs4_dlss.ini` built in. Nothing to unzip or install; it runs on the .NET Framework that is part of Windows.

1. Download **`mgs4-dlss-launcher.exe`** and put it anywhere outside the game folder. Windows SmartScreen may warn
   about an unsigned download the first time: *More info*, then *Run anyway*.
2. Run it. It opens on the **Setup** tab, finds the game in your Steam libraries (**Change...** if it cannot) and
   lists the install as cards, in order. Each card has a button to its download and a drop area.
3. Follow the cards top to bottom, with the game closed:
   - **The game**: press **Set them for me**. The game goes on DirectX 12 with FXAA off, vsync off and the frame
     limiter at 60.
   - **ReShade**: download the setup marked *with full add-on support* from reshade.me and drop it on the tab. It is
     run for you against `mgs4.exe`.
   - **The Streamline zip**: drop it on the tab. What is needed is taken out of it.
   - **`renodx-dlss.addon64`**: drop it on the tab. Optional, but it is what DLSS 5 Neural Rendering needs.
   - **MGS4 DLSS**: press **Install the add-on**. The add-on and ini built into the exe are written next to
     `mgs4.exe`; an ini already there is kept.
4. Press **Re-check** (F5). Every row should be green. Start the game from the **Play** tab.

Already have an earlier version? Run the new exe and press **Install the add-on** again; it replaces the add-on and
keeps your ini. From v1.3.3 the launcher also checks for a newer release itself, once a day or from
**Check for updates** on the Setup tab, and updates itself with one button.

### By hand

The same steps without the launcher, using the two loose files below. With the game closed, in order:

1. **DirectX 12 in the game.** Options > Graphics > API = DirectX 12, FXAA off, vsync off, frame limiter 60. The
   add-on does nothing on the DirectX 11 backend.
2. **ReShade with add-on support.** Run the setup marked *with full add-on support* from reshade.me, point it at
   `MGS4\mgs4.exe`, choose the Direct3D 10/11/12 renderer and tick no shader packs. It installs as `MGS4\dxgi.dll`.
   A ReShade build without add-on support looks right on disk and silently loads nothing.
3. **The Streamline zip.** Extract everything in it straight into `MGS4\`, the folder with `mgs4.exe`.
4. **`renodx-dlss.addon64`** next to `mgs4.exe`. Optional; the add-on detects it. In RenoDX's own tab in the ReShade
   overlay set **Hook Method** to *Upscaled*, so Neural Rendering takes this add-on's DLAA output. Keep only one
   RenoDX DLSS add-on (older pins named it `renodx-dlss5.addon64`).
5. **`mgs4_dlss.addon64`** and **`mgs4_dlss.ini`** from the assets below, next to `mgs4.exe`, last. Updating from
   an earlier version: replace the add-on and keep your ini.
6. Start the game. `MGS4\logs\mgs4_dlss.log` records what happened; the ReShade overlay (Home key) has an
   **MGS4 DLSS** tab with the live controls.

Settings are in `MGS4\mgs4_dlss.ini`, documented key by key in
[configuration.md](https://github.com/NeilGraham/mgs4-dlss/blob/master/docs/configuration.md). The shipped ini is
the verified configuration: DLAA, dynamic frame generation to 240 fps (`FGTargetFps`; set it to your refresh rate,
or `FrameGen=0` on a 60 Hz display). The full walk-through, the verified versions and troubleshooting are in
[install.md](https://github.com/NeilGraham/mgs4-dlss/blob/master/docs/install.md); how the add-on works is in
[dlss-pipeline.md](https://github.com/NeilGraham/mgs4-dlss/blob/master/docs/dlss-pipeline.md).
