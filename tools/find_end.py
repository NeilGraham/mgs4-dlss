"""Finds where a recorded MGS4 cutscene actually ends, from the video itself.

The decision comes from the picture (see screen_signals.py): the solid orange "OLD SNAKE" bar means gameplay has
started, and a black frame carrying the red MGS4 emblem in the bottom-left means the loading / continue screen. Both
were calibrated against frames whose timing was checked by hand.

  python find_end.py <file> [...]        - report the end point of each file
  python find_end.py --all               - report for every recording in the output folder
  python find_end.py --trim <file> [...] - cut the file at the detected end (stream copy, keeps the original as .orig)
"""
import json, os, subprocess, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from screen_signals import classify        # noqa: E402
import paths                               # noqa: E402

OUT_DIR = paths.OUT_DIR
PROBE_W, PROBE_H = 320, 180
HOLD = 3               # seconds the same verdict must hold
HEAD_SKIP = 15         # the recording starts at the boot prompts; ignore those


def sample(path, fps=1):
    """Classify one frame a second; returns [(t, verdict)]."""
    cmd = [paths.FFMPEG, "-v", "error", "-i", path, "-vf", f"fps={fps},scale={PROBE_W}:{PROBE_H}",
           "-pix_fmt", "rgb24", "-f", "rawvideo", "-"]
    frame_bytes = PROBE_W * PROBE_H * 3
    out = []
    with subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL) as proc:
        idx = 0
        while True:
            buf = proc.stdout.read(frame_bytes)
            if len(buf) < frame_bytes:
                break
            verdict, _ = classify(buf, PROBE_W, PROBE_H)
            out.append((idx / fps, verdict))
            idx += 1
    return out


def find_end(path, verbose=False):
    s = sample(path)
    if not s:
        return None, "no samples", []
    run, kind = 0, None
    for t, verdict in s:
        if t < HEAD_SKIP:
            continue
        if verdict in ("gameplay", "end-screen"):
            run = run + 1 if verdict == kind else 1
            kind = verdict
            if run >= HOLD:
                return t - HOLD + 1, kind, s
        else:
            run, kind = 0, None
    return None, "none", s


def hms(t):
    return "%d:%02d" % (int(t) // 60, int(t) % 60)


def main():
    args = sys.argv[1:]
    trim = "--trim" in args
    args = [a for a in args if a != "--trim"]
    if "--all" in args:
        res = json.load(open(os.path.join(OUT_DIR, "recordings.json")))
        files = [os.path.join(OUT_DIR, r["file"]) for r in sorted(res, key=lambda r: r["index"])
                 if r.get("status") == "ok" and r.get("file")]
    else:
        files = [a if os.path.isabs(a) else os.path.join(OUT_DIR, a) for a in args]
    for f in files:
        if not os.path.exists(f):
            print("missing:", f); continue
        end, why, _ = find_end(f)
        dur = float(subprocess.run([paths.FFPROBE, "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", f],
                                   capture_output=True, text=True).stdout.strip() or 0)
        print(f"{os.path.basename(f):48s} length {hms(dur):>6s}  end {hms(end) if end else '   -':>6s} ({why})")
        if trim and end and dur - end > 3:
            tmp = f + ".trim.mkv"
            subprocess.run([paths.FFMPEG, "-v", "error", "-y", "-i", f, "-t", str(end), "-c", "copy", tmp],
                           capture_output=True)
            if os.path.exists(tmp) and os.path.getsize(tmp) > 1_000_000:
                os.replace(f, f + ".orig")
                os.replace(tmp, f)
                print(f"    trimmed to {hms(end)} (original kept as .orig)")


if __name__ == "__main__":
    main()
