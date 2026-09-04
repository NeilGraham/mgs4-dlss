"""Boots every stage id and records it through OBS: 4K60 AV1 with audio, one file per scene, each starting on the
loading screen a few seconds before the scene itself - and, from the add-on's AssetTrace line, the engine's own
name for what it loaded (the demo number, the environment bank, the video), which is what identifies a scene.

  python tools/sweep_record.py                          every catalog id, act by act
  python tools/sweep_record.py --ids my.txt             one id per line
  python tools/sweep_record.py --acts 1,2               only those acts (1-5; 0 = the s00 epilogue; 10/20/30/99 = interludes)
  python tools/sweep_record.py --skip-broken-acts 1,2,3 leave out ids scene_info.json already calls broken in those acts
  python tools/sweep_record.py --first 10               stop after this many recordings (a check pass)
  python tools/sweep_record.py --cap-gameplay 10 --cap-cutscene 300 --cap-other 20
  python tools/sweep_record.py --recompute         re-derive the fingerprint columns from the kept logs

How long each scene is recorded depends on what it turns out to be, judged live from the add-on's SCENE-STATE
line and from a frame of the picture:
  cutscene   a demo (e_d###.bank) or a video (.bk2) loaded at boot, or 3D with no HUD for `min-cutscene` s
             -> until it hands over to gameplay and stays there, or `cap-cutscene` (300)
  gameplay   the engine reads gameplay for 2 s *and* a still shows the psyche gauge -> `cap-gameplay` (10);
             a demo at boot with the HUD still up at 12 s (an item pickup, a boss intro) -> 45 s (DEMO_GAMEPLAY_CAP)
  codec      the call screen, read off a still - it wins over the engine's gameplay -> `cap-other` (20)
  other      neither confirmed by 20 s (a title card, a menu, black) -> `cap-other` (20)
  mgs1       no 3D frame and mgs1.exe is running: the MGS1 dream (s04a05l) -> `cap-other` (20)
  duplicate  the same demo / video as an id already recorded -> stopped at 3 s, nothing kept (see `seen`)
What a scene finally *is* (boss, video, item pickup...) is decided afterwards by tools/apply_sweep.py from the
stills, the OCR'd HUD (tools/hud_read.py) and the demo number.

OBS rather than ffmpeg for two reasons that are not close: it captures **audio** (these are meant to be
watchable), and its Display Capture source sidesteps the Game Capture hook dropping to black for several seconds
whenever the game rebuilds its swapchain on a stage load - which is exactly when a cutscene starts. That lesson is
already written into tools/obs_control.py; it is not being relearned here. The websocket address comes from
MGS4_OBS_HOST / _PORT / _PASSWORD (config.ini or the environment).

**Where a recording starts.** Not at the desktop. The add-on says "swapchain created" the moment the game's window
exists, which is about 5 s ahead of the first rendered frame, and recording begins there. Afterwards the file is
trimmed losslessly to `lead` seconds before the scene proper, so every video opens on the loading screen rather
than on whatever was on the monitor. Trimming after the fact rather than trying to start the recorder at exactly
the right instant: OBS takes a moment to spin up, and a late start clips the opening of the scene, which is the
one part that cannot be recovered.

**The fingerprint.** With AssetTrace=1 in mgs4_dlss.ini the add-on logs every named file the game opens under its
own folder ("SCENE-ASSET open f<frame> <path>"). The ones that matter, per docs/scene-identity.md:
  e_d###.bank        the demo - MGS4's own number for a cutscene; two ids that load the same one are one scene
  env_<stage>_NN.bank a gameplay entry's environment
  BK2\\*.bk2          a pre-rendered video
  *boss_*.bank        a boss fight's music (bgm_sm_boss_vamp, bgm_boss_mantis01, E_bgm_ee_boss_raven_phase_01 for
                      the Beauty phase) where an escort section loads bgm_*_event_* (the Stryker legs, the bike)
Only the opens up to a few seconds after FIRST-3D-FRAME count as the scene's own ("demo"); anything chained in
later is kept apart ("demo_late"), so a long capture and a short one of the same scene still agree.

Output
  tools/stage_probe.csv          one row per id (FIELDS below)
  <MGS4_OUT>/sweep/video/<id>.mkv
  <MGS4_OUT>/sweep/<id>_a.jpg, _b.jpg, _z.jpg  stills at ~5 s, ~18 s (9 s for gameplay) and the last frame, 1280 wide
  <MGS4_OUT>/sweep/<id>.addon.log
"""
import os, re, sys, csv, time, json, shutil, subprocess, argparse

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import paths          # noqa: E402
import obs_control as oc  # noqa: E402

