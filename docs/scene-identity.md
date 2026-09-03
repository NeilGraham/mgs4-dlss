# Identifying a scene from the game, not from the picture

A handoff. This is where the work on "which of these 420 stage ids are the same scene, and what is each one?"
had got to, what has actually been measured, and which part is still guesswork.

## The problem

`tools/scenes.csv` lists ~420 stage ids the launcher can boot. Many are the same scene filed under several ids
(`s02a25l`, `s02a25l_D1`, `s02a25l_D2`), some crash, and none of them carry a name a player would recognise. The
catalog needs three things per id: does it boot, what kind of scene is it, and is it the same scene as another id.
`tools/scene_info.json` is where the answers go; `tools/stage_probe.csv` is where the measurements go.

Booting and kind are handled - see `tools/sweep_record.py` and `tools/classify_scene.py`. This document is about
**identity**: telling two recordings of one scene from two recordings of different scenes.

## What is already in the toolbox: comparing the video

`tools/find_duplicates.py` records each scene, reduces it to 2 frames a second of 64x36 greyscale, and scores a
pair at its best alignment. It works. Measured on the four ids that share the `s01a40l` stem, all recorded
through the same pipeline:

```
s01a40l   s01a40l_1     4.98   <- same scene, different save state
s01a40l   s01a40l_D2   24.09
s01a40l   s01a40l_2    30.12
s01a40l_1 s01a40l_D2   25.51
s01a40l_1 s01a40l_2    30.75
s01a40l_2 s01a40l_D2   29.77
```

`s01a40l` and `s01a40l_1` are the same Advent Palace lobby entry with a different loadout; `s01a40l_2` is the
Naomi interrogation and `s01a40l_D2` is Snake in the corridor. The duplicate scores 4.98 and the nearest
non-duplicate 24.09, so the separation is about 5x.

Things worth knowing before trusting it:

* **The window has to start after the loading screen.** The loading screen is not the same picture from one run
  to the next - the same stage came up behind a plain title card once and behind the act's own loading art the
  next time - so any of it inside the window is two recordings of one scene disagreeing about something that is
  not the scene. `scene_start()` finds it: the darkest thing in the head of the file is either the black gap
  before the scene fades up or the loading screen itself, and the scene begins on the frame after the last of it.
  A first attempt keyed on "where sustained motion begins" and failed, because one act's loading screen has a
  spinner.
* **The threshold (`MATCH = 6.0`) is the weak part.** A true duplicate came in at 4.98 - only 17% of headroom -
  while the nearest non-duplicate was at 24. Something near 12 would sit in the middle of a 5x gap. It has not
  been moved because one stem is not enough evidence.
* It needs a recording, so it costs a boot plus ~10 MB per 10 seconds, and it is only as good as the alignment.
* Ids are only compared when they share a stem (`s02a25l` for `s02a25l_D1`), so unrelated corridors cannot merge.

Keep it. It is the fallback for the one case the engine data below cannot settle.

## The study: what the engine will tell you directly

The add-on has a `FileTrace=1` option (`[DLSS]` section of `mgs4_dlss.ini`) that hooks `CreateFileW`/`CreateFileA`
and logs every open as `FILE open <path>` or `FILE MISS (n) <path>`. Turning it on and booting the same four ids
gives something much sharper than a video score.

**First, the negative result, because it is the one that will waste your time.** Do not compare file *sets*. All
four scenes open 1322 of the same 1322 files under the game folder - Jaccard 0.997 between completely different
scenes. MGS4 loads the whole stage's assets no matter which scene you enter, so bulk overlap says everything in
Advent Palace is the same thing. Roughly 1300 of those are hash-named meshes (`common\mesh\pc\0003a157.mdn`) and
textures, and they are identical across scenes.

**The signal is in the handful of named files.** The entire difference between two different cutscenes in the
same stage is one file:

