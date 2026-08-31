"""Measures stutter in a recording and lines it up with what the add-on logged at the same moment.

OBS records at a fixed 60 fps, so whenever the game fails to present a new frame the recording simply repeats the
previous one. Counting repeated frames therefore gives the game's real frame rate over time:

    effective fps in a second = 60 - (repeated frames in that second)

  python analyse_stutter.py <file.mkv> [--log <addon log>] [--csv out.csv]

Prints a per-second timeline of the worst stretches, the overall effective frame rate, and any add-on events
(history resets, feature re-creations, resolution changes) that fall inside the bad seconds.
"""
import argparse, os, re, subprocess, sys, datetime, collections

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import paths                                                     # noqa: E402

FRAME_RE = re.compile(r"pts_time:([\d.]+)")
VAL_RE = re.compile(r"lavfi\.signalstats\.YAVG=([\d.]+)")
# A repeated frame is not bit-identical in the recording - AV1 re-encodes it - so it shows up as a very small but
# non-zero difference. Measured distribution over a scene: minimum 0.0004, 5th percentile 0.019, median 1.47, so
# anything under ~0.005 is a re-presented frame while genuinely near-static shots sit above it.
DUP_LIMIT = 0.005


def frame_diffs(path):
    """[(t, diff)] for every frame, using a small greyscale difference so it is quick on 4K."""
    cmd = [paths.FFMPEG, "-v", "error", "-i", path, "-vf",
           "scale=480:270,format=gray,tblend=all_mode=difference,signalstats,"
           "metadata=print:key=lavfi.signalstats.YAVG:file=-", "-f", "null", "-"]
    out = subprocess.run(cmd, capture_output=True, text=True).stdout
    res, t = [], None
    for line in out.splitlines():
        m = FRAME_RE.search(line)
        if m:
            t = float(m.group(1))
        m = VAL_RE.search(line)
        if m and t is not None:
            res.append((t, float(m.group(1))))
    return res


def per_second(diffs):
    buckets = collections.defaultdict(lambda: [0, 0])   # second -> [frames, repeats]
    for t, d in diffs:
        b = buckets[int(t)]
        b[0] += 1
        if d < DUP_LIMIT:
            b[1] += 1
    return {s: (n, r) for s, (n, r) in sorted(buckets.items())}


def file_start_time(path):
    """Wall-clock time the recording started. OBS writes no creation time into the file, so the recorder stores it
    in recordings.json; failing that, fall back to the file's end time minus its duration."""
    import json
    rec = os.path.join(os.path.dirname(path), "recordings.json")
    if os.path.exists(rec):
        try:
            for r in json.load(open(rec)):
                if r.get("file") == os.path.basename(path) and r.get("started_at"):
                    h, m, s2 = (int(x) for x in r["started_at"].split(":"))
                    d = datetime.datetime.fromtimestamp(os.path.getmtime(path))
                    return d.replace(hour=h, minute=m, second=s2, microsecond=0)
        except Exception:
            pass
    dur = subprocess.run([paths.FFPROBE, "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", path],
                         capture_output=True, text=True).stdout.strip()
    end = datetime.datetime.fromtimestamp(os.path.getmtime(path))
    try:
        return end - datetime.timedelta(seconds=float(dur))
    except Exception:
        return end


def addon_events(log_path, start, seconds):
    """{second_offset: [event, ...]} for interesting add-on log lines inside the recording's span."""
    events = collections.defaultdict(list)
    if not log_path or not os.path.exists(log_path):
        return events
    pat = re.compile(r"^\[(\d\d):(\d\d):(\d\d)\.\d+\]\s+(.*)$")
    keep = re.compile(r"RESET |CreateFeature DLSS \(|re-creating|DRS:|scene viewport changed|SCENE-STATE ")
    for line in open(log_path, encoding="utf-8", errors="replace"):
        m = pat.match(line)
        if not m or not keep.search(line):
            continue
        h, mi, s = int(m.group(1)), int(m.group(2)), int(m.group(3))
        when = start.replace(hour=h, minute=mi, second=s, microsecond=0)
        off = int((when - start.replace(microsecond=0)).total_seconds())
        if 0 <= off <= seconds:
            events[off].append(m.group(4).strip())
    return events


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--log", default=paths.ADDON_LOG)
    ap.add_argument("--csv", default="")
    ap.add_argument("--worst", type=int, default=15)
    args = ap.parse_args()

    path = args.file if os.path.isabs(args.file) else os.path.join(paths.OUT_DIR, args.file)
    diffs = frame_diffs(path)
    if not diffs:
        print("no frames read"); return
    secs = per_second(diffs)
    total = len(diffs)
    repeats = sum(r for _, r in secs.values())
    dur = max(secs) + 1
    print(f"{os.path.basename(path)}: {dur} s, {total} frames, {repeats} repeated "
          f"({100.0 * repeats / total:.1f} %), effective {(total - repeats) / dur:.1f} fps of 60")

    start = file_start_time(path)
    events = addon_events(args.log, start, dur)
    worst = sorted(secs.items(), key=lambda kv: -kv[1][1])[:args.worst]
    print(f"\nworst seconds (repeats of {60} frames):")
    for s, (n, r) in sorted(worst):
        note = "; ".join(events.get(s, []))[:110]
        print(f"  {s // 60:d}:{s % 60:02d}  {r:2d} repeated  ({n - r:2d} new frames){'  <- ' + note if note else ''}")

    hits = sum(1 for s, (n, r) in secs.items() if r >= 6 and events.get(s))
    stuttery = sum(1 for _, (n, r) in secs.items() if r >= 6)
    if stuttery:
        print(f"\n{stuttery} seconds lost 6+ frames; {hits} of them coincide with an add-on event "
              f"({100.0 * hits / stuttery:.0f} %)")
    if args.csv:
        with open(args.csv, "w", encoding="utf-8") as f:
            f.write("second,frames,repeats,effective_fps,events\n")
            for s, (n, r) in secs.items():
                f.write(f"{s},{n},{r},{n - r},\"{'; '.join(events.get(s, []))}\"\n")
        print("wrote", args.csv)


if __name__ == "__main__":
    main()
