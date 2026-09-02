# Development

## Building the add-on

Requirements: MSVC Build Tools, ReShade 6.8 installed as `MGS4\dxgi.dll`, the game set to DirectX 12, and `nvngx_dlss.dll` in `MGS4\` (copy `third_party/DLSS/lib/Windows_x86_64/rel/nvngx_dlss.dll` or let the NVIDIA app override supply it). `build.bat` finds the MSVC environment through `vswhere` and `fxc.exe` in the newest Windows 10 SDK; `MGS4_VCVARS` / `MGS4_FXC` in `config.ini` override that. The install script copies to whatever game folder is configured — see [Paths](#paths-configini).

```bat
dlss-addon\build.bat                       :: -> build\mgs4_dlss.addon64
```
```sh
sh dlss-addon/install.sh                    # -> <game>\MGS4\ (keeps an existing mgs4_dlss.ini)
```

## Building the launcher

`mgs4-dlss-launcher.bat` builds `mgs4-dlss-launcher.exe` on its first run with the C# compiler that ships with Windows (`launcher\build.ps1`); nothing has to be installed. That exe is not in the repo, because the icon inside it is read from the `mgs4.exe` on the building machine and that artwork is Konami's. `launcher\build.ps1 -Release` builds the one a release carries: the launcher's own icon (`tools\launcher_icon.ps1`), and `build\mgs4_dlss.addon64` + `dlss-addon\mgs4_dlss.ini` embedded as resources, so the single file installs the add-on by itself. How the app is put together is in [launcher.md](launcher.md).

## Layout

```
mgs4-dlss-launcher           the app, and the only entry point: Play / Settings / Setup
launcher/src, build.ps1      the app itself: Program, Paths, Checks, Install, Catalog, Runner, Ui/
config.example.ini           machine-local paths; copy to config.ini (git-ignored)
dlss-addon/                  src/mgs4_dlss.cpp, build.bat, install.sh, mgs4_dlss.ini (sample)
tools/paths.py|ps1|sh        where the game / the output folder live on this machine
tools/install_manifest.json  the file list, verified versions and download links the checks render
tools/scenes.csv             every launchable scene; labels.json the names, scene_info.json the corrections
third_party/minhook/         MinHook (BSD-2), vendored
third_party/reshade/         ReShade add-on API headers (v6.8.0, BSD-3)
third_party/DLSS/            NVIDIA DLSS SDK headers + nvsdk_ngx_s.lib (DLLs git-ignored)
docs/                        reverse-engineering notes and the DLSS plan
```

## Paths (`config.ini`)

The checkout can live anywhere; nothing in it assumes a path. Every script asks `tools/paths.py` (Python),
`tools/paths.ps1` (PowerShell) or `tools/paths.sh` (Git Bash) for the machine's paths, and all three resolve each
value the same way: **environment variable > `config.ini` in the repo root > auto-detection**.

| key | what | detected as |
| --- | --- | --- |
| `MGS4_DIR` | the folder holding `mgs4.exe` (the install root also works) | the library holding app 2492670, on any drive: Steam's own install names them all in `libraryfolders.vdf`, and the app manifest names the folder under `steamapps\common` |
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

## Packaging a release

```bat
dlss-addon\build.bat
python tools\package_release.py --version 1.3.0
gh release create v1.3.0 --title v1.3.0 --notes-file release\notes-1.3.0.md release\mgs4-dlss-launcher.exe release\mgs4_dlss.addon64 release\mgs4_dlss.ini
```

`tools\package_release.py` assembles `release\` (git-ignored) with the three assets a release carries: `mgs4-dlss-launcher.exe` - the launcher built with `-Release`, one file with the add-on and ini inside it - and the loose `mgs4_dlss.addon64` and `mgs4_dlss.ini` for a manual install or an add-on-only update. It also writes `release\notes-<version>.md`, the matching section of [releases.md](releases.md), for `gh release create --notes-file`.

Pushing a tag `v<version>` does the same on GitHub: `.github\workflows\release.yml` builds the add-on and the exe on a Windows runner and publishes the release with that version's notes. The `## v<version>` section in [releases.md](releases.md) has to exist before the tag is pushed.

## Recording the in-game cutscenes (4K60 AV1)

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

## Stage rotation for testing

`tools\test_stages.ps1 -Stages "s00a00l,s02a50l_D1,s03a30l_D1" -HoldSeconds 40 [-MvVis] [-ObjectMV]` boots each stage/cutscene in turn (`tools\stages.md` lists the 75 stage ids from the executable and their `_D<n>` cutscene / `_<n>` section variants; `s02a50l_D1` is the Naomi lab scene), waits for the first 3D frame, holds, takes screenshots (and the motion-vector visualizer with `-MvVis`), and writes a per-stage log digest plus `summary.txt` (evaluations, DRS frames, last scene viewport, crashes) to `<MGS4_OUT>\stage_tests\<timestamp>\` (`-OutDir` overrides).
