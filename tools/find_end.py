"""Finds where a recorded MGS4 cutscene actually ends, from the video itself.

Two signals, sampled once a second:
  * HUD chroma - MGS4's HUD is orange, so the mean V (red-difference) of the bottom strip, where the ration and
    weapon boxes sit, jumps from ~128 (neutral: a cutscene) to ~137+ (gameplay).
  * frozen picture - a "PRESS ANY BUTTON" / results screen barely changes, so consecutive frames are near-identical.

  python find_end.py <file> [...]        - report the end point of each file
  python find_end.py --all               - report for every recording in D:\\mgs4-dlss5
  python find_end.py --trim <file> [...] - cut the file at the detected end (stream copy, keeps the original as .orig)
"""
import json, os, re, subprocess, sys

OUT_DIR = r"D:\mgs4-dlss5"
# The HUD is orange, so the mean V (red-difference) of the two bottom corners - where the ration box and the weapon
# box sit - rises well above neutral (128) in gameplay. Scene content can tint one corner for a moment, so both
# corners have to be lit at once, and stay lit.
LEFT_CROP = "iw*0.22:ih*0.09:iw*0.03:ih*0.88"
RIGHT_CROP = "iw*0.26:ih*0.09:iw*0.70:ih*0.88"
LEFT_MIN, RIGHT_MIN = 133.0, 136.0
HUD_HOLD = 4           # seconds both corners must stay lit
STILL_DIFF = 1.2       # mean absolute difference between consecutive samples: below this the picture is still
STILL_MIN = 3          # seconds of stillness before it counts as a prompt/results screen
STILL_MAX = 25         # if the file just ends inside a still stretch, cut this far into it
HEAD_SKIP = 15         # the recording starts at the boot prompts; ignore those


def _series(path, vf, key):
    cmd = ["ffmpeg", "-v", "error", "-i", path, "-vf", vf + f",metadata=print:key={key}:file=-", "-f", "null", "-"]
    out = subprocess.run(cmd, capture_output=True, text=True).stdout
    vals, t = [], None
    for line in out.splitlines():
        m = re.search(r"pts_time:([\d.]+)", line)
        if m:
            t = float(m.group(1))
        m = re.search(re.escape(key) + r"=([\d.]+)", line)
        if m and t is not None:
            vals.append((t, float(m.group(1))))
    return vals


def sample(path):
    left = _series(path, f"fps=1,crop={LEFT_CROP},signalstats", "lavfi.signalstats.VAVG")
    right = dict(_series(path, f"fps=1,crop={RIGHT_CROP},signalstats", "lavfi.signalstats.VAVG"))
    diff = dict(_series(path, "fps=1,scale=320:180,tblend=all_mode=difference,signalstats", "lavfi.signalstats.YAVG"))
    return [(t, v, right.get(t, 0.0), diff.get(t, 99.0)) for t, v in left]


def find_end(path, verbose=False):
    s = sample(path)
    if not s:
        return None, "no samples", []
    hud, still_from = 0, None
    for t, lv, rv, d in s:
        if t < HEAD_SKIP:
            continue
        hud = hud + 1 if (lv > LEFT_MIN and rv > RIGHT_MIN) else 0
        if hud >= HUD_HOLD:                       # the gameplay HUD is up: the scene ended when it appeared
            return t - HUD_HOLD + 1, "hud", s
        if d < STILL_DIFF:
            still_from = t if still_from is None else still_from
        else:
            # a prompt ("PRESS ANY BUTTON") or results screen holds still and then the game moves on: the scene is
            # over when the picture starts moving again, which is also where the game itself left the cutscene
            if still_from is not None and t - still_from >= STILL_MIN:
                return t, "still-screen", s
            still_from = None
    if still_from is not None and s[-1][0] - still_from >= STILL_MIN:
        return min(still_from + STILL_MAX, s[-1][0]), "still-to-end", s
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
        dur = float(subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", f],
                                   capture_output=True, text=True).stdout.strip() or 0)
        print(f"{os.path.basename(f):48s} length {hms(dur):>6s}  end {hms(end) if end else '   -':>6s} ({why})")
        if trim and end and dur - end > 3:
            tmp = f + ".trim.mkv"
            subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", f, "-t", str(end), "-c", "copy", tmp],
                           capture_output=True)
            if os.path.exists(tmp) and os.path.getsize(tmp) > 1_000_000:
                os.replace(f, f + ".orig")
                os.replace(tmp, f)
                print(f"    trimmed to {hms(end)} (original kept as .orig)")


if __name__ == "__main__":
    main()
