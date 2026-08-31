"""Contact sheets of the first and last seconds of each finalized gold clip (head: 8 frames over 2 s from the start;
tail: 8 frames over 2 s up to the end), written next to the clip as <name>.check.png, for a look at the cut points.
  python inspect_gold.py [--stages a,b] [--span 2.0]"""
import json, os, subprocess, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import paths                                                     # noqa: E402

GOLD = paths.GOLD
FF = paths.FFMPEG
FP = paths.FFPROBE
args = sys.argv[1:]
wanted = set(args[args.index("--stages") + 1].split(",")) if "--stages" in args else set()
span = float(args[args.index("--span") + 1]) if "--span" in args else 2.0
res = json.load(open(os.path.join(GOLD, "results.json")))
for r in sorted(res, key=lambda r: r.get("index", 0)):
    f = r.get("final")
    if not f or not os.path.exists(f) or (wanted and r["stage"] not in wanted): continue
    dur = float(subprocess.run([FP, "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", f], capture_output=True, text=True).stdout.strip() or 0)
    out = os.path.splitext(f)[0] + ".check.png"
    step = span / 8.0
    vf = "select='lt(t,%.3f)*eq(mod(floor(t/%.4f),1),0)'" % (span, step)
    # head and tail via two inputs, each sampled at 8/span fps, tiled 4x2 each, stacked vertically
    cmd = [FF, "-v", "error", "-y", "-t", "%.3f" % span, "-i", f, "-ss", "%.3f" % max(0.0, dur - span), "-i", f,
           "-filter_complex", "[0:v]fps=%.4f,scale=480:270,tile=4x2[a];[1:v]fps=%.4f,scale=480:270,tile=4x2[b];[a][b]vstack" % (8.0 / span, 8.0 / span),
           "-frames:v", "1", out]
    subprocess.run(cmd, capture_output=True)
    print(f"[{r['index']:02d}] {r['stage']:12s} {dur:7.1f}s -> {os.path.basename(out)}")
