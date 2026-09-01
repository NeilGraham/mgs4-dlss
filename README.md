# mgs4-dlss

**v1.1.1 (2026-08-30)** — DLAA/DLSS with camera jitter, camera + per-object motion vectors, DLSS 5 Neural Rendering compatibility, DLSS-G frame generation (2x/3x/4x/dynamic), correct handling of the port's dynamic resolution, DLSS inserted before the HUD (no HUD ghosting, clean HUD-less/UI layers for frame generation), and the pause-menu / Codec backgrounds kept as the DLSS (+NR) image. Download the add-on and the ini from the [releases](https://github.com/NeilGraham/mgs4-dlss/releases); install steps below.

Real DLSS (DLAA and the upscaling modes) for the PC port of *Metal Gear Solid 4* (Master Collection Vol. 2), built as a ReShade add-on. The NGX feature it creates can be hooked by NGX-based add-ons — **directly compatible with the DLSS 5 Neural Rendering add-on (`renodx-dlss5.addon64`)**, which is auto-detected: with it loaded, DLAA runs on the final image so NR works at full strength.

## Status

| Step | State |
|---|---|
| Route A — run the port on bgfx's built-in Direct3D 12 backend | **Done** — the game has a native renderer option (Options -> Graphics, `api=dx12` in `mgs4_savedata_win\<steamid>\mgs4\mgs4.savedsettings`) |
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

`mgs4-dlss-launcher` walks through this on its Setup tab, and takes most of these files by drag-and-drop. In order:

