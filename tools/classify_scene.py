"""Decides, from one frame, whether a stage is a Codec call, a cutscene or gameplay.

The judgement is made here, from pixels, not by eye - the sweep (tools/sweep_record.py) keeps stills at ~5 s, ~18 s
and the last frame of every recording, and this reads them.

  python tools/classify_scene.py <dir-or-file> [...]     classify, print id,kind,score
  python tools/classify_scene.py --features <dir>        dump every feature as CSV, for tuning the cut-offs
  python tools/classify_scene.py --json out.json <dir>   write {id: {kind, ...}} for the catalog

What separates the three, and why these features:

* **Gameplay** - the psyche gauge sits in the top-left corner: a long horizontal run of saturated amber
  ("OLD SNAKE" over a STRESS readout). A row inside that corner that is largely amber is the giveaway; warm
  scenery is saturated too, but it does not form a bar. Measured at 0.74 on two unrelated gameplay frames - it is
  a fixed-size UI element, so the number is nearly identical wherever it appears.
* **Codec** - the call UI is *ruled*: it draws long straight horizontal edges across the frame, which rendered 3D
  essentially never does. On a 96-frame corpus this separated perfectly - the nine highest `ruled` values (0.039
  to 0.053) were all Codec calls and every other frame sat at or near 0.000 (p90 = 0.003). `greenish` is kept only
  as a weak guard so another UI panel cannot walk in: the Final Results screen measures 0.01 there against 0.34 to
  0.80 for the calls. Darkness and symmetry were tried and dropped - the calls sit over whatever the scene was, so
  their brightness is the scene's, and a flat sky is as symmetric as two portraits.
* **Cutscene** - whatever is left. A scene that draws no gauge and is not a call is a cutscene.

Letterboxing is deliberately NOT used. The port's cutscenes are not letterboxed at the swapchain: the black bands
that appear when one of these frames is viewed are the viewer padding the image, and the pixels underneath are the
scene (row 0 of a title-card frame measures 0.85 luma, not 0.0). It was checked before being relied on.
"""
import sys, os, json, csv, colorsys
import numpy as np
from PIL import Image

# --- cut-offs ---------------------------------------------------------------------------------------------------
# Tuned on the sweep corpus (tools/stage_probe.csv). Each is a plain threshold on a feature below, so a
# misclassification can be traced to one number rather than to a model.
BAR_MIN         = 0.34    # amber fraction of the strongest row in the top-left corner (gameplay measures ~0.74)
RULED_MIN       = 0.025   # fraction of rows that are a ruled edge across the frame
GREEN_MIN       = 0.20    # fraction of lit pixels that are green-dominant


def _load(path, width=640):
    im = Image.open(path).convert("RGB")
    if im.width != width:
        im = im.resize((width, max(1, round(width * im.height / im.width))), Image.BILINEAR)
    return np.asarray(im).astype(np.float32) / 255.0