REPO = os.path.dirname(HERE)
LAUNCHER = os.path.join(REPO, "mgs4-dlss-launcher.exe")
PROBE = os.path.join(HERE, "stage_probe.csv")
INFO = os.path.join(HERE, "scene_info.json")
SHOTS = os.path.join(paths.OUT_DIR, "sweep")
VIDEO = os.path.join(SHOTS, "video")
ADDON_LOG = os.path.join(paths.GAME_DIR, "logs", "mgs4_dlss.log")
LAUNCH_LOG = os.path.join(paths.GAME_DIR, "logs", "launcher.log")
DUMPS = os.path.join(paths.GAME_DIR, "crash_dumps")
FIELDS = ["id", "result", "seconds", "scene_offset", "scene_at", "video", "state", "draws", "hud", "kind_live",
          "demo", "demo_late", "env", "movie", "boss", "detail"]

# The game's window does not cover the screen the instant the add-on says "swapchain created" - the desktop is
# still there for about a second. Every recording therefore loses at least this much off the front, whatever the
# scene offset works out to, so nothing opens on a stray window.
DESKTOP_TRIM = 1.6
# A demo opened this many frames past FIRST-3D-FRAME (25 s at 60 fps) still belongs to the scene's own load, unless
# an environment bank was opened first: the environment is where a cutscene hands over to play, so a demo after it
# is chained in by what happens next, not by what the id starts. The window is wide because an act's opening shows
# its title card for 17 s before the demo loads (s02a10l: E_d328 at frame 1175 under the "gameplay" the engine
# reads for the card), and narrow at the environment so a gameplay entry's later trigger stays "late".
BOOT_FRAMES = 1500
# A gameplay entry that opened with a demo (an item pickup, a boss intro) is recorded this long rather than the
# gameplay cap: a boss's health bar comes up only once its intro is over - Vamp's split-screen entrance on
# s04a60l ran past the 20 s the entry used to get, and the bar was never on tape for the OCR to read.
DEMO_GAMEPLAY_CAP = 45.0

STATE_RE = re.compile(r"SCENE-STATE(?:-TICK)? (cutscene|gameplay|no-3d) \(frame \d+, scene draws (\d+), HUD draws (\d+)")
FIRST_RE = re.compile(r"FIRST-3D-FRAME \(frame (\d+)\)")
ASSET_RE = re.compile(r"SCENE-ASSET (open|miss) f(\d+) (.+)")


def ps(cmd):
    subprocess.run(["powershell", "-NoProfile", "-Command", cmd], capture_output=True)


def kill_game():
    # mgs1 too: s04a05l starts the bundled MGS1 as its own process, which nothing else closes; and the Master
    # Collection's own front-end (@collection), matched by its path so no other launcher.exe is touched.
    ps("Get-Process mgs4,mgs1,mgs4-dlss-launcher -ErrorAction SilentlyContinue | Stop-Process -Force; "
       "Get-Process launcher -ErrorAction SilentlyContinue | Where-Object { $_.Path -like '*METAL GEAR SOLID 4*' } | Stop-Process -Force")


def record_menu_entry(cl, source, sid, cap):
    """The two menu entries that are not stage ids: @main (mgs4.exe --skip-to-main-menu) and @collection (the
    Master Collection's Unity front-end, a different process the add-on never sees). Nothing to wait for and
    nothing to fingerprint: launch, record `cap` seconds from the moment the window is up, keep stills."""
    t0 = time.time()
    proc = subprocess.Popen([LAUNCHER, sid])
    time.sleep(2.5)                                   # the window is up within a couple of seconds
    cl.start_record(); rec_started = time.time()
    shots = {}
    while time.time() - rec_started < cap:
        el = time.time() - rec_started
        if "a" not in shots and el >= 5.0 and source:
            shots["a"] = shot(cl, source, os.path.join(SHOTS, sid + "_a.jpg"))
        if "b" not in shots and el >= cap - 1.5 and source:
            shots["b"] = shot(cl, source, os.path.join(SHOTS, sid + "_b.jpg"))
        time.sleep(0.5)
    if source:
        shot(cl, source, os.path.join(SHOTS, sid + "_z.jpg"))
    out_path = None
    try:
        out_path = cl.stop_record().output_path
    except Exception as e:
        print("   stop_record failed:", e)
    try:
        proc.kill()
    except Exception:
        pass
    kill_game(); time.sleep(1.5)
    final = ""
    if out_path and os.path.exists(out_path):
        dst = os.path.join(VIDEO, sid + ".mkv")
        if trim(out_path, dst, 0.0, cap):
            os.remove(out_path)
        else:
            shutil.move(out_path, dst)
        final = sid + ".mkv"
    return dict(id=sid, result="boot", seconds=int(time.time() - t0), scene_offset=0.0, scene_at=0.0,
                video=final, state="", draws=-1, hud=-1, kind_live="start", demo="", demo_late="", env="",
                movie="", boss="", detail="menu entry, recorded %.0fs" % cap)


