"""The frame each scene is shown by: chosen by eye from its contact sheets, kept by timestamp, cut from the video.

  python tools/pick_thumbs.py --brief 1 work/thumbs_brief_act1.md   what a picker needs for act 1: every sheet of every scene
  python tools/pick_thumbs.py --missing                              scenes recorded but not yet picked
  python tools/pick_thumbs.py --merge work/thumbs_act1.json          fold a picker's answers into tools/scene_thumbs.json
  python tools/pick_thumbs.py --apply                                cut every picked frame out of its recording
  python tools/pick_thumbs.py --apply --force                        ...again, even where a frame already exists

The pick is a **timestamp**, not a picture, and it lives in tools/scene_thumbs.json:

    "s01a40l_2": {"offset": 15.0, "why": "Naomi across the table, both faces lit"}

`offset` is seconds into the scene - the number captioned on the sheet's tile - so the same pick survives a
re-recording (the recorder's `scene_at` says where the scene starts in the file, and that is added at apply time).
`--apply` cuts that frame out of <MGS4_OUT>\\sweep\\video\\<id>.mkv at 1280 wide into <MGS4_OUT>\\sweep\\thumbs\\<id>.jpg,
which tools/make_thumbs.py prefers over the sweep's own stills. A scene with no pick keeps the sweep's still.

What makes a frame the one to show, for whoever (or whatever) reads the brief: the scene at its most
recognisable - the character or place a player would name it by, lit and in focus, mid-action rather than
mid-fade; not the loading screen, not a black or white frame, not the desktop a recording's tail can drift onto,
not a title card unless the scene *is* the title card, and not a subtitle-only or menu frame. For gameplay the
HUD is fine. Prefer a tile a few seconds in over the first, which is often still fading up.
"""
import os, re, sys, csv, io, json, subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import paths  # noqa: E402
import sweep_record as sr  # noqa: E402
import sweep_sheets as ss  # noqa: E402

PROBE = os.path.join(HERE, "stage_probe.csv")
INFO = os.path.join(HERE, "scene_info.json")
PICKS = os.path.join(HERE, "scene_thumbs.json")
VIDEO = os.path.join(paths.OUT_DIR, "sweep", "video")
THUMBS = os.path.join(paths.OUT_DIR, "sweep", "thumbs")
WIDTH = 1280


def load_picks():
    return json.load(open(PICKS, encoding="utf-8")) if os.path.exists(PICKS) else {}


def save_picks(p):
    io.open(PICKS, "w", encoding="utf-8", newline="\n").write(json.dumps(p, indent=1, ensure_ascii=False, sort_keys=True) + "\n")


def recorded(acts=None):
    """{id: probe row} for every recorded scene that stands as its own row (aliases fold into their primary)."""
    info = json.load(open(INFO, encoding="utf-8")).get("scenes", {})
    by = {r["stage_entry"].strip(): r for r in sr.catalog()}
    out = {}
    for r in csv.DictReader(open(PROBE, newline="")):
        if not r.get("video") or r.get("result") not in ("boot", "no-scene") or "sameAs" in info.get(r["id"], {}):
            continue      # a crash row has a recording too - eleven seconds of the auto-save notice
        if acts and sr.act_number(by.get(r["id"], {"act": ""})) not in acts:
            continue
        out[r["id"]] = r
    return out, info


def brief(act, out_path):
    rows, info = recorded({act})
    lines = ["# Thumbnail brief: act %s" % act, "",
             "Pick one tile per scene. Answer with the tile's captioned offset (the +Ns in its corner).", ""]
    for sid, r in rows.items():
        e = info.get(sid, {})
        sheets = ss.sheets_of(sid)
        lines.append("## %s" % sid)
        lines.append("- kind: %s; name: %s" % (e.get("kind", "?"), e.get("name", "")))
        if e.get("description"):
            lines.append("- about: %s" % e["description"])
        lines.append("- tiles every %g s" % ss.interval_for(r))
        lines.append("- sheets: " + (" | ".join(sheets) if sheets else "none"))
        lines.append("")
    io.open(out_path, "w", encoding="utf-8", newline="\n").write("\n".join(lines))
    print("brief for %d scenes -> %s" % (len(rows), out_path))


def missing():
    rows, _ = recorded()
    picks = load_picks()
    gone = [i for i in rows if i not in picks]
    for i in gone:
        print(i)
    print("%d recorded scenes, %d without a pick" % (len(rows), len(gone)))


def merge(files):
    picks = load_picks()
    rows, _ = recorded()
    n = 0
    for f in files:
        for sid, e in json.load(open(f, encoding="utf-8")).items():
            if not isinstance(e, dict) or "offset" not in e or sid not in rows:
                continue
            picks[sid] = {"offset": float(e["offset"]), "why": (e.get("why") or "").strip()}
            n += 1
    save_picks(picks)
    print("%d picks merged -> %s (%d total)" % (n, PICKS, len(picks)))


def apply(force):
    rows, _ = recorded()
    picks = load_picks()
    os.makedirs(THUMBS, exist_ok=True)
    made = skipped = 0
    for sid, p in picks.items():
        r = rows.get(sid)
        if r is None:
            continue
        dst = os.path.join(THUMBS, sid + ".jpg")
        if os.path.exists(dst) and not force:
            skipped += 1
            continue
        v = os.path.join(VIDEO, r["video"])
        at = float(r.get("scene_at") or 0) + float(p["offset"])
        subprocess.run([paths.FFMPEG, "-v", "error", "-ss", "%.2f" % max(0.0, at), "-i", v, "-frames:v", "1",
                        "-vf", "scale=%d:-2" % WIDTH, "-q:v", "2", "-y", dst], capture_output=True)
        if os.path.exists(dst):
            made += 1
        else:
            print("   no frame for %s at %.1fs" % (sid, at))
    print("%d frames cut (%d already there) -> %s" % (made, skipped, THUMBS))


def main():
    argv = sys.argv[1:]
    if "--brief" in argv:
        i = argv.index("--brief")
        brief(int(argv[i + 1]), argv[i + 2])
    elif "--missing" in argv:
        missing()
    elif "--merge" in argv:
        merge(argv[argv.index("--merge") + 1:])
    elif "--apply" in argv:
        apply("--force" in argv)
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
