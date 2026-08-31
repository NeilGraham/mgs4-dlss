"""Trims the gold raw clips to the scene itself and remuxes them to MP4 (AV1, stream copy).

Start: the scene's first frame is the last black frame before the add-on reported the cutscene (cutscenes fade in
from black; the boot prompts before it are text on black too, so the cut goes at the last black-to-picture edge in the
3 s before the report). End, by how the scene ended:
  gameplay      - the first frame with the HUD (orange OLD SNAKE bar) around the reported transition
  game-exited   - the last frame before the picture jumps (desktop after the window closed), minus trailing black
  static        - the first black / static frame after the last cutscene frame
Every cut is checked against the picture and written to results.json with the frame times; --report only prints.

  python finalize_gold.py [--report] [--stages a,b]
"""
import json, os, subprocess, sys
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from screen_signals import classify        # noqa: E402
import paths                               # noqa: E402

GOLD = paths.GOLD
FF = paths.FFMPEG
FP = paths.FFPROBE
W, H = 320, 180


def frames(path, t0, t1, fps=60):
    """Grayscale + rgb frames of [t0, t1) at `fps`; returns (times, gray[n,H,W], rgb bytes list)."""
    t0 = max(0.0, t0)
    cmd = [FF, "-v", "error", "-ss", "%.3f" % t0, "-t", "%.3f" % max(0.1, t1 - t0), "-i", path, "-vf", "fps=%d,scale=%d:%d" % (fps, W, H), "-pix_fmt", "rgb24", "-f", "rawvideo", "-"]
    raw = subprocess.run(cmd, capture_output=True).stdout
    n = len(raw) // (W * H * 3)
    rgb = np.frombuffer(raw[:n * W * H * 3], dtype=np.uint8).reshape(n, H, W, 3)
    gray = (0.299 * rgb[..., 0] + 0.587 * rgb[..., 1] + 0.114 * rgb[..., 2]).astype(np.float32)
    return [t0 + k / fps for k in range(n)], gray, rgb


def duration(path):
    out = subprocess.run([FP, "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", path], capture_output=True, text=True).stdout.strip()
    try: return float(out)
    except ValueError: pass
    # OBS's MKV carries no duration: stream-copy through the null muxer and read the last progress time (no decode)
    err = subprocess.run([FF, "-v", "info", "-i", path, "-map", "0:v", "-c", "copy", "-f", "null", "-"], capture_output=True, text=True).stderr
    import re
    m = re.findall(r"time=(\d+):(\d\d):(\d\d\.\d+)", err)
    if not m: return 0.0
    h, mi, se = m[-1]; return int(h) * 3600 + int(mi) * 60 + float(se)


def find_start(path, t_report):
    """Last black->picture edge in the 3 s before the reported cutscene (fallback: the report time)."""
    ts, g, _ = frames(path, t_report - 3.0, t_report + 1.0)
    if len(ts) < 2: return t_report, "no-frames"
    mean = g.mean(axis=(1, 2))
    black = mean < 6.0
    # walk backwards from the report: the first black frame we meet going back is where the fade-in began
    k = min(range(len(ts)), key=lambda i: abs(ts[i] - t_report))
    for i in range(k, -1, -1):
        if black[i]: return ts[i], "black-edge"
    return ts[0], "no-black-before-report"


def find_end(path, t_hint, reason):
    dur = duration(path)
    if reason == "gameplay":
        ts, g, rgb = frames(path, t_hint - 3.0, min(dur, t_hint + 3.0), fps=30)
        for i, t in enumerate(ts):
            v, _ = classify(rgb[i].tobytes(), W, H)
            if v == "gameplay": return max(0.0, t - 1 / 30.0), "hud-appears"
        return t_hint, "hud-not-seen"
    if reason == "game-exited":
        ts, g, _ = frames(path, max(0.0, dur - 6.0), dur, fps=30)
        if len(ts) < 3: return dur, "short"
        d = np.abs(np.diff(g, axis=0)).mean(axis=(1, 2)); mean = g.mean(axis=(1, 2))
        jump = next((i for i in range(len(d)) if d[i] > 25.0), None)          # window closed -> desktop
        end_i = jump if jump is not None else len(ts) - 1
        while end_i > 0 and mean[end_i - 1] < 6.0: end_i -= 1                # drop trailing black, keep 0.5 s of it
        return min(dur, ts[end_i] + 0.5), "before-desktop" if jump is not None else "tail"
    # static / cap / other: the first black-or-static frame after the last cutscene frame
    ts, g, _ = frames(path, t_hint - 2.0, min(dur, t_hint + 3.0), fps=30)
    if len(ts) < 3: return t_hint, "short"
    mean = g.mean(axis=(1, 2)); d = np.abs(np.diff(g, axis=0)).mean(axis=(1, 2))
    for i in range(1, len(ts)):
        if mean[i] < 6.0 or (i + 15 < len(d) and d[i:i + 15].max() < 0.3): return ts[i] + 0.3, "black-or-static"
    return t_hint, "hint"


def safe_name(s):
    return "".join(c if c.isalnum() or c in " -_.'" else "_" for c in s)


def main():
    args = sys.argv[1:]
    report = "--report" in args
    wanted = set()
    if "--stages" in args: wanted = set(args[args.index("--stages") + 1].split(","))
    res_path = os.path.join(GOLD, "results.json")
    results = json.load(open(res_path))
    for r in sorted(results, key=lambda r: r.get("index", 0)):
        if r.get("status") != "ok" or not r.get("raw") or (wanted and r["stage"] not in wanted): continue
        raw = r["raw"]
        if not os.path.exists(raw): print("missing", raw); continue
        off_start = r["cut_start"] - r["t_rec"]; off_end = r["scene_end"] - r["t_rec"]
        s, why_s = find_start(raw, off_start)
        e, why_e = find_end(raw, off_end, r["end_reason"])
        r["trim"] = {"start": round(s, 3), "end": round(e, 3), "start_how": why_s, "end_how": why_e, "hint_start": round(off_start, 2), "hint_end": round(off_end, 2)}
        print(f"[{r['index']:02d}] {r['stage']:12s} {r['title']:40s} start {s:7.2f}s ({why_s}, hint {off_start:.1f})  end {e:7.2f}s ({why_e}, hint {off_end:.1f})  length {(e - s) / 60:5.1f} min  [{r['end_reason']}]")
        if report: continue
        out = os.path.join(GOLD, f"{r['index']:02d} - {safe_name(r['title'])}.mp4")
        cmd = [FF, "-v", "error", "-y", "-ss", "%.3f" % s, "-to", "%.3f" % e, "-i", raw, "-c", "copy", "-movflags", "+faststart", out]
        subprocess.run(cmd, capture_output=True)
        if os.path.exists(out) and os.path.getsize(out) > 1_000_000:
            r["final"] = out; r["final_seconds"] = round(duration(out), 1)
            print(f"      -> {os.path.basename(out)} ({r['final_seconds']:.0f}s, {os.path.getsize(out) / 1048576:.0f} MB)")
        else: print("      trim failed")
    json.dump(results, open(res_path, "w"), indent=1)


if __name__ == "__main__":
    main()
