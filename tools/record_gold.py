"""Gold recordings of MGS4's in-engine cutscenes: 3840x2160, 60 fps, NVENC AV1, into <MGS4_OUT>\\gold\\raw.

For each scene (the curated list below, story order) the game boots straight into the cutscene, OBS records from the
boot prompts on, a virtual DualShock 4 taps Cross only until the add-on reports the cutscene (Cross during a scene
triggers flashbacks; Options would skip it), and the recording stops at the first of:
  * the game exits (the port quits after a demo that has no continuation),
  * the add-on reports gameplay (HUD) for 5 s,
  * a static no-3D screen (act result / continue prompt / load) held for 120 s - a load followed by the cutscene
    resuming never ends the scene,
  * the hard cap.
Prerecorded (Bink) demos are recognized (moving picture, never a 3D cutscene) and skipped. Wall-clock times of the
scene's start and end are stored with each raw clip; finalize_gold.py trims to the exact frames from the picture.

  python record_gold.py [--stages a,b] [--limit N] [--cap-minutes 20] [--dry-run]
"""
import argparse, json, os, re, subprocess, sys, time, shutil

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from winfocus import find_window, focus, is_foreground, abort_requested   # noqa: E402
from ds4 import DS4                                              # noqa: E402
import obs_control as obsc                                       # noqa: E402
from letterbox import read_ppm                                   # noqa: E402
from screen_signals import classify                              # noqa: E402
import paths                                                     # noqa: E402
PROBE_PATH = os.path.join(os.environ.get("TEMP", "."), "mgs4_gold_probe.ppm")


def hud_on_screen(cl):
    """True when the capture shows the gameplay HUD (orange OLD SNAKE bar); None if the probe failed."""
    try:
        cl.save_source_screenshot(obsc.CAPTURE, "ppm", PROBE_PATH, 320, 180, -1)
        img = read_ppm(PROBE_PATH)
    except Exception:
        return None
    if not img: return None
    w, h, px = img
    return classify(px, w, h)[0] == "gameplay"

GAME_DIR = paths.GAME_DIR
GAME_EXE = paths.GAME_EXE
ADDON_LOG = paths.ADDON_LOG
GOLD = paths.GOLD
RAW = os.path.join(GOLD, "raw")
STATE_RE = re.compile(r"^\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\] SCENE-STATE (cutscene|gameplay|no-3d)")
WINDOW_RE = re.compile(r"^\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\] scene window: on")
TICK_RE = re.compile(r"^\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\] SCENE-STATE-TICK (cutscene|gameplay|no-3d) \(frame (\d+), scene draws (\d+), HUD draws (\d+)")

# the curated scenes, story order (from the "greatest" shortcut set)
SCENES = [
    # the user's picks (2026-08-30), story order; Prologue and the briefing/database screens left out
    ("s01a10l_D1", "Act 1 - Codec With Otacon"),
    ("s01a10l_D2", "Act 1 - Metal Gear Mk. II Introduced"),
    ("s01a30l_D2", "Act 1 - Militia Ambush"),
    ("s01a40l_D2", "Act 1 - Rat Patrol In The Garage"),
    ("s01a40l_D3", "Act 1 - Snake Meets Meryl"),
    ("s01a60l_D1", "Act 1 - Nomad Debriefing"),
    ("s01a60l_D2", "Act 1 - Ending Battlefield"),
    ("s02a10l_D1", "Act 2 - South America Insertion"),
    ("s02a10l_D2", "Act 2 - Jungle Patrol"),
    ("s02a25l_D1", "Act 2 - Snake And The Monkey"),
    ("s02a25l_D2", "Act 2 - Rebel Camp"),
    ("s02a50l_D1", "Act 2 - Naomi In The Lab"),
    ("s02a70l_D", "Act 2 - Naomi Taken By The PMCs"),
    ("s02a95l_D", "Act 2 - Raiden Versus Vamp"),
    ("s03a00l_D1", "Act 3 - Meryl In Eastern Europe"),
    ("s03a30l_D1", "Act 3 - The Cathedral"),
    ("s03a70l_D1", "Act 3 - Climax"),
    ("s03a90l_D1", "Act 3 - Liquid On The Ship"),
    ("s04a10l_D1", "Act 4 - Shadow Moses Snowstorm"),
    ("s04a30l_D1", "Act 4 - Inside The Base"),
    ("s04a60l_D1", "Act 4 - REX Versus RAY"),
    ("s04a60l_D2", "Act 4 - After The REX Fight"),
    ("s04a60l_D3", "Act 4 - Liquid Leaves Shadow Moses"),
    ("s04a75l_D1", "Act 4 - Old Snake In The Rain"),
    ("s05a20l_D1", "Act 5 - Outer Haven"),
    ("s05a45l_D", "Act 5 - Before The Finale"),
    ("s05a50l_D", "Act 5 - Liquid Ocelot Finale"),
]