def game_up():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq mgs4.exe", "/NH"], capture_output=True, text=True).stdout
    return "mgs4.exe" in out


def mgs1_up():
    """s04a05l is the MGS1 dream: mgs4.exe hands over to the bundled 1998 game in a process of its own (mgs1.exe),
    which never draws a 3D frame the add-on can see. The process is the fingerprint."""
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq mgs1.exe", "/NH"], capture_output=True, text=True).stdout
    return "mgs1.exe" in out


def tail_since(path, mark):
    try:
        if os.path.getsize(path) <= mark:
            return ""
        with open(path, "r", errors="replace") as fh:
            fh.seek(mark)
            return fh.read()
    except OSError:
        return ""


def dumps_count():
    try:
        return len([f for f in os.listdir(DUMPS) if f.endswith(".dmp")])
    except OSError:
        return 0


def trim(src, dst, start, length=None):
    """Copy the stream from `start` without re-encoding. AV1 in Matroska cuts at the nearest keyframe, which at
    OBS's default interval is within a couple of seconds - close enough for 'starts on the loading screen'."""
    cmd = [paths.FFMPEG, "-v", "error", "-ss", "%.2f" % max(0.0, start), "-i", src]
    if length:
        cmd += ["-t", "%.2f" % length]
    cmd += ["-c", "copy", "-y", dst]
    r = subprocess.run(cmd, capture_output=True)
    return r.returncode == 0 and os.path.exists(dst) and os.path.getsize(dst) > 0


def act_of(row):
    """Sort key for the catalog's act column: the five acts in order, then the s00 epilogue, then the interludes."""
    a = row.get("act", "")
    m = re.match(r"Act (\d)", a)
    if m:
        return int(m.group(1))
    if a.startswith("Prologue"):
        return 6
    m = re.search(r"s(\d+)", a)
    return 6 + (int(m.group(1)) if m else 99)


def act_number(row):
    """The number --acts / --skip-broken-acts speak in: 1-5, 0 for the s00 epilogue, 10/20/30/99 for interludes."""
    a = row.get("act", "")
    m = re.match(r"Act (\d)", a)
    if m:
        return int(m.group(1))
    if a.startswith("Prologue"):
        return 0
    m = re.search(r"s(\d+)", a)
    return int(m.group(1)) if m else 99


def catalog():
    with open(os.path.join(HERE, "scenes.csv"), newline="") as fh:
        rows = [r for r in csv.DictReader(fh) if r.get("stage_entry")]
    order = sorted(range(len(rows)), key=lambda i: (act_of(rows[i]), i))
    return [rows[i] for i in order]


def assets_of(log_text):
    """demo / demo_late / env / movie / assets from the add-on log's SCENE-ASSET lines."""
    first = None
    m = FIRST_RE.search(log_text)
    if m:
        first = int(m.group(1))
    demo, late, env, movie, boss, other = [], [], [], [], [], []
    env_at = None
    for ok, frame, path in ASSET_RE.findall(log_text):
        if ok != "open":
            continue
        frame = int(frame)
        base = re.split(r"[\\/]", path.strip())[-1]
        lower = base.lower()
        d = re.match(r"e_d(\d+)\.bank$", lower)
        if d:
            limit = (first + BOOT_FRAMES) if first is not None else None
            if env_at is not None and first is not None and env_at > first:
                limit = min(limit, env_at)
            tgt = late if (limit is not None and frame > limit) else demo
            if d.group(1) not in tgt:
                tgt.append(d.group(1))
            continue
        e = re.match(r"env_(.+)\.bank$", lower)
        if e:
            if env_at is None:
                env_at = frame
            if e.group(1) not in env:
                env.append(e.group(1))
            continue
        if lower.endswith(".bk2") or "\\bk2\\" in ("\\" + path.lower()):
            if base not in movie:
                movie.append(base)
            continue
        bb = re.search(r"boss_([a-z_]+?)\d*(?:_beast|_phase)?(?:_\d+)?\.bank$", lower)   # bgm_sm_boss_vamp, E_bgm_ee_boss_raven_phase_01, bgm_boss_mantis01
        if bb:
            if bb.group(1) not in boss:
                boss.append(bb.group(1))
            continue
        if base not in other:
            other.append(base)
    return dict(demo="+".join(demo), demo_late="+".join(late), env="+".join(env), movie="+".join(movie),
                boss="+".join(boss))


