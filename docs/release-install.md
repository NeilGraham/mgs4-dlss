## Install

**You need:** the Steam *Metal Gear Solid 4* (Master Collection Vol. 2) on **DirectX 12**, an NVIDIA RTX GPU (40 series or
newer for frame generation), [ReShade](https://reshade.me/) **with full add-on support**, and from the
[RenoDX Discord](https://discord.gg/renodx)'s Pinned Messages the Streamline zip (`DLSS310.8.0-Streamline2.13.zip`)
and `renodx-dlss.addon64`. NVIDIA's and RenoDX's files are theirs and are not in this release.

### With the launcher

`mgs4-dlss-launcher.exe` is the whole mod in one file: the launcher with this release's add-on and ini built in.

1. Download it, put it anywhere outside the game folder, run it. (SmartScreen: *More info*, *Run anyway*.) It opens on
   **Setup** and finds the game.
2. With the game closed, go down the cards: **Set them for me** on the game card, drop the ReShade setup, the Streamline
   zip and `renodx-dlss.addon64` on the tab, press **Install the add-on**.
3. **Settings tab, Neural Rendering: Hook method = Upscaled.** The current RenoDX build defaults to Auto, which applies NR
   to the whole backbuffer instead of this add-on's DLAA output.
4. **Re-check** (F5): all green. Play from the **Play** tab.

Updating: run the new exe and press **Install the add-on**; your ini is kept. The launcher also checks for new
releases once a day and updates itself with one button.

### By hand

With the game closed, in order:

1. In the game: Options, Graphics, API = **DirectX 12**, FXAA off, vsync off, frame limiter 60.
2. Run the ReShade setup marked *with full add-on support* against `MGS4\mgs4.exe` (Direct3D 10/11/12, no shader
   packs). It installs as `MGS4\dxgi.dll`.
3. Extract everything in the Streamline zip into `MGS4\` (the folder with `mgs4.exe`).
4. Put `renodx-dlss.addon64` next to `mgs4.exe` (keep only one RenoDX DLSS add-on), then set its **Hook Method** to
   *Upscaled*: in the ReShade overlay (Home), **RenoDX DLSS** tab, Options Mode **DLSS-NR**. Or with the game closed,
   `DirectNeuralRenderingHookPoint=2` under `[RENODX-DLSS]` in `MGS4\ReShade.ini`.
5. Put `mgs4_dlss.addon64` and `mgs4_dlss.ini` from the assets below next to `mgs4.exe`. Updating: replace the add-on,
   keep your ini.
6. Start the game. The ReShade overlay (Home) has an **MGS4 DLSS** tab with the live controls; `MGS4\logs\mgs4_dlss.log`
   records what happened.

Every key is documented in [configuration.md](https://github.com/NeilGraham/mgs4-dlss/blob/master/docs/configuration.md);
the full walk-through, verified versions and troubleshooting are in
[install.md](https://github.com/NeilGraham/mgs4-dlss/blob/master/docs/install.md).
