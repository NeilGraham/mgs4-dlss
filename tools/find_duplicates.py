"""Finds stage ids that are the same scene under another name, and writes them into scene_info.json as "sameAs"
so the launcher folds them into one row with the other ids offered beside it.

  python tools/find_duplicates.py                report what it would merge, write nothing
  python tools/find_duplicates.py --scores       also print the best score for every pair considered
  python tools/find_duplicates.py --apply        write the sameAs entries

Compared as **video, not stills**. Stills would have to assume the two runs are in step, and they are not: scene
detection lands within about a second of the first 3D frame but not the same second every time, so "20 s in" is a
different moment of a moving cutscene from one run to the next. Two recordings of one scene would then disagree
and the duplicate would be missed.

So each scene becomes a short fingerprint - 2 frames a second of 64x36 greyscale over the scene window - and a
pair is scored at its *best alignment*, sliding one against the other by up to ALIGN seconds. Identical footage
lines up at some offset and matches closely there; different scenes match nowhere.

Two further guards, because two entrances to one map look alike while nobody is holding a controller:
  * the match has to hold across the whole overlap, not at one instant
  * the ids have to share a stem (s02a25l for s02a25l_D1, _D2), so unrelated corridors cannot merge
The primary of a group is the shortest id, which is the bare stage entry where there is one.
"""
import os, re, sys, csv, json, io, subprocess, itertools
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import paths  # noqa: E402

SHOTS = os.path.join(paths.OUT_DIR, "sweep")   # kept for the frame fallback
VIDEO = os.path.join(SHOTS, "video")
PROBE = os.path.join(HERE, "stage_probe.csv")
INFO = os.path.join(HERE, "scene_info.json")

FPS = 2.0          # fingerprint sample rate
W, H = 64, 36      # fingerprint frame size
WINDOW = 30.0      # seconds of scene sampled
ALIGN = 6.0        # seconds of slide allowed when lining two fingerprints up
MATCH = 6.0        # mean abs luma difference (0-255) at best alignment, below which it is the same footage
MIN_FRAMES = 8     # a score over fewer frames than this is noise, whatever the clips are
OVERLAP_FRAC = 0.6 # ...and it has to cover this much of the shorter clip

SEARCH = 3.0       # seconds either side of the recorder's own scene_at that are searched for the real start
PROBE_FPS = 60     # the head of the file is read at the recording's rate, so the answer is frame-accurate
DARK = 1.0         # how far above the darkest frame in that head still counts as "not the scene yet"


def stem(sid):
    """s02a25l_D1 -> s02a25l, s02a20l_11 -> s02a20l. The suffix is what marks a re-filing of one stage."""
    return re.sub(r"_(D\d*|\d+)$", "", sid)


STARTS = {}        # id -> where the scene was found to start, for the report


def grey(path, start, window, fps):
    """`window` seconds from `start`, as fps x 36 x 64 greyscale."""
    cmd = [paths.FFMPEG, "-v", "error", "-ss", str(max(0.0, start)), "-i", path, "-t", str(window),
           "-vf", "fps=%g,scale=%d:%d" % (fps, W, H), "-pix_fmt", "gray", "-f", "rawvideo", "-"]
    try:
        raw = subprocess.run(cmd, capture_output=True, timeout=180).stdout
    except Exception:
        return None
    n = len(raw) // (W * H)
    if not n:
        return None
    return np.frombuffer(raw[: n * W * H], dtype=np.uint8).reshape(n, H, W).astype(np.float32)


def scene_start(path, fallback):
    """Seconds into the file where the loading screen is over and the scene itself starts.

    Taken from the picture, not from the sweep's `scene_at`. The recorder trims losslessly, so it cuts at the
    nearest keyframe and the nominal lead drifts by a frame or two; and, more to the point, the loading screen is
    not the same picture from one run to the next - the same stage came up behind a plain title card once and
    behind the act's own loading art the next time. Any of that left inside the window is two recordings of one
    scene disagreeing about something that is not the scene.

    The rule is simply that **the darkest thing in the head of the file is not the scene**: it is either the
    black gap between the loading screen going away and the scene fading up, or - where there is no gap - the
    loading screen itself. The scene starts at the frame after the last of it. `scene_at` is used only to say
    where to look, since it is right to within a keyframe.
    """
    window = max(0.5, fallback) + SEARCH
    a = grey(path, 0.0, window, PROBE_FPS)
    if a is None or len(a) < PROBE_FPS:
        return fallback
    lum = a.mean(axis=(1, 2))
    dark = np.nonzero(lum <= lum.min() + DARK)[0]
    at = (int(dark[-1]) + 1) / float(PROBE_FPS)
    # Everything in the window was dark, so there is no rise to find and nothing has been learnt - a scene that
    # opens on black for longer than we looked. Leave it to the recorder's figure rather than guess.
    if at > window - 0.5:
        return fallback
    return round(at, 3)