def recompute(out):
    """The fingerprint columns of every row, again, from the add-on log the sweep kept for it - so a change to
    assets_of() (the boot window) applies to what is already recorded, not only to what comes next."""
    rows = list(csv.DictReader(open(out, newline="")))
    changed = 0
    for r in rows:
        p = os.path.join(SHOTS, r["id"] + ".addon.log")
        if not os.path.exists(p):
            continue
        fp = assets_of(open(p, errors="replace").read())
        if any(r.get(k, "") != fp[k] for k in fp):
            changed += 1
            r.update(fp)
    with open(out, "w", newline="") as fh:
        w = csv.DictWriter(fh, FIELDS); w.writeheader()
        for r in rows:
            w.writerow({k: r.get(k, "") for k in FIELDS})
    print("recomputed %d rows, %d changed -> %s" % (len(rows), changed, out))
    return 0


def early_key(fp):
    """The exact part of a fingerprint: the demo(s) loaded at boot, else the video. None for a gameplay entry."""
    if fp.get("demo"):
        return ("demo", fp["demo"])
    if fp.get("movie"):
        return ("movie", fp["movie"])
    return None


def shot(cl, source, path, width=1280):
    try:
        cl.save_source_screenshot(source, "jpg", path, width, int(width * 9 / 16), 88)
        return os.path.exists(path)
    except Exception as e:
        print("   screenshot failed:", e)
        return False


def still_kind(path):
    """codec / gameplay / cutscene as tools/classify_scene.py reads the still; "" when it cannot."""
    try:
        import classify_scene
        return classify_scene.classify(classify_scene.features(path))[0]
    except Exception as e:
        print("   classify failed:", e)
        return ""