def features(path):
    a = _load(path)
    h, w, _ = a.shape
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    luma = 0.2126 * r + 0.7152 * g + 0.0722 * b
    mx, mn = a.max(2), a.min(2)
    sat = np.where(mx > 1e-6, (mx - mn) / np.maximum(mx, 1e-6), 0.0)

    # letterbox: rows that are black nearly all the way across, counted in from each edge
    rowdark = (np.percentile(luma, 92, axis=1) < 0.075) & (luma.mean(1) < 0.045)
    def run(seq):
        n = 0
        for v in seq:
            if not v:
                break
            n += 1
        return n / float(h)
    lb_top, lb_bot = run(rowdark), run(rowdark[::-1])
    letterbox = min(lb_top, lb_bot)

    # the psyche gauge: a mostly-amber row in the top-left corner
    y0, y1 = int(0.015 * h), int(0.22 * h)
    x0, x1 = int(0.015 * w), int(0.46 * w)
    reg = a[y0:y1, x0:x1]
    rr, gg, bb = reg[..., 0], reg[..., 1], reg[..., 2]
    rmx, rmn = reg.max(2), reg.min(2)
    rsat = np.where(rmx > 1e-6, (rmx - rmn) / np.maximum(rmx, 1e-6), 0.0)
    amber = (rr > 0.42) & (gg > 0.28) & (bb < 0.62 * rr) & (rsat > 0.42) & (rmx > 0.38)
    bar = float(amber.mean(1).max()) if amber.size else 0.0

    # darkness and colour poverty
    dark = float(luma.mean())
    colourpoor = float(sat.std())

    # left-right symmetry (kept as a diagnostic; on its own a flat sky scores as high as two portraits)
    flip = luma[:, ::-1]
    sym = float(1.0 - np.abs(luma - flip).mean() / max(luma.mean() + 1e-6, 0.05))
    sym = max(0.0, min(1.0, sym))

    # "drawn panel, not rendered scene": rows that are a ruled edge running most of the way across the frame.
    # Rendered 3D is textured everywhere and its edges do not line up with scanlines.
    gx = np.abs(np.diff(luma, axis=1))
    gy = np.abs(np.diff(luma, axis=0))
    g = np.zeros_like(luma)
    g[:, :-1] += gx; g[:, 1:] += gx; g[:-1, :] += gy; g[1:, :] += gy
    flat = float((g < 0.012).mean())
    ruled = float((gy > 0.10).mean(axis=1).__ge__(0.55).mean())

    # green dominance over the lit part of the frame: the call UI is green on black throughout.
    # (the green channel is read from the array again - `g` above was rebound to the gradient buffer)
    green = a[..., 1]
    lit = mx > 0.10
    greenish = float(((green >= r - 0.02) & (green >= b) & (sat > 0.10) & lit).mean())

    return {"letterbox": round(letterbox, 4), "lb_top": round(lb_top, 4), "lb_bot": round(lb_bot, 4),
            "bar": round(bar, 4), "dark": round(dark, 4), "colourpoor": round(colourpoor, 4),
            "sym": round(sym, 4), "flat": round(flat, 4), "ruled": round(ruled, 4),
            "greenish": round(greenish, 4)}


def classify(f):
    """The gauge first (it is the one unambiguous element), then the call screen, then everything else."""
    if f["bar"] >= BAR_MIN:
        return "gameplay", f["bar"]
    if f["ruled"] >= RULED_MIN and f["greenish"] > GREEN_MIN:
        return "codec", f["ruled"]
    return "cutscene", 1.0 - f["bar"] / BAR_MIN


def scene_id(path):
    n = os.path.basename(path)
    for suf in ("_b.jpg", "_a.jpg", ".jpg", ".png"):
        if n.endswith(suf):
            return n[: -len(suf)]
    return os.path.splitext(n)[0]


def gather(args, want="_b.jpg"):
    out = []
    for a in args:
        if os.path.isdir(a):
            out += [os.path.join(a, n) for n in sorted(os.listdir(a)) if n.endswith(want)]
        elif os.path.isfile(a):
            out.append(a)
    return out


def main():
    argv = sys.argv[1:]
    mode, jsonpath = "classify", None
    if argv and argv[0] == "--features":
        mode, argv = "features", argv[1:]
    elif argv and argv[0] == "--json":
        mode, jsonpath, argv = "json", argv[1], argv[2:]
    files = gather(argv)
    if not files:
        print(__doc__)
        return 1

    rows, result = [], {}
    for p in files:
        try:
            f = features(p)
        except Exception as e:
            print("%s: %s" % (p, e), file=sys.stderr)
            continue
        kind, score = classify(f)
        sid = scene_id(p)
        rows.append(dict(id=sid, kind=kind, score=round(score, 3), **f))
        # kind and score, plus `dark` for apply_sweep's lit-or-black test; --features prints the rest as CSV
        result[sid] = {"kind": kind, "score": round(score, 3), "dark": f["dark"]}

    if mode == "features":
        wr = csv.DictWriter(sys.stdout, fieldnames=list(rows[0].keys()), lineterminator="\n")
        wr.writeheader()
        wr.writerows(rows)
    elif mode == "json":
        json.dump(result, open(jsonpath, "w"), indent=1, sort_keys=True)
        tally = {}
        for v in result.values():
            tally[v["kind"]] = tally.get(v["kind"], 0) + 1
        print("%d frames -> %s  (%s)" % (len(result), jsonpath, ", ".join("%s %d" % kv for kv in sorted(tally.items()))))
    else:
        for r in rows:
            print("%-20s %-9s %.2f" % (r["id"], r["kind"], r["score"]))
        tally = {}
        for r in rows:
            tally[r["kind"]] = tally.get(r["kind"], 0) + 1
        print("\n" + ", ".join("%s %d" % kv for kv in sorted(tally.items())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
