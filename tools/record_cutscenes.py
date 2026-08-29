"""Unattended recorder for MGS4's in-game cutscenes.

For every cutscene entry of the stage table (`<stage>_D<n>`, chronological) it boots the game straight into the
cutscene, records it with OBS (3840x2160, 60 fps, NVENC AV1) and stops when the cutscene is over. A virtual
DualShock 4 taps Cross throughout, both to get through the auto-save / "press any button" prompts and to trigger the
flashback sequences inside the cutscenes.

End of a cutscene is decided from two signals:
  * the add-on's SCENE-STATE log (cutscene = 3D scene without HUD, gameplay = HUD visible, no-3d = menu/loading/video)
  * the recording's byte rate: a static screen (continue prompt, act result, black) compresses to almost nothing,
    while a prerecorded video inside a demo keeps the bitrate high, so videos are not mistaken for the end.

  python record_cutscenes.py [--stages a,b,c] [--limit N] [--max-minutes 30] [--min-free-gb 100] [--dry-run]
"""
import argparse, csv, json, os, re, subprocess, sys, time, shutil, ctypes

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from winfocus import find_window, focus, is_foreground          # noqa: E402
from ds4 import DS4, CROSS                                       # noqa: E402
import obs_control as obsc                                       # noqa: E402

GAME_DIR = r"C:\Program Files (x86)\Steam\steamapps\common\METAL GEAR SOLID 4\MGS4"
GAME_EXE = os.path.join(GAME_DIR, "mgs4.exe")
ADDON_LOG = os.path.join(GAME_DIR, "logs", "mgs4_dlss.log")
OUT_DIR = r"D:\mgs4-dlss5"
STATE_RE = re.compile(r"SCENE-STATE (cutscene|gameplay|no-3d)")
# heartbeat: SCENE-STATE-TICK <state> (frame N, scene draws X, HUD draws Y, post-skipped Z, ...)
TICK_RE = re.compile(r"SCENE-STATE-TICK (cutscene|gameplay|no-3d) \(frame (\d+), scene draws (\d+), HUD draws (\d+)")

ACTS = {"00": "prologue", "01": "act1-middle-east", "02": "act2-south-america", "03": "act3-eastern-europe",
        "04": "act4-shadow-moses", "05": "act5-outer-haven", "10": "interlude", "20": "briefing", "30": "epilogue"}


def log(msg):
    line = time.strftime("[%H:%M:%S] ") + msg
    print(line, flush=True)
    with open(os.path.join(OUT_DIR, "recording.log"), "a", encoding="utf-8") as f:
        f.write(line + "\n")


def free_gb(drive="D:\\"):
    free = ctypes.c_ulonglong(0)
    ctypes.windll.kernel32.GetDiskFreeSpaceExW(ctypes.c_wchar_p(drive), None, None, ctypes.byref(free))
    return free.value / (1024 ** 3)


def natural_key(entry):
    m = re.match(r"s(\d\d)a(\d\d)([a-z]?)_D(\d+)", entry)
    if m:
        return (m.group(1), m.group(2), m.group(3), int(m.group(4)))
    return (entry[1:3], entry[4:6], "", 0)


def cutscene_entries():
    rows = list(csv.DictReader(open(os.path.join(HERE, "scenes.csv"), encoding="utf-8")))
    ents = [r["stage_entry"] for r in rows if r["kind"] == "cutscene"]
    return sorted(ents, key=natural_key)


class Tail:
    """Reads only the add-on log lines produced since the game was started."""

    def __init__(self, path):
        self.path = path
        self.pos = os.path.getsize(path) if os.path.exists(path) else 0

    def new_lines(self):
        if not os.path.exists(self.path):
            return []
        size = os.path.getsize(self.path)
        if size < self.pos:      # log replaced
            self.pos = 0
        with open(self.path, "r", encoding="utf-8", errors="replace") as f:
            f.seek(self.pos)
            data = f.read()
            self.pos = f.tell()
        return data.splitlines()


def kill_game():
    subprocess.run(["taskkill", "/IM", "mgs4.exe", "/F"], capture_output=True)
    time.sleep(3)