def log(msg):
    line = time.strftime("[%H:%M:%S] ") + msg
    print(line, flush=True)
    with open(os.path.join(GOLD, "gold.log"), "a", encoding="utf-8") as f:
        f.write(line + "\n")


def free_gb():
    """Free space where the recordings go."""
    return paths.free_gb()


def log_time(m, base):
    """Wall-clock seconds (time.time() scale) of an add-on log timestamp, assuming it is within a day of `base`."""
    h, mi, s, ms = int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4))
    lt = time.localtime(base)
    day = time.mktime((lt.tm_year, lt.tm_mon, lt.tm_mday, 0, 0, 0, 0, 0, -1))
    t = day + h * 3600 + mi * 60 + s + ms / 1000.0
    if t < base - 3600: t += 86400          # the log rolled past midnight
    return t


class Tail:
    def __init__(self, path):
        self.path = path
        self.pos = os.path.getsize(path) if os.path.exists(path) else 0

    def new_lines(self):
        if not os.path.exists(self.path): return []
        size = os.path.getsize(self.path)
        if size < self.pos: self.pos = 0
        with open(self.path, "r", encoding="utf-8", errors="replace") as f:
            f.seek(self.pos); data = f.read(); self.pos = f.tell()
        return data.splitlines()


def kill_game():
    subprocess.run(["taskkill", "/IM", "mgs4.exe", "/F"], capture_output=True)
    time.sleep(3)


def start_game(stage):
    subprocess.Popen([GAME_EXE, "--stage", stage, "--res_width", "3840", "--res_height", "2160"], cwd=GAME_DIR)
    for _ in range(90):
        h = find_window()
        if h: return h
        time.sleep(1)
    return None


def stop_and_take(cl, dst_base):
    """Stops the recording and moves the file to dst_base + .mkv; returns the path or None."""
    try:
        r = cl.stop_record()
    except Exception as e:
        log(f"    stop_record failed: {e}"); return None
    src = getattr(r, "output_path", None)
    for _ in range(40):                     # the muxer finishes after StopRecord returns
        if src and os.path.exists(src) and os.path.getsize(src) > 0: break
        time.sleep(0.25)
    time.sleep(1.0)
    if not src or not os.path.exists(src): return None
    dst = dst_base + ".mkv"
    for _ in range(60):                     # OBS keeps the file open until the muxer has finished
        try:
            if os.path.exists(dst): os.remove(dst)
            os.replace(src, dst); return dst
        except PermissionError:
            time.sleep(1.0)
    log(f"    could not move {src} (still open); keeping it there"); return src


