"""Contact sheets for every recording the sweep made, so the scenes can be read and named from a few images each.

  python tools/sweep_sheets.py                  every recorded id without sheets yet
  python tools/sweep_sheets.py --acts 1         only that act (numbers as tools/sweep_record.py takes them)
  python tools/sweep_sheets.py --force          redo the ones already made
  python tools/sweep_sheets.py --brief 1 out.md write the naming brief for act 1 (ids, sheets, what is known)

Per id it runs tools/scene_sheet.py from the scene's own start (`scene_at` in stage_probe.csv), at an interval
that suits what the sweep decided the scene was: 2 s for a 10-second gameplay entry, 5 s for a cutscene (a 5-minute
one comes to four 16-tile sheets), 3 s for a Codec call or a video. Sheets land in <MGS4_OUT>\\sweep\\sheets as
<id>_sheetNN.jpg.

The brief is what a reader who writes the names gets: one section per id with the sheet paths, the kind, the
engine fingerprint (demo / environment / video - docs/scene-identity.md), the location the catalog already knows,
and whatever name was written before, so the new one can be checked against it.
"""
import os, re, sys, csv, json, subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import paths  # noqa: E402
import sweep_record as sr  # noqa: E402

VIDEO = os.path.join(paths.OUT_DIR, "sweep", "video")
SHEETS = os.path.join(paths.OUT_DIR, "sweep", "sheets")
PROBE = os.path.join(HERE, "stage_probe.csv")


def interval_for(row):
    k = row.get("kind_live") or ""
    if k == "gameplay":
        return 2.0
    if k == "cutscene":
        return 5.0
    return 3.0


def sheets_of(sid):
    if not os.path.isdir(SHEETS):
        return []
    return sorted(os.path.join(SHEETS, n) for n in os.listdir(SHEETS)
                  if re.match(re.escape(sid) + r"_sheet\d+\.jpg$", n))


def rows_for(acts):
    rows = list(csv.DictReader(open(PROBE, newline="")))
    if acts:
        by = {r["stage_entry"].strip(): r for r in sr.catalog()}
        rows = [r for r in rows if sr.act_number(by.get(r["id"], {"act": ""})) in acts]
    return rows


def make(rows, force):
    os.makedirs(SHEETS, exist_ok=True)
    made = 0
    for r in rows:
        if not r.get("video"):
            continue
        v = os.path.join(VIDEO, r["video"])
        if not os.path.exists(v):
            continue
        if sheets_of(r["id"]) and not force:
            continue
        start = float(r.get("scene_at") or 0)
        if start < 0:
            start = 0.0
        cmd = [sys.executable, os.path.join(HERE, "scene_sheet.py"), v, "--start", "%.2f" % start,
               "--interval", "%g" % interval_for(r), "--out", SHEETS]
        out = subprocess.run(cmd, capture_output=True, text=True)
        n = len(sheets_of(r["id"]))
        print("%-14s %-9s %d sheet(s)%s" % (r["id"], r.get("kind_live") or r.get("result"), n,
                                           "" if out.returncode == 0 else "  FAILED: " + out.stderr.strip()[-200:]))
        made += n
    return made


def brief(rows, act, out_path):
    names = json.load(open(os.path.join(HERE, "scene_names.json"), encoding="utf-8"))
    info = json.load(open(os.path.join(HERE, "scene_info.json"), encoding="utf-8")).get("scenes", {})
    cat = {r["stage_entry"].strip(): r for r in sr.catalog()}
    import apply_sweep
    where = apply_sweep.locations()
    lines = ["# Naming brief: act %s" % act, ""]
    for r in rows:
        sid = r["id"]
        sh = sheets_of(sid)
        lines.append("## %s" % sid)
        lines.append("- catalog: %s, %s" % (cat.get(sid, {}).get("act", "?"), cat.get(sid, {}).get("kind", "?")))
        if where.get(sid.split("_")[0]):
            lines.append("- location banner (from the game's data): %s" % where[sid.split("_")[0]])
        lines.append("- sweep: result %s, live kind %s, engine state %s, %s" %
                     (r["result"], r.get("kind_live") or "-", r.get("state") or "-", r.get("detail")))
        fp = []
        if r.get("demo"):
            fp.append("demo %s" % r["demo"])
        if r.get("demo_late"):
            fp.append("then demo %s" % r["demo_late"])
        if r.get("env"):
            fp.append("env %s" % r["env"])
        if r.get("movie"):
            fp.append("video %s" % r["movie"])
        lines.append("- engine fingerprint: %s" % (", ".join(fp) or "none"))
        prev = names.get(sid) or {}
        if prev.get("name"):
            lines.append("- earlier name: %s" % prev["name"])
        if prev.get("description"):
            lines.append("- earlier description: %s" % prev["description"])
        e = info.get(sid) or {}
        if e.get("sameAs"):
            lines.append("- scene_info: sameAs %s" % e["sameAs"])
        elif e.get("name"):
            lines.append("- scene_info name: %s" % e["name"])
        stills = [os.path.join(paths.OUT_DIR, "sweep", sid + s) for s in ("_a.jpg", "_b.jpg")]
        stills = [s for s in stills if os.path.exists(s)]
        if sh:
            lines.append("- sheets: " + " | ".join(sh))
        elif stills:
            lines.append("- stills: " + " | ".join(stills))
        else:
            lines.append("- no picture")
        lines.append("")
    open(out_path, "w", encoding="utf-8").write("\n".join(lines))
    print("brief for %d ids -> %s" % (len(rows), out_path))


def main():
    argv = sys.argv[1:]
    acts, force, brief_act, brief_out = None, "--force" in argv, None, None
    i = 0
    while i < len(argv):
        if argv[i] == "--acts":
            acts = {int(a) for a in argv[i + 1].split(",")}; i += 2
        elif argv[i] == "--brief":
            brief_act, brief_out = int(argv[i + 1]), argv[i + 2]; i += 3
        else:
            i += 1
    if brief_act is not None:
        acts = {brief_act}
    rows = rows_for(acts)
    if brief_act is not None:
        brief(rows, brief_act, brief_out)
        return 0
    print("%d sheets" % make(rows, force))
    return 0


if __name__ == "__main__":
    sys.exit(main())