```
s01a40l_1 vs s01a40l_2     only in _1:  common\bank\default\env_s01a40l_02.bank
                           only in _2:  ww\bank\default\e_d321.bank
                                        common\localization/demo/demo_en
                                        common\localization/movie/movie_en

s01a40l_1 vs s01a40l_D2    only in _1:  common\bank\default\env_s01a40l_02.bank
                           only in _D2: ww\bank\default\e_d320.bank
                                        common\localization/demo/demo_en
                                        common\localization/movie/movie_en

s01a40l_2 vs s01a40l_D2    only in _2:  ww\bank\default\e_d321.bank
                           only in _D2: ww\bank\default\e_d320.bank
```

Reading that:

* **`e_d###.bank` is the demo number** - MGS4's own id for a cutscene. `s01a40l_2` is demo 321, `s01a40l_D2` is
  demo 320. Two ids with the same demo number are literally the same cutscene. Exact equality, no threshold, no
  alignment, no recording.
* **`env_<stage>_NN.bank` marks a gameplay entry** and names the environment. `s01a40l` and `s01a40l_1` both load
  `env_s01a40l_02.bank` and no `e_d###.bank` at all, which is exactly the pair the video method calls duplicate.
* **`localization/demo` and `localization/movie` appear only for the cutscenes**, so their presence is a second,
  independent cutscene/gameplay discriminator to cross-check `classify_scene.py` against.
* Weapon banks (`e_wpn007_m4.bank`, `e_wpn003_rugermk2.bank`) track the **save state, not the scene** - they are
  what makes `s01a40l_1` carry an M4 and `s01a40l` a stun knife. Ignore them for identity.
* `e_rtr_s01a40l_vram.bank`, `e_s01a40l_event_hdd.bank` and friends are per-stage and shared by every scene in
  the stage, so they identify the stage but not the scene.

The proposed rule, then: **cutscene identity = the demo number; gameplay identity = the environment bank; video
comparison only to separate two gameplay entries that share an environment.** That last case is the one the
engine data provably cannot settle and video provably can.

## What has not been established

Be sceptical of the above in proportion to how little it has been tested. It rests on **four ids in one stage of
Act 1**. Specifically unknown:

1. **Does every cutscene load exactly one `e_d###.bank`?** If a scene chains two demos, or a demo is loaded
   lazily part-way through, the key is a set rather than a scalar and the rule needs restating.
2. **Do different gameplay spawn points share an environment bank?** `s01a40l_01` through `s01a40l_06` exist in
   the catalog and would settle it. If they do share one, the environment bank over-merges and video is doing
   real work, not just backup.
3. **Codec calls, briefings and Videos.** None were traced. `s02a25l_D3` is the known Video (45 engine draws
   against 873 for a real scene) - does it show up as a `movie` open with no `e_d###`? Briefings (`s10a20l` and
   the like) are their own thing again.
4. **Does the demo number map to anything published?** MGS4's demos are numbered, and if a public list exists,
   `e_d320` becomes a name rather than just a key - which would beat OCR for naming outright.
5. **Timing.** The trace has to run long enough. In the study `s01a40l` logged only 775 opens against 1320 for
   the others purely because its capture was cut short, and a truncated set is a subset, which will read as a
   perfect containment match if you use the wrong metric.
6. Whether the loading screen's own art (`common\additions\new_loading\...`) identifies the act or the scene. It
   was excluded as noise but never checked.

## How to reproduce the study

Turn the trace on by adding `FileTrace=1` under `[DLSS]` in the installed `mgs4_dlss.ini`, and restore the file
afterwards - never edit it while the game is running. Then per id:

```
mgs4-dlss-launcher.exe <stage_id> --hold 25 --max-minutes 3
```

Wait for `FIRST-3D-FRAME` in `<game>\logs\mgs4_dlss.log`, give it several more seconds so the scene's own assets
are all in, then kill `mgs4` and copy the log. Delete the log before each launch: the add-on truncates it on
start, but until the new process gets that far the file still holds the previous run.

