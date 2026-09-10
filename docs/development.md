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
tools/scenes.csv             every launchable scene; scene_info.json everything measured and written about each
tools/thumbs/                one 960x540 frame per scene, zipped into the exe at build time
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
python tools\package_release.py --version 1.3.3
gh release create v1.3.3 --title v1.3.3 --notes-file release\notes-1.3.3.md release\mgs4-dlss-launcher.exe release\mgs4_dlss.addon64 release\mgs4_dlss.ini
```

`tools\package_release.py` assembles `release\` (git-ignored) with the three assets a release carries: `mgs4-dlss-launcher.exe` - the launcher built with `-Release`, one file with the add-on and ini inside it - and the loose `mgs4_dlss.addon64` and `mgs4_dlss.ini` for a manual install or an add-on-only update. It also writes `release\notes-<version>.md` for `gh release create --notes-file`: the install steps every release page carries, [release-install.md](release-install.md) (with the launcher, and by hand - keep it in step with [install.md](install.md)), followed by that version's section of [releases.md](releases.md).

Pushing a tag `v<version>` does the same on GitHub: `.github\workflows\release.yml` builds the add-on and the exe on a Windows runner and publishes the release with that version's notes. The `## v<version>` section in [releases.md](releases.md) has to exist before the tag is pushed.

## Recording and measuring the scenes

The whole catalog - every id `tools\scenes.csv` lists - is recorded, classified, deduplicated, named and given a
thumbnail by the tools in the *Measuring the catalog* table of [launcher.md](launcher.md#measuring-the-catalog):
`sweep_record.py` boots each id and records it through OBS (4K60 AV1) for as long as the scene needs, reading the
engine's own name for what it loaded from the add-on's `AssetTrace` line; `scene_identity.py`, `hud_read.py`,
`stage_names.py`, `apply_sweep.py`, `sweep_sheets.py`, `pick_thumbs.py` and `make_thumbs.py` do the rest, and
`verify_detection.py --cases` re-derives the reviewed verdicts from the recordings alone. How a scene is identified is
in [scene-identity.md](scene-identity.md).

## Stage rotation for testing

`tools\test_stages.ps1 -Stages "s00a00l,s02a50l_D1,s03a30l_D1" -HoldSeconds 40 [-MvVis] [-ObjectMV]` boots each stage/cutscene in turn (`tools\stages.md` lists the 75 stage ids from the executable and their `_D<n>` cutscene / `_<n>` section variants; `s02a50l_D1` is the Naomi lab scene), waits for the first 3D frame, holds, takes screenshots (and the motion-vector visualizer with `-MvVis`), and writes a per-stage log digest plus `summary.txt` (evaluations, DRS frames, last scene viewport, crashes) to `<MGS4_OUT>\stage_tests\<timestamp>\` (`-OutDir` overrides).