def looks_like_codec(path):
    return still_kind(path) == "codec"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ids")
    ap.add_argument("--acts", help="comma-separated act numbers to include")
    ap.add_argument("--skip-broken-acts", help="acts whose scene_info.json 'broken' ids are left out")
    ap.add_argument("--first", type=int, default=0, help="stop after this many recordings")
    ap.add_argument("--cap-gameplay", type=float, default=10.0)
    ap.add_argument("--cap-cutscene", type=float, default=300.0)
    ap.add_argument("--cap-other", type=float, default=20.0)
    ap.add_argument("--lead", type=float, default=3.0, help="seconds of loading screen kept before the scene")
    ap.add_argument("--boot-timeout", type=float, default=30.0,
                    help="seconds after the window appears to wait for a 3D frame")
    ap.add_argument("--min-cutscene", type=float, default=12.0,
                    help="seconds of cutscene needed before a handover to gameplay ends the recording")
    ap.add_argument("--settle", type=float, default=8.0,
                    help="seconds of sustained gameplay that count as the cutscene having ended")
    ap.add_argument("--out", default=PROBE)
    ap.add_argument("--recompute", action="store_true",
                    help="re-derive demo / env / movie / assets for every row from <MGS4_OUT>/sweep/<id>.addon.log and exit")
    args = ap.parse_args()
    if args.recompute:
        return recompute(args.out)

    os.makedirs(VIDEO, exist_ok=True)
    rows = catalog()
    if args.ids:
        want = [l.strip() for l in open(args.ids) if l.strip() and not l.startswith("#")]
        byid = {r["stage_entry"].strip(): r for r in rows}
        rows = [byid.get(i, {"stage_entry": i, "act": ""}) for i in want]
    if args.acts:
        acts = {int(a) for a in args.acts.split(",")}
        rows = [r for r in rows if act_number(r) in acts]
    skipped = []
    if args.skip_broken_acts:
        acts = {int(a) for a in args.skip_broken_acts.split(",")}
        info = json.load(open(INFO, encoding="utf-8")).get("scenes", {})
        keep = []
        for r in rows:
            e = info.get(r["stage_entry"].strip(), {})
            if act_number(r) in acts and e.get("kind") == "broken":
                skipped.append(r["stage_entry"].strip())
            else:
                keep.append(r)
        rows = keep
    ids = []
    for r in rows:
        i = r["stage_entry"].strip()
        if i not in ids:
            ids.append(i)

    done = {}
    if os.path.exists(args.out):
        with open(args.out, newline="") as fh:
            done = {r["id"]: r["result"] for r in csv.DictReader(fh)}
    else:
        with open(args.out, "w", newline="") as fh:
            csv.DictWriter(fh, FIELDS).writeheader()
    todo = [i for i in ids if i not in done]
    if args.first:
        todo = todo[: args.first]

    cl = oc.client()
    st = cl.get_record_status()
    if st.output_active:
        cl.stop_record(); time.sleep(1.5)
    # the display-capture source of whatever scene OBS is on, for the stills
    scene = cl.get_current_program_scene()
    scene_name = getattr(scene, "current_program_scene_name", None) or getattr(scene, "scene_name", None)
    source = None
    for it in cl.get_scene_item_list(scene_name).scene_items:
        if it.get("sceneItemEnabled") and it.get("inputKind") in ("monitor_capture", "game_capture", "window_capture"):
            source = it["sourceName"]
    print("sweep: %d ids (%d skipped as broken), %d recorded, %d to go  (gameplay %.0fs / cutscene %.0fs / other %.0fs, lead %.0fs)"
          % (len(ids), len(skipped), len(ids) - len([i for i in ids if i not in done]), len(todo),
             args.cap_gameplay, args.cap_cutscene, args.cap_other, args.lead))
    print("OBS scene %r, stills from %r, recordings to %s" % (scene_name, source, VIDEO))

    tally = {"boot": 0, "crash": 0, "no-scene": 0}
    for r in done.values():
        tally[r] = tally.get(r, 0) + 1
    # What has already been recorded, by the engine's own key, so a repeat can be cut off the moment its demo or
    # video is known - within a few seconds of the first frame, instead of five minutes later. Only the exact
    # keys are trusted for this (a demo, a video); two gameplay entries sharing an environment may still be two
    # spawns, and they cost ten seconds each anyway.
    seen = {}
    if os.path.exists(args.out):
        with open(args.out, newline="") as fh:
            for r in csv.DictReader(fh):
                k = early_key(r)
                if k and r.get("video"):
                    seen.setdefault(k, r["id"])

    max_cap = max(args.cap_gameplay, args.cap_cutscene, args.cap_other)
    for n, sid in enumerate(todo, 1):
        kill_game(); time.sleep(1)
        if sid.startswith("@"):
            row = record_menu_entry(cl, source, sid, args.cap_other)
            tally["boot"] += 1
            with open(args.out, "a", newline="") as fh:
                csv.DictWriter(fh, FIELDS).writerow(row)
            print("[%4d/%d] %-14s %-9s %4ds %-9s %s" % (n, len(todo), sid, row["result"], row["seconds"],
                                                       row["kind_live"], row["detail"]), flush=True)
            continue
        # Delete the add-on log before launching. The add-on recreates it (fopen "w") on every start, but until
        # the new process gets that far the file still holds the *previous* run - and reading that made every
        # trigger fire instantly on stale "swapchain created" / "FIRST-3D-FRAME" lines, so a known-crashing id
        # was recorded as a boot in 1 second. Absence is the only reliable "this run has not written yet".
        try:
            os.remove(ADDON_LOG)
        except OSError:
            pass
        for suf in ("_a.jpg", "_b.jpg", "_z.jpg"):
            try:
                os.remove(os.path.join(SHOTS, sid + suf))
            except OSError:
                pass
        before, lmark = dumps_count(), (os.path.getsize(LAUNCH_LOG) if os.path.exists(LAUNCH_LOG) else 0)
        t0 = time.time()
        proc = subprocess.Popen([LAUNCHER, sid, "--hold", str(int(max_cap + 20)), "--max-minutes",
                                 str(int(max_cap / 60) + 2)])

        # 1. wait for the game's window to exist, then start recording - never the desktop
        rec_started = None
        while time.time() - t0 < 30:
            if "swapchain created" in tail_since(ADDON_LOG, 0):   # log is this run's: it was deleted above
                cl.start_record(); rec_started = time.time(); break
            if proc.poll() is not None or dumps_count() > before:
                break
            time.sleep(0.2)
        if rec_started is None and proc.poll() is None and dumps_count() == before:
            cl.start_record(); rec_started = time.time()   # fall back rather than lose the run

        # 2. wait for the first 3D frame - or for a pre-rendered video to open. A video never draws a 3D frame
        #    (the engine reads no-3d throughout), yet to the player it is a cutscene, so it is recorded as one:
        #    the moment the .bk2 is opened counts as the scene's start.
        scene_at, video_scene, start_entry = None, False, False
        deadline = (rec_started or t0) + args.boot_timeout
        while time.time() < deadline:
            tail = tail_since(ADDON_LOG, 0)
            if "FIRST-3D-FRAME" in tail:
                scene_at = time.time(); break
            if rec_started and time.time() - rec_started > 3.0 and assets_of(tail).get("movie"):
                scene_at, video_scene = time.time(), True; break
            if dumps_count() > before:
                break
            # The launcher leaves at once on a game-start entry (the credits and the menu, s10a10l): there are no
            # boot prompts to press through, so it launches and returns. The game is still up and is recorded
            # for the `cap-other` budget like anything without a scene. Any other early exit is the game dying.
            if proc.poll() is not None:
                if game_up():
                    start_entry = True
                else:
                    break
            time.sleep(0.2)

        # 3. hold until the scene ends, the game dies, or its kind's cap - whichever comes first.
        #    "Ends" is the cutscene handing over to gameplay and *staying* there: the two states flip back and
        #    forth for a few seconds around a transition (the add-on logs cutscene->gameplay->cutscene within
        #    three seconds on s01a30l), so a single reading means nothing and only a sustained one counts.
        end_reason, kind_live, dup_of = "cap", "", None
        shot_a = shot_b = False
        if scene_at:
            # A scene only counts as "a cutscene that ended" if it actually spent time being a cutscene. The
            # add-on reports one or two seconds of "cutscene" at the start of ordinary gameplay too, before the
            # HUD comes up, and treating that as a handover cut s01a00l off after 13 seconds.
            cutscene_secs, cur_state, since, last = 0.0, None, time.time(), time.time()
            by_engine, hud_checked, gauge_seen, looked_at = False, False, False, -99.0
            while True:
                now = time.time()
                elapsed = now - scene_at
                if not game_up():
                    end_reason = "game exited"; break
                # the launcher leaves the moment the game's window is gone, seconds before the process is: a
                # recording that runs on past that point is of the desktop (s01a05l_D, s04a30l_D7)
                if proc.poll() is not None and not start_entry:
                    end_reason = "the game's window closed"; break
                hits = STATE_RE.findall(tail_since(ADDON_LOG, 0))
                if hits:
                    now_state = hits[-1][0]
                    # a video is "no-3d" to the engine, but it is the cutscene being watched
                    if cur_state == "cutscene" or (by_engine and cur_state == "no-3d"):
                        cutscene_secs += now - last
                    if now_state != cur_state:
                        cur_state, since = now_state, now
                last = now
                # The engine's own word, read every pass until it has said something: a demo or a video opened.
                # The same one as an id already recorded? Then this is a repeat: stop here. Any at all? Then it
                # is a cutscene whatever the HUD reading says - the HUD is up for a moment before some cutscenes
                # start (s01a10l_D2 read as gameplay for two seconds and was cut at ten), and a pre-rendered
                # video never draws a 3D frame. Read every pass rather than once: an act's opening shows its
                # title card for ten seconds before the demo loads (s02a10l), and a single look at 3 s missed it.
                if not by_engine and kind_live != "gameplay" and elapsed >= 3.0:
                    fp_now = assets_of(tail_since(ADDON_LOG, 0))
                    k = early_key(fp_now)
                    if k and seen.get(k) not in (None, sid):
                        dup_of = seen[k]
                        end_reason = "duplicate of %s (%s %s)" % (dup_of, k[0], k[1]); kind_live = "duplicate"
                        break
                    if k:
                        kind_live, by_engine = "cutscene", True
                    elif not kind_live and fp_now.get("env") and cur_state == "gameplay" and now - since >= 2.0:
                        # an environment bank at boot and no demo: a playable entry by the engine's own word,
                        # gauge or no gauge (the motorcycle legs open without one). The 5 s still can still
                        # turn it into a Codec call.
                        kind_live = "gameplay"
                # The picture, every few seconds until it has said what this is. The engine's "gameplay" alone is
                # not enough: it reads gameplay under an act's title card and under a Codec call (s01a30l), so
                # gameplay needs the psyche gauge in a still to be confirmed, and the Codec screen wins outright.
                if source and elapsed >= (5.0 if not shot_a else looked_at + 4.0) and (not shot_a or not kind_live):
                    p = os.path.join(SHOTS, sid + ("_a.jpg" if not shot_a else "_c.jpg"))
                    look = still_kind(p) if shot(cl, source, p) else ""
                    looked_at = elapsed
                    if not shot_a:
                        shot_a = True
                    else:
                        try:
                            os.remove(p)
                        except OSError:
                            pass
                    if look == "codec":
                        kind_live = "codec"
                    elif look == "gameplay":
                        gauge_seen = True
                # what is this, so far?
                if not kind_live:
                    if cutscene_secs >= args.min_cutscene:
                        kind_live = "cutscene"
                    elif cur_state == "gameplay" and now - since >= 2.0 and gauge_seen:
                        kind_live = "gameplay"
                b_at = 9.0 if kind_live == "gameplay" else 18.0
                if not shot_b and elapsed >= b_at and source:
                    shot_b = shot(cl, source, os.path.join(SHOTS, sid + "_b.jpg"))
                # A demo at boot is not always a cutscene: an entry that starts on an item pickup loads a tiny
                # "item get" demo and then sits in gameplay behind the item's popup for as long as nobody presses
                # a button (s02a50l_1 spent its whole five minutes on the Syringe card). A real cutscene has no
                # psyche gauge; so if the engine still reads gameplay at 12 s and the gauge is in the picture,
                # this is gameplay. It then gets the `cap-other` budget rather than the gameplay one, because a
                # boss fight opens this way too and its second health bar comes up a few seconds after Snake's
                # (s03a70l_D3) - the still at the end is what the boss check reads.
                if (by_engine and kind_live == "cutscene" and not hud_checked and elapsed >= 12.0 and source
                        and cur_state == "gameplay" and cutscene_secs < args.min_cutscene):
                    # ...and only when it never really was a cutscene: one that ran as a cutscene and then handed
                    # over is ended by the handover rule below, whose settle time leaves the boss bar (s03a70l)
                    # on the last frame instead of cutting at the instant the HUD came up
                    hud_checked = True
                    probe_still = os.path.join(SHOTS, sid + "_c.jpg")
                    if shot(cl, source, probe_still) and still_kind(probe_still) == "gameplay":
                        kind_live = "gameplay"
                        try:
                            os.replace(probe_still, os.path.join(SHOTS, sid + "_b.jpg")); shot_b = True
                        except OSError:
                            pass
                    else:
                        try:
                            os.remove(probe_still)
                        except OSError:
                            pass
                # the caps
                if kind_live == "gameplay" and elapsed >= (DEMO_GAMEPLAY_CAP if by_engine else args.cap_gameplay):
                    end_reason = ("gameplay behind a demo at boot, %.0fs cap" % DEMO_GAMEPLAY_CAP if by_engine
                                  else "gameplay, %.0fs cap" % args.cap_gameplay); break
                if kind_live == "cutscene":
                    if (cur_state == "gameplay" and now - since >= args.settle
                            and cutscene_secs >= args.min_cutscene):
                        end_reason = "handed over to gameplay after %.0fs of cutscene" % cutscene_secs; break
                    if elapsed >= args.cap_cutscene:
                        end_reason = "cutscene, %.0fs cap" % args.cap_cutscene; break
                elif kind_live in ("codec", "") and elapsed >= args.cap_other:
                    end_reason = "%s, %.0fs cap" % (kind_live or "undecided", args.cap_other); break
                time.sleep(0.5)
            if source and game_up() and dup_of is None:
                if not shot_b:
                    shot_b = shot(cl, source, os.path.join(SHOTS, sid + "_b.jpg"))
                # the last frame, for what only shows up late: a boss's health bar, an item card, the HUD after a
                # cutscene's handover. tools/hud_read.py reads these.
                shot(cl, source, os.path.join(SHOTS, sid + "_z.jpg"))
        elif rec_started and (proc.poll() is None or start_entry) and dumps_count() == before and game_up():
            # alive but never drew a 3D frame: a video, a menu, black - or the bundled MGS1 running in its own
            # process. Keep a still of it and a short recording.
            if mgs1_up():
                kind_live, end_reason = "mgs1", "the bundled MGS1 (mgs1.exe) is running"
            elif start_entry:
                kind_live, end_reason = "start", "a game-start entry: recorded %.0fs of it" % args.cap_other
                time.sleep(max(0.0, args.cap_other - (time.time() - rec_started)))
            if source:
                shot_a = shot(cl, source, os.path.join(SHOTS, sid + "_a.jpg"))
                shot_b = shot(cl, source, os.path.join(SHOTS, sid + "_b.jpg"))

        out_path = None
        if rec_started is not None:
            try:
                out_path = cl.stop_record().output_path
            except Exception as e:
                print("   stop_record failed:", e)
        try:
            proc.kill()
        except Exception:
            pass
        kill_game(); time.sleep(1.5)

        # what the engine thought it was drawing - the video/scene discriminator - and what it loaded
        state, draws, hud = "", -1, -1
        fp = dict(demo="", demo_late="", env="", movie="", boss="")
        if os.path.exists(ADDON_LOG):
            shutil.copyfile(ADDON_LOG, os.path.join(SHOTS, sid + ".addon.log"))
            text = open(ADDON_LOG, errors="replace").read()
            hits = STATE_RE.findall(text)
            if hits:
                state, draws, hud = hits[-1][0], int(hits[-1][1]), int(hits[-1][2])
            fp = assets_of(text)

        after, tail = dumps_count(), tail_since(LAUNCH_LOG, lmark)
        if video_scene and "FIRST-3D-FRAME" not in (text if os.path.exists(ADDON_LOG) else "") and kind_live != "duplicate":
            kind_live = "video"       # played to the end (or the cap) without ever drawing a 3D frame
        if after > before:
            result, detail = "crash", "new minidump"
        elif scene_at:
            result, detail = "boot", "3D frame reached" if not video_scene else "video opened"
        else:
            result, detail = "no-scene", "no 3D frame in %ds" % int(time.time() - t0)

        # 4. keep it, trimmed so it opens on the loading screen - unless it is a repeat, which keeps nothing
        offset, scene_in, final = -1.0, -1.0, ""
        if dup_of is not None and out_path and os.path.exists(out_path):
            os.remove(out_path)
            for suf in ("_a.jpg", "_b.jpg", "_z.jpg"):
                try:
                    os.remove(os.path.join(SHOTS, sid + suf))
                except OSError:
                    pass
        elif out_path and os.path.exists(out_path):
            dst = os.path.join(VIDEO, sid + ".mkv")
            if scene_at:
                offset = round(scene_at - rec_started, 2)
                # ...whichever is later: the fixed desktop trim, or `lead` seconds before the scene.
                cut = max(DESKTOP_TRIM, offset - args.lead)
                # Where the scene begins *inside the file that is kept* - everything downstream (sheets,
                # fingerprints) works from the saved video, not from the original recording.
                scene_in = round(offset - cut, 2)
                if trim(out_path, dst, cut):
                    os.remove(out_path)
                else:
                    shutil.move(out_path, dst); scene_in = offset
            else:
                # no scene: a video or a menu. `cap-other` seconds of it after the desktop is gone.
                if trim(out_path, dst, DESKTOP_TRIM, args.cap_other + args.lead):
                    os.remove(out_path)
                else:
                    shutil.move(out_path, dst)
            final = sid + ".mkv"

        tally[result] = tally.get(result, 0) + 1
        k = early_key(fp)
        if k and final and dup_of is None:
            seen.setdefault(k, sid)
        with open(args.out, "a", newline="") as fh:
            csv.DictWriter(fh, FIELDS).writerow(dict(
                id=sid, result=result, seconds=int(time.time() - t0), scene_offset=offset,
                scene_at=scene_in, video=final, state=state, draws=draws, hud=hud, kind_live=kind_live,
                detail=(detail if result != "boot" else end_reason), **fp))
        print("[%4d/%d] %-14s %-9s %4ds %-9s d%-4s env:%-14s %-8s draws:%-5s %s %s  boot %d / crash %d / no-scene %d"
              % (n, len(todo), sid, result, int(time.time() - t0), kind_live or "-", fp["demo"] or "-",
                 fp["env"] or "-", state, draws, fp["movie"], ("boss:" + fp["boss"]) if fp["boss"] else "",
                 tally["boot"], tally["crash"], tally["no-scene"]),
              flush=True)

    print("\ndone: boot %d, crash %d, no-scene %d -> %s"
          % (tally["boot"], tally["crash"], tally["no-scene"], args.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
