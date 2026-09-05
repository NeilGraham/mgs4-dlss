# The launcher (`mgs4-dlss-launcher`)

One window for the whole add-on, and the same things as a command line. It is a C# WPF program built from
`launcher\src` by the compiler that ships with Windows, so it needs nothing installed, and a release is the one exe
on its own: `mgs4_dlss.addon64` and `mgs4_dlss.ini` are built into it, along with everything else it reads.

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
it builds `mgs4-dlss-launcher.exe`, once, and every run after that just starts it. That exe is not in the repo,
because the icon inside it is read from the `mgs4.exe` on this machine and that artwork is Konami's. The exe a
release carries is built with `-Release` instead: it wears the launcher's own icon (drawn by
`tools\launcher_icon.ps1`) and needs no `.bat`. To rebuild by hand:

```bat
powershell -ExecutionPolicy Bypass -File launcher\build.ps1              :: a local build, the game's icon
powershell -ExecutionPolicy Bypass -File launcher\build.ps1 -Release     :: the release exe
```

**One file, everything in it.** The exe carries its XAML, the scene table, the thumbnails and the install file list as
resources, and - once `dlss-addon\build.bat` has run - `mgs4_dlss.addon64` and `mgs4_dlss.ini` too, so a copy
carried off on its own can do the whole install. A file on disk wins over the built-in copy whenever it is there
(`tools\scenes.csv`, `build\mgs4_dlss.addon64`, ...), so editing or rebuilding in a checkout changes what the app
uses without rebuilding the app. A lone exe also keeps its `config.ini` and its `work\` folder under
`%LOCALAPPDATA%\mgs4-dlss-launcher` rather than beside itself; in a checkout they stay in the repo root, where every
script reads them, and a `config.ini` already next to the exe is used wherever it is.

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
| `Paths.DataText` | those data files, from `tools\` or from the copies built into the exe |
| `Install.cs` | the bundled add-on, `steam_appid.txt`, drag-and-drop, the headless ReShade setup |
| `Catalog.cs` | the scene list, in story order |
| `Runner.cs` | the scene run: boot, press through the prompts, tap E for the flashbacks, end on gameplay |
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

- the scene list scrolls **by pixel** (`VirtualizingPanel.ScrollUnit`), keeping recycling virtualization for its
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

Play, Settings and Setup are tabs of the one window, in the top right as three icons — a play triangle, a gear and
the install check — with their names on hover. The **first** run
opens on Setup, because the first thing anyone needs to know is whether the pieces are in place; after that it
opens on Play. `--setup` and `--settings` override that at any time.

**The Setup tab's icon is its verdict**, so the window says whether the install is sound without being asked: a
green tick when everything checked out, an amber warning when something is worth a look, a red cross when a
required file is missing or no game folder was found. It is a gray checklist only until the checks have run, which
happens a moment after the window opens as well as every time Setup is refreshed.

The window opens even when no MGS4 install can be found — it starts on Setup and says which folder it looked in,
which is the one case where a tool that needs the game folder still has to be useful.

## A controller

The whole window works from a pad. An Xbox pad, or anything that arrives as one (Steam Input, DS4Windows, the
virtual pad a Sunshine/Moonlight stream makes), is read through XInput; a DualSense or DualShock 4 plugged straight
in, over USB or Bluetooth, is read as raw HID (`Gamepad.cs`). The moment one is seen, a guide appears in the bottom
bar saying what the buttons do on the tab that is up, in the pad's own names - A / START on an Xbox pad, ✕ / OPTIONS
on a Sony one - and it goes when the pad does. Only the **active** window listens: with the game in front the
presses are the game's.

The bumpers and triggers step between the tabs. On **Play, all scenes** the left stick scrolls the list and the
d-pad steps the pick; when the pick has been scrolled off screen, Down takes the first row on screen and Up the
last. Right (or A) crosses into the run options, where the right stick scrolls, Up and Down walk the boxes, A ticks
one, and Left (or B) comes back. Y goes to the filter chips: Left and Right walk them, A toggles, Y or B leaves.
Start launches whatever is picked, from anywhere; Select flips to **Start Options**, where Left and Right pick a
card, Down reaches the one checkbox, and Select comes back. On **Settings** and **Setup** the right stick scrolls,
Up and Down walk the rows with the same off-screen rule, A presses a row's button or ticks its box, Left and Right
(or the left stick) step a choice or nudge a slider, and Start is Save or Re-check. The numbers with a known range -
volume, sharpness, target fps, display index, trace frames - are sliders for exactly this reason, with the number
in a box beside them that still takes typing.

## Play

**The tab has two views, and it opens on the one that gives nothing away.** The scene list is a spoiler: it names
every cutscene in the game, in the order the story tells them, with a frame of itself on each row — who is in it,
where it happens and how it ends. That is exactly what the person measuring the catalog wants and exactly what
someone who has not finished MGS4 does not.

- **Start Options** (the default) is three cards: **Start the game** (`--main`, straight to the
  menu selection), **Start from the title screen** (`s10a10l`, the logos and PRESS ANY BUTTON first) and **Master
  Collection launcher** (`--collection`). A picture of each, its name and what it is under that; click to pick, and
  **Launch**, **Enter** or a double-click starts it. Each card wears a **Fast** / **Normal** / **Slow** tag in the
  top corner of its picture — green, amber, red — for how long that route makes you wait before you are in the
  game: the menu straight away, the logos and the title screen first, or a second program to get through before
  MGS4 starts at all. One automation is offered, **Skip the boot prompts**, off as
  it comes — and enabled only on the title-screen boot, because the other two open a menu, where a button press
  picks an entry rather than getting past a prompt. It sits in a card with the three buttons on its right, the same
  shape as Setup's "do it for me" rows.
- **All scenes** is the working view described in the rest of this section: the search box, the filter chips and
  the four hundred rows.

**The bottom bar switches them**, in both directions and from either side — **All Scenes** one way, **Start
Options** the other. That strip carries nothing else on this tab, and a switch that is only reachable from
one of two views is a switch you can be stranded away from. Going *towards* the scene list asks first, in as many
words, and asks **once ever**: `SpoilerSeen` in `launcher.json` remembers the answer, because the warning is worth
reading the first time and is nagging by the third, and someone who has said yes has already seen what is behind
it. Coming back asks nothing.

Which view is on is `MGS4_PLAY_VIEW` in `config.ini` — `simple` (or nothing, or anything unrecognised) and `all`.
Settings has it as **Play tab** in the **Launcher** card, and the bottom bar's button writes the same key, so the
two never disagree. It is machine-local like every other `config.ini` key, so a release opens on the safe side on a
machine that has never run it.

**The simple view is centred in whatever room it has**, bounded to 1400 px wide, rather than piled against the top
with the slack under it: three 16:9 cards side by side cannot fill a tall window however they are laid out - their
height is a third of the width - so the leftover goes above and below (a little more below, which is where the eye
reads a block as centred) instead of all in one place. `MinHeight` on the block is the ScrollViewer's own
`ViewportHeight`, so a window too short for it drops back to scrolling.

The list is `tools\scenes.csv` (the stage table) with everything the sweep measured and wrote about each id in
`tools\scene_info.json` - kind, name, description, location, story order, duplicates - plus the two ways of starting the game itself. Pick one, tick what should happen while it
runs, press Launch. The panel shows the command line that does the same thing, so anything set up in the window can
be pasted into a terminal or put in a shortcut — **Copy** puts it on the clipboard, and the box is selectable text.

Those three files, and `tools\install_manifest.json` behind Setup, are **built into the exe as well as read from
`tools\`**. The folder wins when it is there, so editing them in a checkout works as it always has; a copy of the
exe carried off on its own still knows all 414 entries instead of coming up with the two it can name from memory.

**Enter** launches what is picked, **Ctrl+F** reaches the search box from any tab, and **Escape** empties it.

Scenes are grouped by act and every group starts collapsed, so the window opens as a short list of acts rather than
four hundred rows; click a header to open one. The order is story order — Acts 1 to 5, then the epilogue — which the
stage ids do not give you: `s00` is the Big Boss material at the very end of the game, and `s10` / `s20` / `s30` are
the briefings and closing scenes that belong between and after the acts. Typing in the search box opens every group
that matched.

**`tools\scene_info.json`** is where that lives: per stage id, which act it really belongs to, where it sorts inside
it, what kind it is, a name and a one-line description of what you actually see. The stage table can say a scene
exists; only booting it says what it is, so everything in that file was checked by launching it.

**Each row says what kind of scene it is.** A colored badge sits between the stage id and the name, in the same
color families the rest of the window uses: violet **Cutscene** for what you watch, amber **Briefing** for the
Nomad briefings, blue **Stage** for a plain stage boot, green **Start** for the two entries that start the game,
and red **Broken** for the numbered sections that crash. The badge column is a fixed width, so the names line up
down the list. Stage leans cyan rather than taking a truer blue because the stage id beside it is already the
window's blue accent. Gray is left to a kind the launcher does not recognize.

Mint **Gameplay** still exists and nothing wears it: every numbered section in the stage table is broken, so none
of them can be booted far enough to confirm the table's word for it - the badge is there for a section that turns
out to work, set by hand in `scene_info.json`. Its green is kept clear of Start's for that day.

**Any scene can be renamed.** The panel on the right has a **Rename** link under the scene's name: it opens a name
and a description, **Save** keeps them, **Reset** drops them and lets the data files speak again. They are stored
per stage id in `%LOCALAPPDATA%\mgs4-dlss-launcher\launcher.json`, not in the catalog's data files, so re-measuring and
re-naming the catalog cannot lose them - and because the catalog reads them wherever it is used, a renamed scene keeps its
name in `--list` and in the shortcuts you make from it. Either half stands alone: a description typed over a scene
whose name you left alone keeps the catalog's name.

**Double-click a scene to start it** - the same thing the Launch button does with the options as they are
ticked. Act headers and the star are not double-clickable: their clicks are handled before the list sees
them, so neither can pair into one.

**Favorites.** Every scene row has a star on the right: click the outline to add it, click the filled one to take
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

**The filter chips are the badges.** One chip per kind - **Start**, **Stage**, **Cutscene**, **Briefing**,
**Broken** - each wearing that kind's own color: an outline while the filter is off, the color filled in while it
is on, so the row of chips reads as the same legend as the list under it. **Favorites** leads them, in the star's
amber, because it is the one category that is a list you curate rather than something a scene is.

The filter row under the search box is a checklist rather than a dropdown: nothing ticked shows everything (bar the
known-broken ids), and ticking chips shows the union of what they cover - **Cutscene** and **Briefing** together
lists both, not their overlap.

- **Mission briefings** are their own kind, sorted to the top of the act they lead into — the Nomad briefing before
  Act 2 sits above Act 2's own scenes, and the **Briefing** chip shows only those.
- **The same scene under two ids** is one row. `s10a20l` and `s10a20l_D1` start the same thing, as do `s10a40l` and
  `s10a40l_D2`, `s20a00l` and `s20a00l_D1`, `s20a00l_D3` and `s20a10l`, `s30a00l` and `s30a00l_D`, `s30a10l` and
  `s30a00l_D2`. The panel offers both ids so you can boot either, in case they differ in something not visible at
  the first frame.
- **Starting the game** is `--main` (`mgs4.exe --skip-to-main-menu`): straight onto the menu selection, with the
  pre-menu credits and PRESS START already gone. It is the first entry in the Start group, what an empty stage means
  on the command line, and what the window has picked when nothing else has been. It used to lose the device on most
  launches with frame generation on - the menu renders a 3D scene, so the add-on inserts DLSS and switches DLSS-G on,
  and the no-3D gaps around it were then presented with no tags of their own. The add-on now switches frame
  generation off across those gaps (fixed 2026-09-02), so the entry stands on its own.
- **`s10a10l`** is the same boot the long way round: only the Master Collection launcher skipped, so the pre-menu
  credits and PRESS START play first. Listed as "Start from the title screen". It is the one start entry the run
  options mean anything on - `Runner.Run` refuses them for `@main` and `@collection` (`Catalog.IsMenuEntry`),
  because those land on a menu where a press picks NEW GAME rather than getting past a prompt, and this one is the
  game starting itself with a first 3D frame - the title over the cemetery - for the press loop to stop at.
- **Inside a stage, the cutscenes come before the gameplay**: `s01a10l`, then `s01a10l_D1` and `_D2`, then
  `s01a10l_01` onwards. Sorting on the id alone puts `_00` first, because a digit sorts before a letter, which is
  backwards - the demo of a stage plays before the sections it introduces. `_D10` also sorts after `_D9` rather
  than after `_D1`.
- **What is broken is measured, not guessed.** It used to be one regex, `_\d+$` — every numbered section — and
  that was wrong in both directions: `s02a20l_1`, `s02a20l_9` and `s02a20l_11` all boot while `s01a00l_1` crashes,
  and nothing readable separates them (both are in `mgs4.exe`, neither is in the stage-select scenario, the suffix
  shape is the same, and the stage paks are compressed so neither appears in them). Every id is now booted once and
  the verdict recorded. Also worth knowing: **138 of the 250 numbered ids are not in the executable at all**
  (`s01a00l_00`..`_08` are invented; the game has only `s01a00l_1` and `_D`), and the stage-select scenario names
  **29 ids the catalog has never had** — `s04a10l_heliport`, `s04a10l_snowfield`, `s04a40l_smelting_furnace`,
  `s04a50l_underground_aqueduct`, `s03a70l_tower`, `s04a65l_escape`, `s03a35l/s03a40l/s03a60l_MotorCycle`,
  `s04a60l_Vs`, `s04a60l_Vs_VAMP`, `s04a70l_Vs`, `s04a05l_mgs1`, `s20a20l`, `s40a10l` and the `_stryker_event` /
  `_dbg_` ones.

## Measuring the catalog

The tools, run in order. The whole pass takes most of a night and holds the display and the keyboard, so run it
unattended; every step is resumable or re-runnable.

| step | what it does |
|---|---|
| `tools\sweep_record.py` | boots every id one at a time and **records each through OBS** (4K60 AV1, audio), for as long as the scene turns out to need: 10 s of gameplay, a cutscene until it hands over to gameplay or 5 min, 20 s of anything else. With `AssetTrace=1` in `mgs4_dlss.ini` it also reads the engine's own name for the scene - the demo number, the environment bank, the video file - into `tools\stage_probe.csv`, and cuts a repeat of an id already recorded off at 3 s. Skips ids already in the CSV, so Ctrl+C and re-run is safe; `--skip-broken-acts 1,2,3` leaves out ids an earlier pass measured as broken. Recordings land in `<MGS4_OUT>\sweep\video\<id>.mkv`, stills at ~5 s and ~18 s in `<MGS4_OUT>\sweep\<id>_a.jpg` / `_b.jpg`. |
| `tools\scene_identity.py --apply` | which ids are the same scene, from the engine fingerprint: the same demo or video is the same scene outright, and two gameplay entries that share an environment are handed to `tools\find_duplicates.py` (the video comparison) to split. Writes `sameAs` and a `fingerprint` into `scene_info.json`. See [scene-identity.md](scene-identity.md). |
| `tools\stage_names.py` | the location banner the game draws when a stage starts ("MIDDLE EAST / GROUND ZERO"), read out of the game's own string table and stage scripts rather than off the picture. `apply_sweep.py` carries it into `scene_info.json` as `location`, and the Play tab shows it ahead of the description. |
| `tools\classify_scene.py --json tools\scene_kinds.json <MGS4_OUT>\sweep` | reads the ~18 s still and decides **codec / cutscene / gameplay** from the pixels. `--features` dumps the raw numbers as CSV for re-tuning the cut-offs. |
| `tools\sweep_sheets.py` | one contact sheet per 16 frames of each recording, from the scene's own start, under `<MGS4_OUT>\sweep\sheets\`, so a scene can be read - subtitles included - from a few images; `--brief 1 out.md` writes the per-act brief the names are written from. |
| `tools\hud_read.py` | OCR (Windows' own engine, `tools\ocr.ps1`) of every still's top-left and centre: the boss's name on the second health bar - only on a frame that also reads OLD SNAKE - and the ITEM ACQUIRED card. Writes `tools\scene_hud.json`; `apply_sweep.py` turns a boss name into kind **boss** and an item card with no HUD into **gameplay**. |
| `tools\verify_detection.py --cases` | re-derives every kind, merge and order verdict from the recordings alone (`.mkv` frames through the classifier and the OCR, the add-on log for the demo / environment / video) and prints the evidence beside each one. The cases are the ones a review found wrong; keep it green before editing `scene_names.json`. |
| `tools\pick_thumbs.py` | the frame each scene is shown by, chosen by eye from its contact sheets and kept as a **timestamp** in `tools\scene_thumbs.json` (`offset` seconds into the scene, plus a few words on why). `--brief 1 out.md` lists every sheet of an act for the picker, `--merge` folds the answers in, `--apply` cuts each picked frame out of its recording at 1280 wide into `<MGS4_OUT>\sweep\thumbs\`. A pick survives a re-recording because the offset is measured from the scene's own start. |
| `tools\make_thumbs.py` | writes one frame per recorded scene to `tools\thumbs\<id>.jpg` (960x540, q74, ~47 KB each) - the picked frame when there is one, else the sweep's ~18 s still. Individual files so a re-pick changes one file in git; `launcher\build.ps1` zips the folder into the exe at build time, and the launcher decodes at the detail pane's physical width (960 on a 4K screen at 200%) rather than the logical one. |
| `tools\apply_sweep.py` | folds `stage_probe.csv`, `scene_kinds.json`, `scene_hud.json`, the locations and the written `tools\scene_names.json` into `tools\scene_info.json`, which is what the launcher reads. The kind is measured (`measured_kind`: Codec screen > boss bar > 400-series demo = video > item card > environment-at-boot = gameplay > the sweep's live reading) and so is the story order inside a stage (`story_order`: cutscenes by demo number, a playable entry right after the cutscene that hands over into the environment it boots into; the launcher sorts on `order`). Run `scene_identity.py --apply` after it, so an alias row is only ever `sameAs`. |

**How the classifier decides**, and why it is these three signals:

- **Gameplay** — the psyche gauge in the top-left corner: a long horizontal run of saturated amber (`OLD SNAKE`
  over a `STRESS` readout). It is a fixed-size UI element, so the measurement lands at ~0.74 on unrelated gameplay
  frames; warm scenery is saturated too but never forms a bar.
- **Codec** — the call screen is *ruled*: long straight horizontal edges across the frame, which rendered 3D
  essentially never draws (0.039-0.053 on the calls against ~0.000 elsewhere), with a weak green-dominance guard.
  Darkness and symmetry were tried and dropped - a call sits over whatever scene was running.
- **Cutscene** — whatever is left. A scene that draws no gauge and is not a call is a cutscene.

Letterboxing is deliberately **not** a signal. The port's cutscenes are not letterboxed at the swapchain: the black
bands that show up when one of these frames is viewed are the viewer padding the image, and the pixels underneath
are the scene (row 0 of a title-card frame measures 0.85 luma, not 0.0). That was checked before being relied on.
- **Ids that crash or come up black** are out of the list, one by one as the sweep found them rather than by id
  shape. The **Broken** chip shows them if you want them anyway, and searching for one by id still finds it.
- **The categories are Start, Gameplay, Boss, Cutscene, Codec, Video, Briefing and Broken.** There is no "Stage"
  chip: what a bare stage id shows is gameplay or a cutscene like anything else, and it is filed under whatever
  was measured. Only **`Briefing`** (and `Start`) is hand-written in `scene_names.json`: a briefing says what a
  scene is *for*, which nothing measured can show. **`Boss` is measured**: every boss fight loads its own music
  bank (`bgm_sm_boss_vamp`, `bgm_boss_mantis01`, the Beauty phase's `E_bgm_*_boss_*_phase_01`), and the name on
  the second health bar, OCR'd by `tools\hud_read.py`, confirms it. **`Video`** is the drawn-schematic series of
  demo numbers, 431 and up. `apply_sweep.py` keeps a hand-written kind over a measured one, except that a measured
  boss beats a hand-written cutscene or gameplay.
- `s00a00l` and `s00a00l_D` are the "three days earlier" cemetery scene, which plays inside Act 1 rather than with
  the rest of `s00`, so they sit in Act 1 right after the militia-town opening (`sortAs` and `order` in
  `scene_names.json` put them there).

The three game-start entries take none of this: a menu has no boot prompts to press through and no first 3D frame
to wait for, and tapping a key on it would just start a new game, so the launcher starts the game and leaves it
alone. The options below are grayed out while one of them is picked.

What can be ticked (all of it also works from the command line):

- **Skip the boot prompts** (`--advance`, on by default) — taps **Enter** until the add-on log reports the first 3D
  frame, which is what gets a `--stage` boot past the auto-save notice, the "press any button" screen and the load.
  Those prompts take any key, and Enter is the one that has always got past them.
- **Keep pressing E for the flashbacks** (`--mash-x`) — taps **E** about six times a second for the whole scene.
  E is the port's keyboard binding for Cross, and Cross is what MGS4's in-cutscene **flashback** prompts want; Enter
  gets past the boot prompts but does not trigger them. This used to be a virtual DualShock 4 through the
  [ViGEmBus](https://github.com/nefarius/ViGEmBus) driver, which meant installing a kernel driver and a DLL for one
  checkbox; a key needs nothing installed, and the driver, the DLL and the Setup row for them are gone from the
  launcher (`tools\ds4.py`, the sweep's own pad, still uses them). `--press-key <name>` taps that key instead, for
  the boot prompts and the scene both.
- **Close the game when gameplay starts** (`--end-on-gameplay`) — for cutscenes. The add-on's `SCENE-STATE` /
  `SCENE-STATE-TICK` lines say whether the frame is a cutscene, gameplay or no 3D at all; the HUD coming up (40+ HUD
  draws in one heartbeat, against the 4-5 a cutscene draws) or a sustained `gameplay` state ends the run, and a
  sustained `no-3d` catches a scene that ended on a loading / continue screen. `--min-seconds` (30) keeps the HUD
  flicker at the start of some cutscenes from ending them immediately.
- **Close it after a fixed time** (`--hold N`), **render resolution** (`--res 3840x2160`, the port's
  `--res_width` / `--res_height`) - a list of the game's 16:9 sizes in the window, any `WxH` on the command line.

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

## Settings

**Every group says whose setting it is**, in a badge left of its title: **Launcher**, **Game**, **MGS4 DLSS**,
**RenoDX**, and **Debug** alongside the add-on's on the diagnostics, which cost frames and are not for normal play.
The window's own group leads, then the game's, because those are the ones that have to be right before any of the
rest matters. Each card names the file - or files - its rows are written to.

- **Launcher** — *this window's own*, in `config.ini`. **Play tab** (`MGS4_PLAY_VIEW`) is which of the two Play
  views is on: the three ways to start the game, or every scene in it. **Menu music** (`MGS4_MUSIC`) and **Music
  volume** (`MGS4_MUSIC_VOLUME`) are below it — see the section under this one. These are the settings about the
  app rather than about the game, which is why the card wears a badge of its own rather than the Game one the other
  two `config.ini` keys take - those are the game's settings kept in our file.

### Menu music

The window can play the game's own soundtrack while it is open. **None**, **Shuffle**, or one of the tracks the
install actually has — the list is read off disk when the form is built, so it is that machine's answer rather than
a list written here. It stops the moment a run starts and comes back when the game is gone: the game gets the
speakers to itself.

**The menu's own music is not on the iPod.** The named banks are the iPod's playlist; the game's score is filed
as `bgm_*` cues, and its cutscene music under `ww\bank\default`. Two of those are put on the list by name
(`Music.Extras`): `bgm_title_01`, the title screen's theme and what the main menu plays, and
`E_bgm_hv_24demo_lovetheme`, the full Love Theme. They head the list as **Title screen** and **Love Theme**.

**The deck.** While anything plays, the bottom bar carries the track's title, the buttons under it drawn the way
iTunes draws its own, and under those a scrubber with the time either side - drag it or click along it to seek.
A single picked track loops and gets pause alone; the queued modes get back and next as well. Back within three
seconds of a track starting goes to the one before it, later than that to the top of the one playing, and at the
head of a queue with nowhere to go it is greyed out.

**Shuffle** (`MGS4_MUSIC=shuffle`; a `config.ini` still saying `random` means the same) is the iPod on the Nomad:
the whole list dealt into a random order once, walked to its end before a fresh deal is cut, and the first card of
the new deal never the track that just finished. **Playlist** (`playlist`) plays a list of your own in order and
goes round; **Playlist, shuffled** (`playlist-shuffle`) deals that list the same way. The list itself is
`MGS4_PLAYLIST` in `config.ini`, file names comma-separated in play order, and is edited from the Launcher card's
**Edit playlist...** row: the iPod on the left, the list on the right, double-click or Enter to add, Delete to take
out, Space to sample the picked track on the deck, Escape or Done to keep it. On a controller Left and Right pick
the list, A adds or removes, X samples, B is Done. Order of adding is the order of playing.

**Favourites.** Each row on the iPod side ends in a heart; click it, press F, or Y on a pad, and the track is
hearted (`MusicFavorites` in `launcher.json`). Favourites head every list the iPod is shown in - the Menu music
choice and the editor's own - ahead of the starred tracks, with a ♥ in front of the name.

**A plain launcher.** `MGS4_SETUP_TAB=hidden` in `config.ini` (Settings, Launcher, **Setup tab**) takes the Setup
tab away: no install check, no add-on, nothing about DLSS on screen, for a machine with nothing to set up - an AMD
card's, say. The bumpers skip it and a first run opens on Play instead.

**Where the music is.** `<GameDir>\common\bank\default`, one FMOD Studio bank per track and named for the track:
`At_Dawn.bank`, `Sea_Breeze.bank`, `Beyond_The_Bounds.bank`. 272 banks there, 73 of them named tracks — the rest
are the combat and alert cues (`bgm_*`), the per-stage ambience (`env_*`) and FMOD's own project banks, none of
which is music to sit a menu on. 48 kHz stereo, two and a half to four and a half minutes each.

**Why it needs a decoder.** Inside each bank is a RIFF/`FEV` wrapper around an FSB5 block, and the audio in that is
Vorbis — but FMOD strips the setup headers out of the stream and leaves a CRC naming which codebooks it was built
against (`0xc4c30a29`, the same for every track here). Without those codebooks not one byte decodes. **ffmpeg
cannot do it**: it has an `fsb` demuxer, but carving the FSB5 out and feeding it in answers `version 5 is not
implemented` — FSB3 and FSB4 only. `vgmstream` carries the codebook table and reads these directly, at about a
third of a second for a three-and-a-half-minute track.

**How the decoder gets there.** It is not shipped here — the release is still the one exe. The **Download
vgmstream** button in the Launcher card fetches it (about 4 MB, from vgmstream's own GitHub release) into
`%LOCALAPPDATA%\mgs4-dlss-launcher\tools\vgmstream`. It is a press rather than something the app does on its own
the first time it runs: that is this program reaching out to the internet, and it is the person's call. Already
have one? `MGS4_VGMSTREAM` in `config.ini` names it, and a `vgmstream-cli.exe` on PATH is found too. With no
decoder the setting still shows and says what is missing.

**Nothing of Konami's is copied into this repo or into a release.** The banks are read from the install the person
already owns, decoded into a cache under their own `%LOCALAPPDATA%\mgs4-dlss-launcher\music`, and played there —
the same rule the window already follows for the key art and the game's icon. The cache keeps the four
most-recently-used tracks and drops the rest; a decode is a third of a second and a WAV is 40 MB, so keeping every
track anyone ever tried would be paying gigabytes to save nothing.

- **Display** and **Quality** — *the game's own*, out of `mgs4_savedata_win\<steamid>\mgs4\mgs4.savedsettings`, the
  same file its in-game menu writes: renderer (`api`), display index, vsync, frame limiter, the four quality levels
  and FXAA. Four of them are what the add-on needs set a particular way, and the Setup tab keeps its one-click
  **Set them for me** for exactly those four.
- **Display** also holds two keys that are this app's rather than the game's, in `config.ini`: **Resolution**
  (`MGS4_RES`, a list of the game's 16:9 sizes: 720p, 1080p, 1440p, 2160p) and **Mode** (`MGS4_WINDOWING`). The game has no setting for either - it takes
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
each key carries its own spelling and a tick box writes whichever its file expects. **Save settings** is grayed until something is actually
changed - it compares each control against what its file said when the form was built - and then writes all of
them at once and says what went where (`written: 23 to mgs4_dlss.ini, 9 to mgs4.savedsettings`); a group whose file
does not exist yet shows *not there* until whatever writes it has run.

`mgs4-dlss-launcher --settings` opens the window on this tab, the way `--setup` opens its own;
`--show-settings` prints every group the way the tab shows them, naming each group's files. `--set
Key=Value` writes without opening a window and **routes each key to the file, and the section, that holds it**, so
`--set api=dx12 --set MGS4_RES=3840x2160 --set NRIntensity=3 --set Sharpness=42` writes one value to each of four
files; a key no group knows goes to the add-on's ini, which is where every key used to go.

Saving is blocked while the game is running, because every one of these files has an owner then: the add-on rewrites
`mgs4_dlss.ini` through the Windows profile API, whose cache will quietly undo an outside edit, and the game
rewrites `mgs4.savedsettings` when it exits. While the game *is* up, the add-on's keys are live in ReShade's
overlay, Add-ons tab, and the game's are live in its own options menu.

## Setup

Most of the files this add-on needs cannot be shipped here: NVIDIA's DLSS runtimes, the Streamline runtime and
ReShade all have to be fetched from their own projects, so an install is assembled by hand and it is easy to end up
one file short. The Setup tab opens with the verdict, then the game folder everything is checked against — the path, where it came
from, **Open folder** to show it in Explorer, and **Change...** to record a folder of your own as `MGS4_DIR` in
`config.ini`. The Steam-library lookup is what runs whenever `MGS4_DIR` is not set, so going back to it is a matter
of clearing that key - remove or comment out the `MGS4_DIR` line in `config.ini`, and unset the environment
variable of the same name if one is set, since the environment wins over the file. **A game on another drive needs nothing special**: Steam's own install - found
from the registry, normally on C: - carries `steamapps\libraryfolders.vdf`, and that file names every library on
every drive, so a D: install is read out of the C: client. The PowerShell and Python resolvers additionally try
each drive in letter order for the handful of places a library sits, which catches one Steam has forgotten.
When no `mgs4.exe` turns up, the card **names the libraries it looked in** - if the drive holding the install is
not among them, that library is not registered with Steam and Change... is the way in. **Re-check** (or F5) re-runs everything with the window open, so it can be left up on a
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
  an **Install the add-on** button that copies the pair the app ships with into the game folder: a release has them
  built into the exe, a source checkout has the built add-on in `build\` and the sample ini in `dlss-addon\`, and a
  pair dropped next to the exe wins over both.
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
- **Last run**, parsed from `logs\mgs4_dlss.log` and `ReShade.log`: whether NGX initialized, whether DLSS came from
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
checks in `launcher\src\Checks.cs` only render it. A copy goes into the exe at build time and is used when that
folder is not there; with neither, Setup says so in a card and the rest of the window carries on.

The shipped ini is the configuration v1.2.0 ships with and was verified on: DLAA preset K at 3840x2160, jitter + camera and object motion vectors, DLSS 5 NR through `renodx-dlss5`, dynamic-resolution handling, depth of field re-applied after NR (`PostDof=1`) and dynamic frame generation to 240 fps. The diagnostic keys at the bottom (`TraceFreeze`, `TraceFrames`, `Probe`, `DumpShaders`) are off; turning them on costs frames.

## Direct stage boot (what the Play tab does)

`mgs4.exe --stage <name>` skips the Master Collection screen and the menus: `s00title_1` (OTC intro), `s00a00l` (cemetery opening),
`s01a00l` (Act 1 start), ... (names listed in the exe). `steam_appid.txt` next to the exe keeps Steam from
relaunching. A desktop shortcut "MGS4 (stage s00a00l)" boots straight into the cemetery for quick tests.

`mgs4-dlss-launcher` (see above) does this and the rest of it — a scene list, the
E tapping the flashback prompts want, ending a scene when gameplay starts — and it is what the desktop shortcuts
and `tools\test_stages.ps1` call. `tools\launch_stage.ps1` is still there as a shim over it, so existing shortcuts
and notes keep working:

```bat
mgs4-dlss-launcher s00a00l                            :: boot it, press through the prompts, exit
mgs4-dlss-launcher s00a00l --keys "5,ENTER,4,ENTER"   :: an explicit key sequence instead (menus)
mgs4-dlss-launcher s00a00l --mash-x --end-on-gameplay :: play the whole cutscene, then close the game
```

Keys go through `keybd_event` with the window forced to the foreground — this port ignores scan-code `SendInput`
events, and only accepts input while it is the foreground window. Log: `MGS4\logs\launcher.log`.
