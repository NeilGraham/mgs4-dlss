"""Writes one frame per scene into tools/thumbs/<id>.jpg - what the launcher shows as the banner on each row and
beside the description when a scene is picked.

  python tools/make_thumbs.py                 build from <MGS4_OUT>\\sweep
  python tools/make_thumbs.py --width 640 --quality 60

Individual files rather than one zip, deliberately: a re-pick of one scene then changes one 47 KB file in git,
where a zip changed all 10 MB of itself every time. launcher\\build.ps1 zips the folder into the exe at build time,
so the launcher still reads a single resource (Thumbs.cs) and the release workflow needs nothing but the tree.

The frame is the one picked by eye from the scene's contact sheets and cut from its recording
(tools/pick_thumbs.py -> <MGS4_OUT>\\sweep\\thumbs\\<id>.jpg); until a scene has a pick, the sweep's own ~18 s still
stands in (the ~9 s one, then the location frame, when that is missing).

Sized for the detail pane at its physical size: the pane is 480 logical px wide, 960 on a 4K screen at 200%,
and the launcher decodes at that width, so 960x540 shows without upscaling. At q74 a scene costs ~47 KB.
"""
import os, sys, csv, io
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import paths  # noqa: E402

SHOTS = os.path.join(paths.OUT_DIR, "sweep")
PROBE = os.path.join(HERE, "stage_probe.csv")
OUT = os.path.join(HERE, "thumbs")



def main():
    argv = sys.argv[1:]
    width, quality = 960, 74
    while argv:
        if argv[0] == "--width":
            width, argv = int(argv[1]), argv[2:]
        elif argv[0] == "--quality":
            quality, argv = int(argv[1]), argv[2:]
        else:
            argv = argv[1:]

    ids = []
    if os.path.exists(PROBE):
        with open(PROBE, newline="") as fh:
            ids = [r["id"] for r in csv.DictReader(fh) if r.get("result") in ("boot", "no-scene")]
    if not ids:
        print("no booting ids in %s - run the sweep first" % PROBE)
        return 1

    total = 0
    kept = 0
    os.makedirs(OUT, exist_ok=True)
    wanted = set()
    for sid in ids:
        # the frame picked by eye (tools/pick_thumbs.py) first; the sweep's own stills only until there is one
        src = os.path.join(SHOTS, "thumbs", sid + ".jpg")
        if not os.path.exists(src):
            src = None
            for suf in ("_b", "_a", "_loc"):
                p = os.path.join(SHOTS, "%s%s.jpg" % (sid, suf))
                if os.path.exists(p):
                    src = p
                    break
        if not src:
            continue
        full = Image.open(src).convert("RGB")
        im = full.resize((width, max(1, round(width * full.height / full.width))), Image.LANCZOS)
        dst = os.path.join(OUT, sid + ".jpg")
        im.save(dst, "JPEG", quality=quality, optimize=True)
        wanted.add(sid + ".jpg")
        total += os.path.getsize(dst)
        kept += 1
    # a scene that stopped being recorded (or became an alias) loses its picture
    for n in os.listdir(OUT):
        if n.endswith(".jpg") and n not in wanted:
            os.remove(os.path.join(OUT, n))
    print("%d thumbnails -> %s (%.0f KB, %.1f KB each at %dpx q%d)" %
          (kept, OUT, total / 1024.0, total / 1024.0 / max(kept, 1), width, quality))
    return 0


if __name__ == "__main__":
    sys.exit(main())