def record_one(cl, pad, index, stage, title, args):
    info = {"index": index, "stage": stage, "title": title, "status": "?", "end_reason": ""}
    tail = Tail(ADDON_LOG)
    log(f"--- [{index:02d}] {stage}  {title}")
    kill_game()
    hwnd = start_game(stage)
    if not hwnd:
        info["status"] = "no-window"; return info
    focus(hwnd); time.sleep(0.5)
    if obsc.CAPTURE == obsc.GAME_SOURCE:   # the hook needs a few seconds after the window appears
        items = {i["sourceName"]: i["sceneItemId"] for i in cl.get_scene_item_list(obsc.SCENE).scene_items}
        cl.set_scene_item_enabled(obsc.SCENE, items[obsc.GAME_SOURCE], True); cl.set_scene_item_enabled(obsc.SCENE, items[obsc.DISPLAY_SOURCE], False)
        t_hook = time.time()
        while time.time() - t_hook < 45:
            tr = cl.get_scene_item_transform(obsc.SCENE, items[obsc.GAME_SOURCE]).scene_item_transform
            if tr["sourceWidth"]: break
            if not is_foreground(hwnd): focus(hwnd)
            time.sleep(0.5)
        log(f"    game capture {tr['sourceWidth']:.0f}x{tr['sourceHeight']:.0f} after {time.time() - t_hook:.0f}s")
        if tr["sourceWidth"]:
            try: obsc.fit_game(cl)
            except Exception as e: log(f"    fit_game: {e}")
        else:   # no hook: this scene goes through the display capture
            log("    game capture did not hook - using the display capture for this scene")
            cl.set_scene_item_enabled(obsc.SCENE, items[obsc.GAME_SOURCE], False); cl.set_scene_item_enabled(obsc.SCENE, items[obsc.DISPLAY_SOURCE], True)
            info["capture"] = "display-fallback"
    try:
        if cl.get_record_status().output_active: cl.stop_record(); time.sleep(2)
    except Exception: pass
    try: st0 = cl.get_stats(); stats0 = (st0.output_skipped_frames, st0.render_skipped_frames, st0.output_total_frames)
    except Exception: stats0 = None
    cl.start_record(); t_rec = time.time()
    info["t_rec"] = t_rec
    state, t_state, ticks_no3d_moving = "unknown", None, 0
    last_bytes, last_check = 0, time.time()
    cut_start = None
    # phase 1: through the prompts into the cutscene. No presses for the first seconds: the NVIDIA app's "Alt+Z"
    # toast and Steam's notifications sit on the screen right after launch, and the game waits at its prompts.
    t0 = time.time()
    while time.time() - t0 < args.start_timeout:
        if abort_requested(): info["status"] = "aborted"; break
        if not is_foreground(hwnd): focus(hwnd)
        if time.time() - t0 >= args.prompt_delay: pad.tap(hold=0.07)
        time.sleep(0.16)
        for line in tail.new_lines():
            m = STATE_RE.search(line)
            if m: state, t_state = m.group(5), log_time(m, t_rec)
            w = WINDOW_RE.search(line)   # a 3D window (Codec caller) = an in-engine Codec scene, not a video
            if w and cut_start is None and time.time() - t0 > args.prompt_delay: cut_start = log_time(w, t_rec); log("    3D window scene (Codec) detected: treating it as the cutscene")
        if cut_start is not None: break
        if state == "cutscene": cut_start = t_state; break
        if state == "gameplay" and t_state and time.time() - t_state > 2.0:
            hud = hud_on_screen(cl)
            if hud is False: cut_start = t_state; log("    3D scene reads as 'gameplay' but no HUD on screen: treating it as the cutscene"); break
        if find_window() is None: info["status"] = "game-exited-early"; break
        now = time.time()
        if now - last_check >= 5:
            st = cl.get_record_status(); rate = (st.output_bytes - last_bytes) / (now - last_check); last_bytes, last_check = st.output_bytes, now
            # a prerecorded demo: never a 3D scene, but a moving picture (high byte rate) for a while
            if state == "no-3d" and rate > args.moving_bytes_per_s and now - t0 > 25: ticks_no3d_moving += 1
            if ticks_no3d_moving >= 9: info["status"] = "prerecorded"; break   # ~70 s of moving no-3D picture
    if cut_start is None:
        if info["status"] == "?": info["status"] = "no-cutscene"
        log(f"    {info['status']} (state {state}) - discarding")
        p = stop_and_take(cl, os.path.join(RAW, f"_discard_{stage}"))
        if p: os.remove(p)
        kill_game(); return info
    info["cut_start"] = cut_start
    log(f"    cutscene started {cut_start - t_rec:.1f}s into the recording")
    # phase 2: the scene. No presses while it plays.
    scene_end, end_reason = None, "cap"
    left_cut_at, gameplay_since, no3d_since = None, None, None
    hud_seen, last_hud_seen, last_probe = None, 0.0, 0.0
    t_cut = time.time()
    while time.time() - t_cut < args.cap_minutes * 60:
        if abort_requested(): end_reason = "aborted"; break
        time.sleep(0.25)
        if find_window() is None:
            end_reason = "game-exited"; scene_end = left_cut_at or time.time(); break
        for line in tail.new_lines():
            m = STATE_RE.search(line)
            if not m: continue
            new, t = m.group(5), log_time(m, t_rec)
            if new != state: log(f"    state {state} -> {new} at {t - t_rec:.1f}s")
            if new == "cutscene": left_cut_at = None; gameplay_since = no3d_since = hud_seen = None
            elif state == "cutscene" or left_cut_at is None: left_cut_at = t
            state = new
        now = time.time()
        if state == "gameplay":
            if now - last_probe >= 1.0:
                last_probe = now
                if hud_on_screen(cl): hud_seen = hud_seen or now; last_hud_seen = now
                elif hud_seen and now - last_hud_seen > 3.0: hud_seen = None
            if hud_seen and now - hud_seen >= 5 and now - t_cut > args.min_seconds:
                end_reason = "gameplay"; scene_end = min(left_cut_at or hud_seen, hud_seen); break
            # 'gameplay' without the HUD bar for long = an interactive Codec conversation waiting for input: the scene
            # proper is over (its last cutscene frame is the end); no presses, they would skip the dialogue
            gameplay_since = gameplay_since or now
            if now - gameplay_since >= args.codec_seconds and now - t_cut > args.min_seconds:
                end_reason = "codec-interactive"; scene_end = left_cut_at or gameplay_since; break
        elif state == "no-3d":
            no3d_since = no3d_since or now
            if now - no3d_since > 5:            # results / continue prompts: Cross gets through them; a load resumes the scene
                if not is_foreground(hwnd): focus(hwnd)
                pad.tap(hold=0.07)
            if now - no3d_since >= args.static_seconds:
                end_reason = "static"; scene_end = left_cut_at or now; break
    if scene_end is None: scene_end = left_cut_at or time.time()
    info["scene_end"] = scene_end; info["end_reason"] = end_reason
    info["scene_seconds"] = round(scene_end - cut_start, 1)
    base = os.path.join(RAW, f"{index:02d}_{stage}")
    try:
        st1 = cl.get_stats(); info["obs"] = {"encoder_skipped": st1.output_skipped_frames - (stats0[0] if stats0 else 0), "render_skipped": st1.render_skipped_frames - (stats0[1] if stats0 else 0), "frames": st1.output_total_frames - (stats0[2] if stats0 else 0)}
        log(f"    OBS: {info['obs']}")
    except Exception as e: log(f"    OBS stats: {e}")
    p = stop_and_take(cl, base)
    kill_game()
    if not p: info["status"] = "no-file"; log("    no recording file"); return info
    try: shutil.copyfile(ADDON_LOG, base + ".log")
    except Exception: pass
    info["raw"] = p; info["status"] = "ok"; info["mb"] = round(os.path.getsize(p) / 1048576, 1)
    log(f"    scene {info['scene_seconds']:.0f}s (end: {end_reason}), raw {os.path.basename(p)} {info['mb']} MB")
    return info


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stages", default="")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--cap-minutes", type=float, default=20)
    ap.add_argument("--min-seconds", type=float, default=20)
    ap.add_argument("--static-seconds", type=float, default=120)
    ap.add_argument("--codec-seconds", type=float, default=45, help="'gameplay' state without a HUD bar for this long ends the scene (interactive Codec)")
    ap.add_argument("--start-timeout", type=float, default=150)
    ap.add_argument("--prompt-delay", type=float, default=12, help="seconds after launch before the first button press (overlay toasts)")
    ap.add_argument("--moving-bytes-per-s", type=float, default=700_000)
    ap.add_argument("--min-free-gb", type=float, default=100)
    ap.add_argument("--redo", action="store_true", help="record again even if a scene already has a raw clip")
    ap.add_argument("--capture", default="game", choices=["game", "display"], help="OBS source: game capture (every presented frame, no overlays) or display capture")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()
    paths.require_game()
    os.makedirs(RAW, exist_ok=True)
    wanted = [s.strip() for s in args.stages.split(",") if s.strip()]
    scenes = [(i, st, ti) for i, (st, ti) in enumerate(SCENES, 1) if not wanted or st in wanted]
    if args.limit: scenes = scenes[:args.limit]
    if args.dry_run:
        for i, st, ti in scenes: print(f"{i:02d} {st:12s} {ti}")
        return
    results_path = os.path.join(GOLD, "results.json")
    results = json.load(open(results_path)) if os.path.exists(results_path) else []
    done = {r["stage"] for r in results if r.get("status") in ("ok", "prerecorded")}
    log(f"=== gold run: {len(scenes)} scenes, cap {args.cap_minutes} min, {free_gb():.0f} GB free")
    cl = obsc.client()
    cl.set_current_program_scene(obsc.SCENE)
    try:
        if cl.get_record_status().output_active: log("OBS was still recording (a killed run); stopping it"); cl.stop_record(); time.sleep(3)
    except Exception as e: log(f"record status: {e}")
    try: cl.set_record_directory(RAW)
    except Exception as e: log(f"set_record_directory: {e} (recordings will land in OBS's current folder)")
    items = {i["sourceName"]: i["sceneItemId"] for i in cl.get_scene_item_list(obsc.SCENE).scene_items}
    if args.capture == "game":
        cl.set_input_settings(obsc.GAME_SOURCE, {"capture_mode": "window", "window": "METAL GEAR SOLID 4 GUNS OF THE PATRIOTS:GLFW30:mgs4.exe", "priority": 2, "capture_overlays": False, "capture_cursor": False, "hook_rate": 0}, True)
        obsc.CAPTURE = obsc.GAME_SOURCE
    cl.set_scene_item_enabled(obsc.SCENE, items[obsc.GAME_SOURCE], args.capture == "game")
    cl.set_scene_item_enabled(obsc.SCENE, items[obsc.DISPLAY_SOURCE], args.capture == "display")
    log(f"capture source: {obsc.CAPTURE}")
    pad = DS4(); time.sleep(1.0)
    try:
        for i, st, ti in scenes:
            if st in done and not args.redo: log(f"--- [{i:02d}] {st} already done, skipping"); continue
            if free_gb() < args.min_free_gb: log("STOPPING: disk space floor reached"); break
            info = record_one(cl, pad, i, st, ti, args)
            results = [r for r in results if r.get("stage") != st] + [info]
            json.dump(results, open(results_path, "w"), indent=1)
            if info.get("status") == "aborted" or info.get("end_reason") == "aborted": log("stopped by Escape"); break
    finally:
        try:
            if cl.get_record_status().output_active: cl.stop_record()
        except Exception: pass
        pad.close(); kill_game()
        ok = [r for r in results if r.get("status") == "ok"]
        log(f"=== done: {len(ok)} scenes recorded, {sum(r.get('scene_seconds', 0) for r in ok) / 60:.1f} min of scenes")


if __name__ == "__main__":
    main()