The log is 0.3-0.7 MB per boot, mostly NVIDIA driver-profile polling
(`C:\ProgramData\NVIDIA Corporation\Drs\...`) repeated every few milliseconds - filter to paths under the game
folder first. A boot-and-trace is ~25 s per id with no recording, against ~25 s plus a video for the sweep, so a
full 420-id pass is a couple of hours and a few hundred MB of logs.

**The change worth making first:** have the add-on emit a single `SCENE-ASSET` line when it sees an `e_d###.bank`
or `env_*.bank` open, instead of logging every `CreateFile`. That turns the whole thing into one grep, drops the
log to a few KB, and means a dedup pass needs no video path at all. The hook already exists in
`dlss-addon/src/mgs4_dlss.cpp` (search `FileTrace`); it only needs a filter and a distinct log line.

## What was built on it (2026-09-03)

The change proposed above was made: `AssetTrace=1` in `mgs4_dlss.ini` gives one `SCENE-ASSET open f<frame> <path>`
line per named file under the game folder, each once, hash-named content filtered out - about fifty lines a boot.
Two things the study did not know:

* **The banks are opened with the `\\?\` extended-length prefix** (`\\?\D:\...\MGS4\ww\bank\default\E_d307.bank`)
  while the paks and localization tables are opened by relative path (`common\stage\...`). A filter that took a
  leading backslash to mean "not under the game folder" saw no bank at all.
* A streaming bank (`E_s01a00l_1.bank`) is reopened about a hundred times in ten seconds. The line is emitted once
  per distinct path; its frame number is the first open, which is the one that places it against `FIRST-3D-FRAME`.

`tools/sweep_record.py` reads the lines into `stage_probe.csv` as `demo` (demos opened within 600 frames of the
first 3D frame), `demo_late` (chained in afterwards), `env`, `movie` and `assets`, and records each id for a length
that suits what it turns out to be (10 s of gameplay, up to 5 min of cutscene, 20 s of anything else).
`tools/scene_identity.py` then merges ids that share a demo or a video outright, and hands a group that only
shares an environment to `find_duplicates.py`, which is the one case the engine cannot settle. Two ids that are
merged get `{"sameAs": primary}` in `scene_info.json`; every measured id gets a `fingerprint` string so the reason
can be read off the file.

**Checked against the picture (2026-09-03):** `s01a05l` and `s01a05l_D` both load demo 313 and were both recorded in
full before the early skip existed; their contact sheets match tile for tile from the first frame, so a shared demo
number is the same cutscene *from the same start*, not a chapter point inside it. Of the first 42 ids swept, the
engine key merged nine pairs outright and the video comparison one (`s01a40l` / `s01a40l_1`, score 5.37 in a
shared environment); the sweep now stops a repeat at 3 s, which is where five minutes of cutscene used to go.

## Where things are

| path | what it is |
|---|---|
| `tools/sweep_record.py` | boots and records every id through OBS; writes `tools/stage_probe.csv` |
| `tools/scene_identity.py` | identity from the engine fingerprint; `--apply` writes `sameAs` and `fingerprint` |
| `tools/find_duplicates.py` | video fingerprint dedup, now only called for gameplay entries that share an environment |
| `tools/scene_sheet.py` | a recording to contact sheets legible enough to read subtitles from |
| `tools/classify_scene.py` | Codec / cutscene / gameplay from one frame |
| `tools/apply_sweep.py` | folds measurements into `tools/scene_info.json` |
| `dlss-addon/src/mgs4_dlss.cpp` | `FileTrace`, `FIRST-3D-FRAME`, `SCENE-STATE` |
| `<MGS4_OUT>/sweep/video/<id>.mkv` | the recordings |
| `<MGS4_OUT>/sweep/<id>.addon.log` | the add-on log kept per id |

`sameAs` in `scene_info.json` is how two ids become one row: `{"sameAs": "s01a40l"}` replaces the entry entirely,
because the launcher reads it before anything else and a stale name left beside it would only mislead.
