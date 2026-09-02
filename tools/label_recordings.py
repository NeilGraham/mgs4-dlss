"""Semantic labeling helper for the recorded cutscenes.

  python label_recordings.py thumbs          - pull 3 frames out of every recording into <MGS4_OUT>\\thumbs
                                               (contact sheets to look at, one per recording)
  python label_recordings.py apply           - rename the recordings using labels.json:
                                               {"s02a50l_D1": "act2-naomi-in-the-lab", ...}
                                               -> 02_act2-naomi-in-the-lab_s02a50l_D1.mkv
  python label_recordings.py index           - write index.csv / index.md (order, act, stage, label, length, size)
"""
import csv, json, os, re, subprocess, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import paths                                                     # noqa: E402

OUT_DIR = paths.OUT_DIR
THUMBS = os.path.join(OUT_DIR, "thumbs")
RESULTS = os.path.join(OUT_DIR, "recordings.json")
LABELS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "labels.json")


def load_results():
    return json.load(open(RESULTS)) if os.path.exists(RESULTS) else []


def duration(path):
    try:
        out = subprocess.run([paths.FFPROBE, "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", path],
                             capture_output=True, text=True).stdout.strip()
        return float(out)
    except Exception:
        return 0.0


def thumbs():
    os.makedirs(THUMBS, exist_ok=True)
    for r in load_results():
        if r.get("status") != "ok":
            continue
        path = os.path.join(OUT_DIR, r["file"])
        if not os.path.exists(path):
            continue
        d = duration(path)
        for frac in (0.15, 0.4, 0.7):
            dst = os.path.join(THUMBS, f"{r['index']:02d}_{r['stage']}_{int(frac*100)}.jpg")
            if os.path.exists(dst):
                continue
            subprocess.run([paths.FFMPEG, "-v", "error", "-y", "-ss", str(round(d * frac, 1)), "-i", path,
                            "-frames:v", "1", "-vf", "scale=960:-1", "-q:v", "4", dst], capture_output=True)
        print(f"{r['index']:02d} {r['stage']}: {d:.0f}s -> thumbs")


def apply_labels():
    labels = json.load(open(LABELS)) if os.path.exists(LABELS) else {}
    results = load_results()
    for r in results:
        if r.get("status") != "ok":
            continue
        label = labels.get(r["stage"])
        if not label:
            continue
        src = os.path.join(OUT_DIR, r["file"])
        if not os.path.exists(src):
            continue
        # the label already carries the act ("act4-...", "epilogue-..."), and it is the reliable one: the stage
        # number is not the act (s00a10l holds the epilogue, not the prologue)
        name = f"{r['index']:02d}_{label}_{r['stage']}.mkv"
        dst = os.path.join(OUT_DIR, name)
        if os.path.abspath(src) != os.path.abspath(dst):
            os.replace(src, dst)
            r["file"] = name; r["label"] = label
            print("renamed ->", name)
    json.dump(results, open(RESULTS, "w"), indent=1)


def index():
    results = load_results()
    labels = json.load(open(LABELS)) if os.path.exists(LABELS) else {}
    rows = []
    for r in results:
        if r.get("status") != "ok":
            continue
        path = os.path.join(OUT_DIR, r["file"])
        d = duration(path) if os.path.exists(path) else r.get("seconds", 0)
        rows.append({"order": r["index"], "act": r["act"], "stage": r["stage"],
                     "label": r.get("label") or labels.get(r["stage"], ""),
                     "minutes": round(d / 60, 2), "mb": r.get("mb", 0), "file": r["file"],
                     "end_reason": r.get("end_reason", "")})
    with open(os.path.join(OUT_DIR, "index.csv"), "w", newline="", encoding="utf-8") as f:
        wr = csv.DictWriter(f, fieldnames=list(rows[0].keys()) if rows else ["order"])
        wr.writeheader(); wr.writerows(rows)
    total = sum(r["minutes"] for r in rows)
    with open(os.path.join(OUT_DIR, "index.md"), "w", encoding="utf-8") as f:
        f.write(f"# MGS4 in-game cutscenes (4K60 AV1)\n\n{len(rows)} recordings, {total:.0f} min total\n\n")
        f.write("| # | act | label | stage | length | size | ended by |\n|---|---|---|---|---|---|---|\n")
        for r in rows:
            f.write(f"| {r['order']:02d} | {r['act']} | {r['label']} | `{r['stage']}` | {r['minutes']:.1f} min | {r['mb']:.0f} MB | {r['end_reason']} |\n")
    print(f"{len(rows)} recordings, {total:.0f} min -> index.csv / index.md")


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "index"
    {"thumbs": thumbs, "apply": apply_labels, "index": index}.get(cmd, index)()