def fingerprint(sid, offset):
    """2 fps of 64x36 grey from the scene's own start. None when there is no usable recording."""
    v = os.path.join(VIDEO, "%s.mkv" % sid)
    if not os.path.exists(v):
        return None
    STARTS[sid] = scene_start(v, offset)
    a = grey(v, STARTS[sid], WINDOW, FPS)
    return a if a is not None and len(a) >= MIN_FRAMES else None


def best_score(a, b):
    """Lowest mean abs difference over every alignment within ALIGN seconds, and the shift that achieved it.

    How much overlap a score needs is measured against the clips in hand, not fixed. A flat 20-frame floor is
    the whole of a 10-second gameplay recording, so every non-zero shift fell under it and the slide - the part
    that makes this work on runs that are not in step - tested exactly one alignment."""
    # Cut both to the shorter of the two first: whatever one recording has that the other does not cannot say
    # anything about whether they are the same scene, and letting it in only moves the alignment around.
    shortest = min(len(a), len(b))
    a, b = a[:shortest], b[:shortest]
    floor = max(MIN_FRAMES, int(OVERLAP_FRAC * shortest))
    span = min(int(ALIGN * FPS), shortest - floor)
    best, at = 1e9, 0
    for shift in range(-span, span + 1):
        if shift >= 0:
            x, y = a[shift:], b[: len(a) - shift]
        else:
            x, y = a[: len(a) + shift], b[-shift:]
        n = min(len(x), len(y))
        if n < floor:
            continue
        d = float(np.abs(x[:n] - y[:n]).mean())
        if d < best:
            best, at = d, shift
    return best, at / FPS


def main():
    argv = sys.argv[1:]
    apply, scores = "--apply" in argv, "--scores" in argv
    if not os.path.exists(PROBE):
        print("no %s - run the sweep first" % PROBE)
        return 1
    rows = [r for r in csv.DictReader(open(PROBE, newline="")) if r.get("result") == "boot"]
    # scene_at, not scene_offset: the kept video is trimmed to open on the loading screen, so the scene begins
    # roughly this far into the file (scene_offset is measured against the original untrimmed recording). Only
    # roughly - it is a hint telling scene_start() where to look, not the figure the comparison runs from.
    offsets = {r["id"]: float(r.get("scene_at") or r.get("scene_offset") or 0) for r in rows}

    groups = {}
    for r in rows:
        groups.setdefault(stem(r["id"]), []).append(r["id"])
    candidates = {k: sorted(v) for k, v in groups.items() if len(v) > 1}
    print("%d booting ids, %d stems with more than one id" % (len(rows), len(candidates)))

    fps_cache, parent = {}, {}
    def fp(sid):
        if sid not in fps_cache:
            fps_cache[sid] = fingerprint(sid, offsets.get(sid, 0))
        return fps_cache[sid]
    for r in rows:
        parent[r["id"]] = r["id"]
    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    pairs = 0
    for st, members in sorted(candidates.items()):
        for a, b in itertools.combinations(members, 2):
            fa, fb = fp(a), fp(b)
            if fa is None or fb is None:
                continue
            d, shift = best_score(fa, fb)
            pairs += 1
            if scores:
                print("   %-16s %-16s  %6.2f  (shift %+.1fs, %.1fs each from %.2fs / %.2fs)%s" %
                      (a, b, d, shift, min(len(fa), len(fb)) / FPS, STARTS.get(a, -1), STARTS.get(b, -1),
                       "  <- same" if d < MATCH else ""))
            if d < MATCH:
                ra, rb = find(a), find(b)
                if ra != rb:
                    parent[ra] = rb

    merged = {}
    for r in rows:
        merged.setdefault(find(r["id"]), []).append(r["id"])
    dups = {k: sorted(v, key=lambda s: (len(s), s)) for k, v in merged.items() if len(v) > 1}
    total = sum(len(v) - 1 for v in dups.values())
    print("\n%d pairs compared, %d groups, folding %d ids into another row\n" % (pairs, len(dups), total))
    for _, members in sorted(dups.items(), key=lambda kv: kv[1][0]):
        print("   %-16s <- %s" % (members[0], ", ".join(members[1:])))

    if not apply:
        print("\n(report only - pass --apply to write scene_info.json)")
        return 0

    info = json.load(open(INFO, encoding="utf-8"))
    scenes = info.setdefault("scenes", {})
    wrote = 0
    for _, members in dups.items():
        for other in members[1:]:
            # sameAs replaces the entry: the launcher reads it before anything else and folds the row away, so a
            # stale name or kind left on it would only be misleading.
            scenes[other] = {"sameAs": members[0]}
            wrote += 1
    io.open(INFO, "w", encoding="utf-8", newline="\n").write(
        json.dumps(info, indent=2, ensure_ascii=False) + "\n")
    print("\nwrote %d sameAs entries to %s" % (wrote, INFO))
    return 0


if __name__ == "__main__":
    sys.exit(main())