1. In the game: **Options -> Graphics -> API = DirectX 12** (`api=dx12` in `mgs4_savedata_win\<steamid>\mgs4\mgs4.savedsettings`), FXAA off, frame limiter 60 (`fpsLimiter=60`), vsync off.
2. **ReShade with add-on support** ([reshade.me](https://reshade.me/)): download the setup marked *with full add-on support* and drop it on the launcher's Setup tab — it is run headless against `MGS4\mgs4.exe`, installs itself as `MGS4\dxgi.dll`, and takes no shader packs (none are used). By hand: run it, point it at `MGS4\mgs4.exe`, choose the Direct3D 10/11/12 renderer, tick no shader packs.
3. **`streamline.zip`** from the [RenoDX Discord](https://discord.gg/renodx), under Pinned Messages: extract everything in it straight into `MGS4\`. One zip carries `nvngx_dlss.dll`, `nvngx_dlssg.dll`, `nvngx_dlssnr.dll` and the `sl.*.dll` set, already matched to each other — the NVIDIA and Streamline SDKs are not needed separately. (If the NVIDIA app's DLSS override is on for this game it supplies `nvngx_dlss.dll` instead.)
4. **`renodx-dlss5.addon64`** from the same Pinned Messages, next to `mgs4.exe`. Optional, and the reason to bother: with it present this add-on runs DLAA on the final image so Neural Rendering works at full strength. It is auto-detected — nothing to configure.
5. **`mgs4_dlss.addon64`** — press **Install the add-on** on the launcher's Setup tab, or run `mgs4-dlss-launcher --install-addon`. The add-on ships with the launcher, so there is nothing to fetch: it copies the add-on next to `mgs4.exe`, last, and an `mgs4_dlss.ini` alongside it if the game folder has none. Dropping a pair from the [releases](https://github.com/NeilGraham/mgs4-dlss/releases) in by hand still works.
6. Start the game; the first run writes the detected `InternalRes` to the ini. `MGS4\logs\mgs4_dlss.log` records the DLSS create/evaluate calls, NR hooking, frame generation and dynamic-resolution state; the ReShade overlay's Add-ons tab has live controls and GPU/CPU timing.

Frame generation only helps when the display (or the virtual display you stream from) refreshes faster than the game's 60 fps — set `FGTargetFps` to your refresh rate (the shipped ini uses `FrameGen=4` + `FGTargetFps=240`); on a 60 Hz output set `FrameGen=0`.

Run **`mgs4-dlss-launcher`** at any point: its Install tab says which of those pieces are actually in place, and its Play tab starts the game or any single scene — see below.

## The app (`mgs4-dlss-launcher`)

One window for the whole add-on, and the same things as a command line. It is a C# WPF program built from
`launcher\src` by the compiler that ships with Windows, so it still needs nothing installed and still runs straight
out of an unzipped release.

| tab | what it is for |
| --- | --- |
| **Play** | start the game, or any one of the 423 launchable scenes, with the automation the tests use |
| **Settings** | every setting as a form: the add-on's `mgs4_dlss.ini` and the game's own `mgs4.savedsettings` |
| **Setup** | the game folder, which files are in place, what the settings say, what the add-on did on its last run |

```bat
mgs4-dlss-launcher                         :: the window
mgs4-dlss-launcher s02a50l_D1              :: boot that scene and exit
mgs4-dlss-launcher --main                  :: MGS4's own menu, past the Master Collection screen
mgs4-dlss-launcher --list naomi            :: what can be launched
mgs4-dlss-launcher --setup                 :: the window, opened on Setup (game folder + install check)
mgs4-dlss-launcher --report                :: the install check as text, for pasting into an issue
mgs4-dlss-launcher --install-addon         :: put the add-on that ships here next to mgs4.exe
mgs4-dlss-launcher <id> --shortcut <file>  :: save that scene, with its run options, as a .lnk
mgs4-dlss-launcher --set FrameGen=0        :: write ini keys without opening anything
mgs4-dlss-launcher --help                  :: every option
```

**Two files, one program.** `mgs4-dlss-launcher.bat` is what a fresh clone has and always works: on its first run
it builds `mgs4-dlss-launcher.exe`, once, and every run after that just starts it. The exe is not in the repo,
because the icon inside it is read from the `mgs4.exe` on this machine and that artwork is Konami's. To rebuild by
hand:

```bat
powershell -ExecutionPolicy Bypass -File launcher\build.ps1
```

**One exe, both jobs.** It is a Windows-subsystem program, so a double-click never flashes a console the way a
`.bat` must - and it still behaves like a command: cmd waits for it, passes its handles through, and
`mgs4-dlss-launcher --report > out.txt` catches what it writes. The thing that makes both true is a single rule in
`Program.KeepCallersOutput`: **attach to the parent's console only when the caller left nothing usable, and put the
caller's own handles back afterwards.** `AttachConsole` replaces the standard handles with the console's, which
silently throws away a redirect - measured as stdout arriving on the file (`FILE_TYPE_DISK`) and leaving on a
console (`FILE_TYPE_CHAR`), with the file catching nothing. A console twin was built for one commit to work around
that, before the cause was understood; it is gone.

**Why C# and not PowerShell.** It was a PowerShell app until 2026-08-31, and the port kept every flag, every file
it reads and writes, and the window's own XAML. What changed is what PowerShell cost: **1.66 s** to put the window
up against **0.34 s** (three runs each, median), and a scripting engine in the per-frame path of every animation -
the scroll easing ran a script block per frame there and native code here. `launcher\src` is laid out as

| file | what it holds |
| --- | --- |
| `Paths.cs` | the game folder and the Steam libraries, resolved in the order `tools\paths.*` resolve them |
| `Checks.cs` | the install check and its text report, still reading `tools\install_manifest.json` |
| `Install.cs` | the bundled add-on, `steam_appid.txt`, drag-and-drop, the headless ReShade setup |
| `Catalogue.cs` | the scene list, in story order |
| `Runner.cs` | the scene run: boot, press through the prompts, tap Cross, end on gameplay |
| `Ui\` | the window: shell, Play, Settings, Setup, artwork, smooth scrolling |

There is no MSBuild and no compiled XAML: `csc` alone cannot produce BAML, so `Window.xaml` is an embedded resource
loaded with `XamlReader` at startup - the same markup the PowerShell app used, lifted whole. Two things that cost a
debugging round each, kept here so they are not rediscovered: `XamlReader` returns a fully formed `Window` and it
has to be used as-is - re-parenting its content into a `Window` subclass takes the process down with an access
violation before anything is drawn; and `Scene` and `SceneRow` are `public` because WPF's binding engine reflects
over public members of public types only.

**The window wears the game's own artwork** when Steam has it cached on this machine: the Metal Gear Solid 4 logo
in place of the title, the key art behind the header band (mirrored, so Snake sits on the right where there is
nothing to read), and the game's icon on the window and the taskbar. All of it is read at runtime from
`Steam\appcache\librarycache` and from `mgs4.exe` itself — none of it is in this repo, because it is Konami's
artwork and it is already on the machine of anyone who owns the game. Every piece falls back to plain text if it is
not there.

**Scrolling.** WPF gives a wheel notch three "lines" and applies it in one jump - and in a `ListBox` a "line" is a
whole row, so the scene list moved three scenes at a time. Three things fix that:

- the scene list scrolls **by pixel** (`VirtualizingPanel.ScrollUnit`), keeping recycling virtualisation for its
  400+ rows - without it an offset of 72 would mean 72 rows rather than 72 pixels;
- a **wheel notch** moves a target offset 72 px and the real offset eases toward it, so a flick glides and repeated
  notches accumulate instead of fighting each other. The step is taken on `CompositionTarget.Rendering` - once per
  frame the compositor is about to draw - and the amount depends on how long the frame took (a 70 ms time
  constant), so a dropped frame costs no distance. A `DispatcherTimer` was the obvious way to do this and the wrong
  one: it runs at `Background` priority, which measured 42 ticks a second with stalls to 147 ms, and every stall is
  a stutter;
- a **precision touchpad** reports the finger continuously in deltas well under a notch. That stream is already
  smooth, and easing it would only put lag between the finger and the page, so anything under a full notch is
  applied as it arrives, one to one.

`ScrollStep` and `ScrollTau` at the top of that block are the two numbers worth touching.

Play, Settings and Setup are tabs of the one window. The **first** run
opens on Setup, because the first thing anyone needs to know is whether the pieces are in place; after that it
opens on Play. `--setup` and `--settings` override that at any time.

The window opens even when no MGS4 install can be found — it starts on Setup and says which folder it looked in,
which is the one case where a tool that needs the game folder still has to be useful.

### Play

The list is `tools\scenes.csv` (the stage table) with names from `tools\labels.json` and the corrections in
`tools\scene_info.json`, plus the two ways of starting the game itself. Pick one, tick what should happen while it
runs, press Launch. The panel shows the command line that does the same thing, so anything set up in the window can
be pasted into a terminal or put in a shortcut.

Scenes are grouped by act and every group starts collapsed, so the window opens as a short list of acts rather than
four hundred rows; click a header to open one. The order is story order — Acts 1 to 5, then the epilogue — which the
stage ids do not give you: `s00` is the Big Boss material at the very end of the game, and `s10` / `s20` / `s30` are
the briefings and closing scenes that belong between and after the acts. Typing in the search box opens every group
that matched.

**`tools\scene_info.json`** is where that lives: per stage id, which act it really belongs to, where it sorts inside
it, what kind it is, a name and a one-line description of what you actually see. The stage table can say a scene
exists; only booting it says what it is, so everything in that file was checked by launching it.

**Each row says what kind of scene it is.** A coloured badge sits between the stage id and the name, in the same
colour families the rest of the window uses: green **Gameplay** for what you play, violet **Cutscene** for what you
watch, amber **Briefing** for the Nomad briefings, blue **Stage** for a plain stage boot, sky **Start** for the two
entries that start the game. The badge column is a fixed width, so the names line up down the list.

**Double-click a scene to start it** - the same thing the Launch button does with the options as they are
ticked. Act headers and the star are not double-clickable: their clicks are handled before the list sees
them, so neither can pair into one.

**Favourites.** Every scene row has a star on the right: click the outline to add it, click the filled one to take
it out. It is saved the moment it is clicked - not when the window closes - so a scene starred and a window shut
straight after keeps it. The list of ids lives with the rest of the window's memory in
`%LOCALAPPDATA%\mgs4-dlss-launcher\launcher.json`.

**The tab comes back as it was left.** The chips still ticked, the acts still open or closed, the scene still
picked, the stars still starred - all of it in
`%LOCALAPPDATA%\mgs4-dlss-launcher\launcher.json`, written when a chip, an act or a star changes as well as when
the window closes, so a launcher that is killed rather than closed does not lose it. The one thing not kept is the
search box: a query is a thing you are doing, not a thing you have set. A first run, with nothing saved yet, opens
the act the picked scene is in so it can be seen - after that the acts are yours, and a filter is never cleared to
bring a row into view.

**Favourites** is also the first filter chip, and works like the others: nothing ticked shows everything, ticking
chips shows the union of what they cover. **Start the game** is no longer a chip - the two entries it covered are
an act of their own at the top of the list, so filtering for them only ever hid everything else.

The filter row under the search box is a checklist rather than a dropdown: nothing ticked shows everything (bar the
known-broken ids), and ticking chips shows the union of what they cover - "Mission briefings" and "Gameplay"
together lists both, not their overlap.

- **Mission briefings** are their own kind, sorted to the top of the act they lead into — the Nomad briefing before
  Act 2 sits above Act 2's own scenes. The Cutscenes filter includes them; "Mission briefings" shows only those.
- **The same scene under two ids** is one row. `s10a20l` and `s10a20l_D1` start the same thing, as do `s10a40l` and
  `s10a40l_D2`, `s20a00l` and `s20a00l_D1`, `s20a00l_D3` and `s20a10l`, `s30a00l` and `s30a00l_D`, `s30a10l` and
  `s30a00l_D2`. The panel offers both ids so you can boot either, in case they differ in something not visible at
  the first frame.
- **Starting the game** is `s10a10l` - the normal boot with the Master Collection launcher skipped: pre-menu
  credits, PRESS START, then the menu. `--main` (`mgs4.exe --skip-to-main-menu`) drops straight onto the menu
  selection with the credits and PRESS START already gone, which is quick for testing but is not how the game
  starts, and it loses the device on most launches with frame generation on - so it is hidden with the rest of the
  known-broken ids, and the panel says so when you pick it.
- **Inside a stage, the cutscenes come before the gameplay**: `s01a10l`, then `s01a10l_D1` and `_D2`, then
  `s01a10l_01` onwards. Sorting on the id alone puts `_00` first, because a digit sorts before a letter, which is
  backwards - the demo of a stage plays before the sections it introduces. `_D10` also sorts after `_D9` rather
  than after `_D1`.
- **Ids that crash or come up black** are out of the list: every one ending in a
  single digit — `_0`, `_1`, ... `_9`, 102 of them. The two-digit sections (`_00`, `_11`) are the ones that work,
  and `_D2` is a cutscene rather than a section: the digit has to be the whole suffix after the underscore for the
  test to fire. The "Known broken" filter shows them if you want them anyway, and searching for one by id still
  finds it.
- `s00a00l` and `s00a00l_D` are the cemetery scene, which plays inside Act 1 rather than with the rest of `s00`, so
  they sit in Act 1 after the `s01a00l` entries (`sortAs` in the data file puts them there).

The three game-start entries take none of this: a menu has no boot prompts to press through and no first 3D frame
to wait for, and tapping Cross on it would just start a new game, so the launcher starts the game and leaves it
alone. The options below are greyed out while one of them is picked.

What can be ticked (all of it also works from the command line):

- **Skip the boot prompts** (`--advance`, on by default) — taps **Enter** until the add-on log reports the first 3D
  frame, which is what gets a `--stage` boot past the auto-save notice, the "press any button" screen and the load.
  On its own it stays on the keyboard: those prompts take any button, so nothing needs a controller, and none is
  created. Tick **Keep pressing X** as well and the tapping moves to the pad's Cross, because that is the one the
  flashback prompts want. `--press-key <name>` taps that key instead of either.
- **Keep pressing X** (`--mash-x`) — a virtual DualShock 4 taps **Cross** about six times a second for the whole
  scene. It is the only thing that creates one: the pad is made when such a run starts and removed when it ends,
  and opening the window does not make one at all. This is what makes MGS4's in-cutscene **flashback** prompts fire; a keyboard Enter gets past the boot
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

**Create shortcut** saves the scene you have picked, *with the options you have ticked*, as a `.lnk` wherever you
choose - so "the Naomi lab cutscene, tapping X the whole way, closing when gameplay starts" becomes one
double-click. The same thing from a terminal:

```bat
mgs4-dlss-launcher s02a50l_D1 --mash-x --end-on-gameplay --shortcut "%USERPROFILE%\Desktop\Naomi lab.lnk"
```

The shortcut holds `mgs4-dlss-launcher.exe`'s **absolute path**, which is the one thing that can break it: move or
re-clone the checkout and it points at a folder that is no longer there. Make a new one rather than editing it.

The port's own command line, for reference (read out of `mgs4.exe`): `--stage <id>`, `--skip-to-main-menu`,
`--res_width` / `--res_height`, `--windowing`, `--screen_index`, `--lang`, `--region`, `--input_device`, `--rumble`,
`--next`, `--forcedlcon`. **`--skip-to-main-menu` is what a bare `mgs4.exe` used to do** — without it the port stops
on the Master Collection screen first, so a plain "run the game" shortcut needs that argument. `--windowing` takes
`windowed` / `full_borderless` but the port has been observed ignoring it.

### Settings

**Every group says whose setting it is**, in a badge left of its title: **Game**, **MGS4 DLSS**, **RenoDX**, and
**Debug** alongside the add-on's on the diagnostics, which cost frames and are not for normal play. The game's
groups come first, because they are the ones that have to be right before any of the rest matters. Each card names
the file - or files - its rows are written to.

- **Display** and **Quality** — *the game's own*, out of `mgs4_savedata_win\<steamid>\mgs4\mgs4.savedsettings`, the
  same file its in-game menu writes: renderer (`api`), display index, vsync, frame limiter, the four quality levels
  and FXAA. Four of them are what the add-on needs set a particular way, and the Setup tab keeps its one-click
  **Set them for me** for exactly those four.
- **Display** also holds two keys that are this app's rather than the game's, in `config.ini`: **Resolution**
  (`MGS4_RES`, two boxes) and **Mode** (`MGS4_WINDOWING`). The game has no setting for either - it takes
  `--res_width` / `--res_height` / `--windowing` on the command line - so these are what a scene boot passes it.
  Empty means "let the game choose". The Play tab's own resolution box wins for the run it is ticked on, and
  `--res` on the command line wins over both. **Mode is the unreliable one**: the port has been seen ignoring
  `--windowing` on a `--stage` boot, and the Master Collection launcher's own display settings are where the window
  mode really lives.
- **DLSS**, **Frame generation**, **Image**, **Diagnostics** — the add-on's own `MGS4\mgs4_dlss.ini`.
- **Neural Rendering** — RenoDX's, out of `[RenoDX.DLSS5]` in `MGS4\ReShade.ini`: neural uplift, intensity, style,
  local tone and structure, skin structure, and NR upscaling. They are read from that file and written back into
  that section, never anywhere else in it - it is a long file full of other people's sections. Not this project's
  settings and not its defaults; the values this add-on was verified with are in "the setup this was verified on".

The files spell booleans differently — the add-on and RenoDX write `1` / `0`, the game writes `true` / `false` — so
each key carries its own spelling and a tick box writes whichever its file expects. **Save settings** is greyed until something is actually
changed - it compares each control against what its file said when the form was built - and then writes all of
them at once and says what went where (`written: 23 to mgs4_dlss.ini, 9 to mgs4.savedsettings`); a group whose file
does not exist yet shows *not there* until whatever writes it has run.

`mgs4-dlss-launcher --settings` prints every group the way the tab shows them, naming each group's files. `--set
Key=Value` writes without opening a window and **routes each key to the file, and the section, that holds it**, so
`--set api=dx12 --set MGS4_RES=3840x2160 --set NRIntensity=3 --set Sharpness=42` writes one value to each of four
files; a key no group knows goes to the add-on's ini, which is where every key used to go.

Saving is blocked while the game is running, because every one of these files has an owner then: the add-on rewrites
`mgs4_dlss.ini` through the Windows profile API, whose cache will quietly undo an outside edit, and the game
rewrites `mgs4.savedsettings` when it exits. While the game *is* up, the add-on's keys are live in ReShade's
overlay, Add-ons tab, and the game's are live in its own options menu.

### Setup

Most of the files this add-on needs cannot be shipped here: NVIDIA's DLSS runtimes, the Streamline runtime and
ReShade all have to be fetched from their own projects, so an install is assembled by hand and it is easy to end up
one file short. The Setup tab opens with the verdict, then the game folder everything is checked against — the path, where it came
from, a Browse button that records your choice as `MGS4_DIR` in `config.ini`, and Detect to stop pinning one and
search the Steam libraries again. **A game on another drive needs nothing special**: Steam's own install - found
from the registry, normally on C: - carries `steamapps\libraryfolders.vdf`, and that file names every library on
every drive, so a D: install is read out of the C: client. The PowerShell and Python resolvers additionally try
each drive in letter order for the handful of places a library sits, which catches one Steam has forgotten.
When no `mgs4.exe` turns up, the card **names the libraries it looked in** - if the drive holding the install is
not among them, that library is not registered with Steam and Browse is the way in. **Re-check** (or F5) re-runs everything with the window open, so it can be left up on a
second monitor while files are dropped into the game folder, and **Copy report** puts the text form on the clipboard.

What it reports, beyond whether a file exists:

- **The install, in the order you do it.** Five numbered groups, each one download and one instruction, with the
  source as a button and every row a path relative to the game folder:

  1. **The game** — Steam. Set Graphics -> API to DirectX 12, FXAA off, vsync off, limiter 60.
  2. **ReShade, with add-on support** — [reshade.me](https://reshade.me/). Download the setup marked *with full
     add-on support* and drop it on the tab: it is run **headless against `mgs4.exe`**, installs as `dxgi.dll`, and
     takes no shader packs — none are used. Running it by hand still works.
  3. **DLSS and Streamline runtimes** — the [RenoDX Discord](https://discord.gg/renodx), Pinned Messages:
     **`streamline.zip`**, extracted straight into the folder with `mgs4.exe`. That one zip carries `nvngx_dlss.dll`,
     `nvngx_dlssg.dll`, `nvngx_dlssnr.dll` and the `sl.*.dll` set already matched to each other — the NVIDIA and
     Streamline SDKs are not needed separately.
  4. **DLSS 5 Neural Rendering add-on** — the same Pinned Messages: the newest `renodx-dlss5.addon64`, next to
     `mgs4.exe`. Nothing to configure; this add-on notices it.
  5. **This add-on** — `mgs4_dlss.addon64` next to `mgs4.exe`, last, so it loads with the rest already in place.
     This is the one group with an **Install the add-on** button, because it is the one group that ships with the
     app: see below.

  **A frame limiter is not part of the install.** A third-party one (MGSFPSUnlock) is a separate mod, and above
  60 fps it works against this port - its physics are tied to 60. The way to put more frames on screen with this
  add-on is **frame generation**: the game keeps running at 60 and the generated frames come on top, which is what
  `FrameGen` + `FGTargetFps` do. Setup still *checks* for one, because a copy left over from an earlier setup is
  worth knowing about: above 60 it gets a warning row.

  **Drag and drop does most of it.** The Setup tab has a drop area naming exactly what it takes —
  `ReShade_Setup_*.exe`, `streamline.zip`, `renodx-dlss5.addon64`, `mgs4_dlss.addon64`, read
  straight out of the manifest so the two cannot drift. Drop any of them anywhere on the tab: archives are unpacked
  into the game folder keeping
  the folders that matter (anything the zip already put in `scripts\`, and any `.asi`, lands in `scripts\`), the
  ReShade setup is **run for you** rather than merely started — `ReShade_Setup_*.exe "<game>\mgs4.exe" --headless
  --api dxgi`, which is its own unattended mode, so it picks no target, no API and no shader packs and is done in
  under a second — and the check re-runs. Only files the install actually uses are written —
  anything else in a dropped archive is left alone and named in the status line.

  **The add-on installs itself.** Every other group is somebody else's download, but `mgs4_dlss.addon64` and
  `mgs4_dlss.ini` are what this app is *for*, so requiring them to be fetched separately made no sense. Group 5 has
  an **Install the add-on** button that copies the pair the app ships with into the game folder: a release carries
  them next to the app, a source checkout has the built add-on in `build\` and the sample ini in `dlss-addon\`.
  `mgs4-dlss-launcher --install-addon` does the same from a terminal. Two rules it keeps: an `mgs4_dlss.ini`
  already in the game folder is never overwritten (it holds your settings, and the `InternalRes` the first run
  wrote), and the game has to be closed, because ReShade holds the loaded `.addon64` open. The button says
  *Reinstall* once the add-on is there, and the drop area still takes a newer pair from a release.

- **Settings** that the Settings tab does not cover, plus anything that reads as wrong. The game's own options
  (`api=dx12`, vsync, the frame limiter, FXAA) — with a **Set them for me** button that writes all four into
  `mgs4.savedsettings`, leaving the rest of that file alone, and refuses while the game is running because the game
  owns it then. Also whether ReShade has the add-on disabled, whether the virtual controller the Play tab wants is
  there — and `FrameGen` checked against the display's actual refresh rate, which
  is the one add-on key a form cannot judge on its own. The add-on's own keys live on the Settings tab and only
  appear here when they are a problem (`Enabled=0`, a diagnostic left on), so this is not a second read-only copy
  of that tab.
- **Last run**, parsed from `logs\mgs4_dlss.log` and `ReShade.log`: whether NGX initialised, whether DLSS came from
  the local DLL or the driver override, whether Neural Rendering actually ran, the insertion point, the Streamline
  and driver versions, and how many frames DLSS evaluated. This is the part a file list cannot tell you — a
  ReShade build **without** add-on support looks perfectly correct on disk and silently loads nothing.

  Neural Rendering is read from RenoDX's own lines in `ReShade.log` (`signed DLSSNR ... runtime initialized`, then
  `feature 18 created ... for NR input`), because that is where NR actually happens: RenoDX hooks NGX directly
  (`EnableHooks=2: NGX hooks only, Streamline modules left unpatched`). Streamline's log carries a
  `DLSS-NR feature is not supported` warning on every run, immediately followed by
  `Ignoring plugin 'sl.dlss_nr' since it is was not requested by the host` — this add-on asks Streamline for
  `DLSS_G`, `Reflex` and `PCL` only (`dlss-addon/src/fg.cpp`), so that warning is about a plugin nothing here uses
  and says nothing about whether NR works.

The file list, the verified versions and the download links are one data file, `tools\install_manifest.json`; the
checks in `launcher\src\Checks.cs` only render it.

The shipped ini is the configuration v1.1.1 was verified with: DLAA preset K at 3840x2160, jitter + camera and object motion vectors, DLSS 5 NR through `renodx-dlss5`, dynamic-resolution handling, depth of field re-applied after NR (`PostDof=1`) and dynamic frame generation to 240 fps. The diagnostic keys at the bottom (`TraceFreeze`, `TraceFrames`, `Probe`, `DumpShaders`) are off; turning them on costs frames.

### The setup this was verified on

The add-on and the ASI come out of this repo, but the rest of the stack lives in the game folder and is worth
recording, because "DLSS 5 NR is on" is not one setting. Versions the v1.1.1 numbers were taken with (the same
versions `tools\install_manifest.json` checks against, so change both together):

| component | file in `MGS4\` | version |
| --- | --- | --- |
| ReShade with add-on support | `dxgi.dll` | 6.8.0 |
| DLSS / DLSS-G / DLSS NR | `nvngx_dlss.dll`, `nvngx_dlssg.dll`, `nvngx_dlssnr.dll` | 310.8.0 |
| Streamline | `sl.interposer.dll` and the other `sl.*.dll` | 2.13.0 |
| DLSS 5 Neural Rendering add-on | `renodx-dlss5.addon64` | - |

Settings that are not files this repo installs:

- **Game** (`mgs4_savedata_win\<steamid>\mgs4\mgs4.savedsettings`): `api=dx12`, `vsync=false`, `fpsLimiter=60`,
  `enableFXAA=false`, quality settings at 3.
- **RenoDX DLSS 5 NR**, in `MGS4\ReShade.ini` under `[RenoDX.DLSS5]` — the NR look the screenshots were taken with:
  `NeuralUplift=1`, `NRIntensity=2`, `NRStyle=2`, `NRLocalTone=1`, `NRSkinStructure=-1`, `NREnableUpscaling=0`
  (upscaling off: this add-on already runs DLAA on the final image, so NR only denoises / uplifts it).
- **`steam_appid.txt`** containing `2492670` next to `mgs4.exe`, so `--stage` boots do not bounce through Steam.
  Neither Steam nor the game ever writes it, and a reinstall never has one — the app writes it itself before a
  scene boot, and the Setup tab offers it as a button while it is missing.

### Build / install (from source)

Requirements: MSVC Build Tools, ReShade 6.8 installed as `MGS4\dxgi.dll`, the game set to DirectX 12, and `nvngx_dlss.dll` in `MGS4\` (copy `third_party/DLSS/lib/Windows_x86_64/rel/nvngx_dlss.dll` or let the NVIDIA app override supply it). `build.bat` finds the MSVC environment through `vswhere` and `fxc.exe` in the newest Windows 10 SDK; `MGS4_VCVARS` / `MGS4_FXC` in `config.ini` override that. The install script copies to whatever game folder is configured — see [Paths](#paths-configini).

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
- The game runs with vsync on (sync interval 1); DLSS-G works with it (driver reports vsync support) but latency is lower with the game's vsync off. The game's own limiter (`fpsLimiter=60`) caps the *game* frame rate — generated frames come on top of that, which is the whole point: the simulation stays at the 60 fps its physics are tied to.
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
- `steam_appid.txt` (2492670) is placed next to `mgs4.exe` — written by the app before any scene boot — so the exe can be launched directly for testing; harmless for Steam launches.

## Direct3D 12

The port has its own renderer setting: **Options -> Graphics -> API = DirectX 12**, stored as `api=dx12` in `mgs4_savedata_win\<steamid>\mgs4\mgs4.savedsettings`. With it bgfx creates the D3D12 device directly, which is all this add-on needs — it is a D3D12 add-on and does nothing on the D3D11 backend. Setting it is step 1 of the install, and the Setup tab checks it.

- The same settings file has `enableFXAA` (turn it off with DLAA — it only blurs the DLSS input) and `vsync` / `fpsLimiter` (vsync off is better for frame-generation latency).

## Layout

```
mgs4-dlss-launcher           the app, and the only entry point: Play / Settings / Setup
launcher/src, build.ps1      the app itself: Program, Paths, Checks, Install, Catalogue, Runner, Ui/
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

`mgs4-dlss-launcher` (see [The app](#the-app-mgs4-dlss-launcher)) does this and the rest of it — a scene list, the
Cross tapping the flashback prompts want, ending a scene when gameplay starts — and it is what the desktop shortcuts
and `tools\test_stages.ps1` call. `tools\launch_stage.ps1` is still there as a shim over it, so existing shortcuts
and notes keep working:

```bat
mgs4-dlss-launcher s00a00l                            :: boot it, press through the prompts, exit
mgs4-dlss-launcher s00a00l --keys "5,ENTER,4,ENTER"   :: an explicit key sequence instead (menus)
mgs4-dlss-launcher s00a00l --mash-x --end-on-gameplay :: play the whole cutscene, then close the game
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
