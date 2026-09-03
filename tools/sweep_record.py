"""Boots every stage id and records it through OBS: 4K60 AV1 with audio, one file per scene, each starting on the
loading screen a few seconds before the scene itself.

  python tools/sweep_record.py                     every catalog id
  python tools/sweep_record.py --ids my.txt        one id per line
  python tools/sweep_record.py --cap 180           seconds of scene to keep (default 180)
  python tools/sweep_record.py --lead 4            seconds of loading screen kept in front (default 4)

OBS rather than ffmpeg for two reasons that are not close: it captures **audio** (these are meant to be
watchable), and its Display Capture source sidesteps the Game Capture hook dropping to black for several seconds
whenever the game rebuilds its swapchain on a stage load - which is exactly when a cutscene starts. That lesson is
already written into tools/obs_control.py; it is not being relearned here.

**Where a recording starts.** Not at the desktop. The add-on says "swapchain created" the moment the game's window
exists, which is about 5 s ahead of the first rendered frame, and recording begins there. Afterwards the file is
trimmed losslessly to `lead` seconds before the scene proper, so every video opens on the loading screen rather
than on whatever was on the monitor. Trimming after the fact rather than trying to start the recorder at exactly
the right instant: OBS takes a moment to spin up, and a late start clips the opening of the scene, which is the
one part that cannot be recovered.

Output
  tools/stage_probe.csv          id,result,seconds,scene_offset,video,state,draws,hud,detail
  <MGS4_OUT>/sweep/video/<id>.mkv
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
SHOTS = os.path.join(paths.OUT_DIR, "sweep")
VIDEO = os.path.join(SHOTS, "video")
ADDON_LOG = os.path.join(paths.GAME_DIR, "logs", "mgs4_dlss.log")
LAUNCH_LOG = os.path.join(paths.GAME_DIR, "logs", "launcher.log")
DUMPS = os.path.join(paths.GAME_DIR, "crash_dumps")
FIELDS = ["id", "result", "seconds", "scene_offset", "scene_at", "video", "state", "draws", "hud", "detail"]

# The game's window does not cover the screen the instant the add-on says "swapchain created" - the desktop is
# still there for about a second. Every recording therefore loses at least this much off the front, whatever the
# scene offset works out to, so nothing opens on a stray window.
DESKTOP_TRIM = 1.6

STATE_RE = re.compile(r"SCENE-STATE(?:-TICK)? (cutscene|gameplay|no-3d) \(frame \d+, scene draws (\d+), HUD draws (\d+)")


def ps(cmd):
    subprocess.run(["powershell", "-NoProfile", "-Command", cmd], capture_output=True)


def kill_game():
    # mgs1 too: s04a05l starts the bundled MGS1 as its own process, which nothing else closes.
    ps("Get-Process mgs4,mgs1,mgs4-dlss-launcher -ErrorAction SilentlyContinue | Stop-Process -Force")


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


def trim(src, dst, start):
    """Copy the stream from `start` without re-encoding. AV1 in Matroska cuts at the nearest keyframe, which at
    OBS's default interval is within a couple of seconds - close enough for 'starts on the loading screen'."""
    r = subprocess.run([paths.FFMPEG, "-v", "error", "-ss", "%.2f" % max(0.0, start), "-i", src,
                        "-c", "copy", "-y", dst], capture_output=True)
    return r.returncode == 0 and os.path.exists(dst) and os.path.getsize(dst) > 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ids")
    ap.add_argument("--cap", type=float, default=180.0, help="seconds of scene to record")
    ap.add_argument("--lead", type=float, default=3.0, help="seconds of loading screen kept before the scene")
    ap.add_argument("--boot-timeout", type=float, default=55.0)
    ap.add_argument("--min-cutscene", type=float, default=12.0,
                    help="seconds of cutscene needed before a handover to gameplay ends the recording")
    ap.add_argument("--settle", type=float, default=8.0,
                    help="seconds of sustained gameplay that count as the cutscene having ended")
    ap.add_argument("--out", default=PROBE)
    args = ap.parse_args()

    os.makedirs(VIDEO, exist_ok=True)
    if args.ids:
        ids = [l.strip() for l in open(args.ids) if l.strip() and not l.startswith("#")]
    else:
        with open(os.path.join(HERE, "scenes.csv"), newline="") as fh:
            ids = [r["stage_entry"].strip() for r in csv.DictReader(fh) if r.get("stage_entry")]
    seen = []
    for i in ids:
        if i not in seen:
            seen.append(i)
    ids = seen

    done = {}
    if os.path.exists(args.out):
        with open(args.out, newline="") as fh:
            done = {r["id"]: r["result"] for r in csv.DictReader(fh)}
    else:
        with open(args.out, "w", newline="") as fh:
            csv.DictWriter(fh, FIELDS).writeheader()
    todo = [i for i in ids if i not in done]

    cl = oc.client()
    st = cl.get_record_status()
    if st.output_active:
        cl.stop_record(); time.sleep(1.5)
    print("sweep: %d ids, %d recorded, %d to go  (cap %.0fs, lead %.0fs)"
          % (len(ids), len(ids) - len(todo), len(todo), args.cap, args.lead))

    tally = {"boot": 0, "crash": 0, "no-scene": 0}
    for r in done.values():
        tally[r] = tally.get(r, 0) + 1

    for n, sid in enumerate(todo, 1):
        kill_game(); time.sleep(1)
        # Delete the add-on log before launching. The add-on recreates it (fopen "w") on every start, but until
        # the new process gets that far the file still holds the *previous* run - and reading that made every
        # trigger fire instantly on stale "swapchain created" / "FIRST-3D-FRAME" lines, so a known-crashing id
        # was recorded as a boot in 1 second. Absence is the only reliable "this run has not written yet".
        try:
            os.remove(ADDON_LOG)
        except OSError:
            pass
        before, lmark = dumps_count(), (os.path.getsize(LAUNCH_LOG) if os.path.exists(LAUNCH_LOG) else 0)
        t0 = time.time()
        proc = subprocess.Popen([LAUNCHER, sid, "--hold", str(int(args.cap + 15)), "--max-minutes", "6"])

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

        # 2. wait for the first 3D frame
        scene_at = None
        while time.time() - t0 < args.boot_timeout:
            tail = tail_since(ADDON_LOG, 0)
            if "FIRST-3D-FRAME" in tail:
                scene_at = time.time(); break
            if dumps_count() > before or proc.poll() is not None:
                break
            time.sleep(0.2)

        # 3. hold until the scene ends, the game dies, or the cap - whichever comes first.
        #    "Ends" is the cutscene handing over to gameplay and *staying* there: the two states flip back and
        #    forth for a few seconds around a transition (the add-on logs cutscene->gameplay->cutscene within
        #    three seconds on s01a30l), so a single reading means nothing and only a sustained one counts.
        end_reason = "cap"
        if scene_at:
            # A scene only counts as "a cutscene that ended" if it actually spent time being a cutscene. The
            # add-on reports one or two seconds of "cutscene" at the start of ordinary gameplay too, before the
            # HUD comes up, and treating that as a handover cut s01a00l off after 13 seconds.
            cutscene_secs, cur_state, since, last = 0.0, None, time.time(), time.time()
            while time.time() - scene_at < args.cap:
                if subprocess.run(["powershell", "-NoProfile", "-Command",
                                   "if (Get-Process mgs4 -ErrorAction SilentlyContinue) {'up'} else {'gone'}"],
                                  capture_output=True, text=True).stdout.strip() == "gone":
                    end_reason = "game exited"; break
                hits = STATE_RE.findall(tail_since(ADDON_LOG, 0))
                now = time.time()
                if hits:
                    now_state = hits[-1][0]
                    if cur_state == "cutscene":
                        cutscene_secs += now - last
                    if now_state != cur_state:
                        cur_state, since = now_state, now
                    if (cutscene_secs >= args.min_cutscene and cur_state == "gameplay"
                            and now - since >= args.settle):
                        end_reason = "handed over to gameplay after %.0fs of cutscene" % cutscene_secs
                        break
                last = now
                time.sleep(1.0)
        else:
            time.sleep(10)

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

        # what the engine thought it was drawing - the video/scene discriminator
        state, draws, hud = "", -1, -1
        if os.path.exists(ADDON_LOG):
            shutil.copyfile(ADDON_LOG, os.path.join(SHOTS, sid + ".addon.log"))
            hits = STATE_RE.findall(open(ADDON_LOG, errors="replace").read())
            if hits:
                state, draws, hud = hits[-1][0], int(hits[-1][1]), int(hits[-1][2])

        after, tail = dumps_count(), tail_since(LAUNCH_LOG, lmark)
        if after > before:
            result, detail = "crash", "new minidump"
        elif scene_at:
            result, detail = "boot", "3D frame reached"
        else:
            result, detail = "no-scene", "no 3D frame in %ds" % int(time.time() - t0)

        # 4. keep it, trimmed so it opens on the loading screen
        offset, scene_in, final = -1.0, -1.0, ""
        if out_path and os.path.exists(out_path):
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
                shutil.move(out_path, dst)
            final = sid + ".mkv"

        tally[result] = tally.get(result, 0) + 1
        with open(args.out, "a", newline="") as fh:
            csv.DictWriter(fh, FIELDS).writerow(dict(
                id=sid, result=result, seconds=int(time.time() - t0), scene_offset=offset,
                scene_at=scene_in, video=final, state=state, draws=draws, hud=hud,
                detail=(detail if result != "boot" else end_reason)))
        print("[%4d/%d] %-18s %-9s %3ds scene@%-6s %-8s draws:%-5s  boot %d / crash %d / no-scene %d"
              % (n, len(todo), sid, result, int(time.time() - t0), offset, state, draws,
                 tally["boot"], tally["crash"], tally["no-scene"]), flush=True)

    print("\ndone: boot %d, crash %d, no-scene %d -> %s"
          % (tally["boot"], tally["crash"], tally["no-scene"], args.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
