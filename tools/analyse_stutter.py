"""Measures stutter in a recording and lines it up with what the add-on logged at the same moment.

OBS records at a fixed 60 fps, so whenever the game fails to present a new frame the recording simply repeats the
previous one. Counting repeated frames therefore gives the game's real frame rate over time:

    effective fps in a second = 60 - (repeated frames in that second)

  python analyse_stutter.py <file.mkv> [--log <addon log>] [--csv out.csv]

Prints a per-second timeline of the worst stretches, the overall effective frame rate, and any add-on events
(history resets, feature re-creations, resolution changes) that fall inside the bad seconds.
"""
import argparse, os, re, subprocess, sys, datetime, collections

FRAME_RE = re.compile(r"pts_time:([\d.]+)")
VAL_RE = re.compile(r"lavfi\.signalstats\.YAVG=([\d.]+)")
DUP_LIMIT = 0.02        # mean absolute difference below this: the frame is a repeat of the previous one


def frame_diffs(path):
    """[(t, diff)] for every frame, using a small greyscale difference so it is quick on 4K."""
    cmd = ["ffmpeg", "-v", "error", "-i", path, "-vf",
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
    """Wall-clock time the recording started, so add-on log lines can be matched to it."""
    try:
        out = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format_tags=creation_time",
                              "-of", "default=nw=1:nk=1", path], capture_output=True, text=True).stdout.strip()
        if out:
            return datetime.datetime.fromisoformat(out.replace("Z", "+00:00")).astimezone()
    except Exception:
        pass
    return datetime.datetime.fromtimestamp(os.path.getmtime(path))


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
    ap.add_argument("--log", default=r"C:\Program Files (x86)\Steam\steamapps\common\METAL GEAR SOLID 4\MGS4\logs\mgs4_dlss.log")
    ap.add_argument("--csv", default="")
    ap.add_argument("--worst", type=int, default=15)
    args = ap.parse_args()

    path = args.file if os.path.isabs(args.file) else os.path.join(r"D:\mgs4-dlss5", args.file)
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