def start_game(stage, width=3840, height=2160):
    # the port takes the render resolution on the command line, so every recording is a real 4K one
    subprocess.Popen([GAME_EXE, "--stage", stage, "--res_width", str(width), "--res_height", str(height)], cwd=GAME_DIR)
    hwnd = None
    for _ in range(90):
        hwnd = find_window()
        if hwnd:
            break
        time.sleep(1)
    return hwnd


def record_one(cl, pad, index, stage, args):
    """Returns a dict describing the attempt."""
    act = ACTS.get(stage[1:3], "other")
    info = {"index": index, "stage": stage, "act": act, "status": "?", "seconds": 0, "file": "", "end_reason": ""}
    tail = Tail(ADDON_LOG)
    log(f"--- [{index:02d}] {stage} ({act})")
    kill_game()
    hwnd = start_game(stage, args.width, args.height)
    if not hwnd:
        info["status"] = "no-window"; return info
    state, t0 = "unknown", time.time()
    # phase 1: get into the cutscene (auto-save notice, "press any button", loading)
    while time.time() - t0 < args.start_timeout:
        if not is_foreground(hwnd):
            focus(hwnd)
        pad.tap(hold=0.12)
        for line in tail.new_lines():
            m = STATE_RE.search(line)
            if m:
                state = m.group(1)
        if state == "cutscene":
            break
        time.sleep(0.5)
    if state != "cutscene":
        log(f"    no cutscene started within {args.start_timeout}s (state={state})")
        info["status"] = "no-cutscene"; info["end_reason"] = state
        kill_game(); return info

    # phase 2: record
    try:
        if cl.get_record_status().output_active:      # a previous run may have been killed mid-recording
            log("    OBS was still recording; stopping that first")
            cl.stop_record(); time.sleep(2)
    except Exception:
        pass
    for attempt in range(3):
        try:
            cl.start_record(); break
        except Exception as e:
            log(f"    StartRecord failed ({e}); retrying")
            try:
                cl.stop_record()
            except Exception:
                pass
            time.sleep(3)
    else:
        info["status"] = "obs-error"; kill_game(); return info
    t_rec = time.time()
    last_bytes, last_check, static_since, gameplay_since, frozen_since = 0, time.time(), None, None, None
    baseline_draws, busy_ticks = None, 0     # gameplay draws far more than a cutscene: a second, independent signal
    end_reason = "max-duration"
    while time.time() - t_rec < args.max_minutes * 60:
        if not is_foreground(hwnd):
            focus(hwnd)
        pad.tap(hold=0.12)                      # flashback prompts
        time.sleep(args.press_period)
        if find_window() is None:
            end_reason = "game-exited"; break
        for line in tail.new_lines():
            t = TICK_RE.search(line)
            if t:
                draws, hud = int(t.group(3)), int(t.group(4))
                if time.time() - t_rec < 60:
                    baseline_draws = max(baseline_draws or 0, draws)   # busiest reading of the opening minute
                elif baseline_draws and draws > max(1200, baseline_draws * 3.0):
                    busy_ticks += 1
                    if busy_ticks >= 4:             # sustained for ~40 s
                        log(f"    draw count {draws} vs cutscene baseline {baseline_draws} -> gameplay")
                        end_reason = "gameplay-drawcount"; break
                else:
                    busy_ticks = 0
                if hud > 0 and time.time() - t_rec > args.min_seconds:
                    log(f"    HUD draws {hud} in tick -> gameplay")
                    end_reason = "gameplay-hud"; break
                continue
            m = STATE_RE.search(line)
            if m:
                state = m.group(1)
                log(f"    state -> {state} at {time.time() - t_rec:.0f}s")
                if state == "cutscene":
                    static_since = gameplay_since = None
        if end_reason in ("gameplay-drawcount", "gameplay-hud"):
            break
        now = time.time()
        if now - last_check >= 5:
            st = cl.get_record_status()
            rate = (st.output_bytes - last_bytes) / (now - last_check)
            last_bytes, last_check = st.output_bytes, now
            moving = rate > args.moving_bytes_per_s
            if now - t_rec < args.min_seconds:
                # the HUD flickers on for a moment at the start of some cutscenes; never end on that
                static_since = gameplay_since = frozen_since = None
                continue
            if rate < args.frozen_bytes_per_s:      # frozen picture regardless of the classified state
                frozen_since = frozen_since or now
                if now - frozen_since >= args.static_grace:
                    end_reason = "frozen-picture"; break
            else:
                frozen_since = None
            if state == "gameplay":
                gameplay_since = gameplay_since or now
                if now - gameplay_since >= args.gameplay_grace:
                    end_reason = "gameplay-started"; break
            elif state == "no-3d" and not moving:
                static_since = static_since or now
                if now - static_since >= args.static_grace:
                    end_reason = "static-screen"; break
            else:
                static_since = None
    r = cl.stop_record()
    time.sleep(2)
    src = getattr(r, "output_path", None)
    secs = time.time() - t_rec
    info["seconds"] = round(secs, 1); info["end_reason"] = end_reason
    if src and os.path.exists(src):
        name = f"{index:02d}_{stage}_{act}.mkv"
        dst = os.path.join(OUT_DIR, name)
        try:
            shutil.move(src, dst)
        except Exception as e:
            log(f"    rename failed ({e}); keeping {src}")
            dst = src
        info["file"] = os.path.basename(dst)
        info["mb"] = round(os.path.getsize(dst) / 1048576, 1)
        info["status"] = "ok"
        log(f"    recorded {secs:.0f}s -> {info['file']} ({info['mb']} MB, end: {end_reason})")
    else:
        info["status"] = "no-file"
        log(f"    recording produced no file (end: {end_reason})")
    kill_game()
    return info


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stages", default="")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--skip", type=int, default=0)
    ap.add_argument("--max-minutes", type=float, default=30)
    ap.add_argument("--min-free-gb", type=float, default=100)
    ap.add_argument("--start-timeout", type=float, default=100)
    ap.add_argument("--press-period", type=float, default=1.0)
    ap.add_argument("--gameplay-grace", type=float, default=9)
    ap.add_argument("--static-grace", type=float, default=25)
    ap.add_argument("--moving-bytes-per-s", type=float, default=700_000)
    ap.add_argument("--frozen-bytes-per-s", type=float, default=300_000)
    ap.add_argument("--min-seconds", type=float, default=30)
    ap.add_argument("--width", type=int, default=3840)
    ap.add_argument("--height", type=int, default=2160)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    os.makedirs(OUT_DIR, exist_ok=True)
    entries = [e.strip() for e in args.stages.split(",") if e.strip()] or cutscene_entries()
    if args.skip:
        entries = entries[args.skip:]
    if args.limit:
        entries = entries[:args.limit]
    log(f"=== recording {len(entries)} cutscenes, max {args.max_minutes} min each, stop below {args.min_free_gb} GB free")
    if args.dry_run:
        for i, e in enumerate(entries, 1):
            print(f"{i:02d} {e} {ACTS.get(e[1:3])}")
        return

    cl = obsc.client()
    obsc.ensure_sources(cl, "METAL GEAR SOLID 4 GUNS OF THE PATRIOTS:GLFW30:mgs4.exe")
    cl.set_current_program_scene(obsc.SCENE)
    pad = DS4()
    results_path = os.path.join(OUT_DIR, "recordings.json")
    results = json.load(open(results_path)) if os.path.exists(results_path) else []
    done = {r["stage"] for r in results if r.get("status") in ("ok", "no-cutscene")}
    try:
        for i, stage in enumerate(entries, 1):
            if stage in done:
                log(f"--- [{i:02d}] {stage} already recorded, skipping"); continue
            gb = free_gb()
            if gb < args.min_free_gb:
                log(f"STOPPING: only {gb:.1f} GB free on D: (floor {args.min_free_gb} GB)")
                break
            info = record_one(cl, pad, i, stage, args)
            info["free_gb_after"] = round(free_gb(), 1)
            results.append(info)
            json.dump(results, open(results_path, "w"), indent=1)
            log(f"    free space now {info['free_gb_after']} GB")
            try:
                obsc.fit_game(cl)
            except Exception:
                pass
    finally:
        try:
            if cl.get_record_status().output_active:
                cl.stop_record()
        except Exception:
            pass
        pad.close()
        kill_game()
        ok = [r for r in results if r.get("status") == "ok"]
        log(f"=== done: {len(ok)} recordings, {sum(r.get('seconds', 0) for r in ok)/60:.1f} min, {free_gb():.1f} GB free")


if __name__ == "__main__":
    main()
