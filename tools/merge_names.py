"""Folds a batch of written names into tools/scene_names.json.

  python tools/merge_names.py work/names_act1.json [more.json ...]

Each input is {id: {name, description, kind?}} as written from an act's naming brief (tools/sweep_sheets.py
--brief). Entries replace whatever scene_names.json had for the same id; the file's _comment and every other id
are kept. Run tools/apply_sweep.py afterwards to carry them into scene_info.json.
"""
import os, sys, io, json

HERE = os.path.dirname(os.path.abspath(__file__))
NAMES = os.path.join(HERE, "scene_names.json")
ALLOWED_KINDS = {"boss", "briefing", "codec", "cutscene", "gameplay", "video", "start"}


def main():
    files = sys.argv[1:]
    if not files:
        print(__doc__)
        return 1
    names = json.load(open(NAMES, encoding="utf-8")) if os.path.exists(NAMES) else {}
    added = replaced = 0
    for f in files:
        batch = json.load(open(f, encoding="utf-8"))
        for sid, e in batch.items():
            if not isinstance(e, dict) or not (e.get("name") or e.get("description")):
                continue
            clean = {}
            if e.get("name"):
                clean["name"] = e["name"].strip()
            if e.get("description"):
                clean["description"] = e["description"].strip()
            if e.get("kind") in ALLOWED_KINDS:
                clean["kind"] = e["kind"]
            # hand-set placement survives a re-naming: a batch only speaks about names
            for keep in ("order", "sortAs"):
                if keep in e:
                    clean[keep] = e[keep]
                elif sid in names and keep in names[sid]:
                    clean[keep] = names[sid][keep]
            if sid in names:
                replaced += 1
            else:
                added += 1
            names[sid] = clean
    # _comment first, then ids in order
    out = {}
    if "_comment" in names:
        out["_comment"] = names["_comment"]
    for sid in sorted(k for k in names if k != "_comment"):
        out[sid] = names[sid]
    io.open(NAMES, "w", encoding="utf-8", newline="\n").write(json.dumps(out, indent=2, ensure_ascii=False) + "\n")
    print("%d added, %d replaced -> %s (%d entries)" % (added, replaced, NAMES, len(out) - ("_comment" in out)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
