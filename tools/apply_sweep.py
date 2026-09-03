"""Folds the sweep's measurements into tools/scene_info.json, which is what the launcher reads.

  python tools/apply_sweep.py            merge and report
  python tools/apply_sweep.py --dry      say what would change, write nothing

Inputs
  tools/stage_probe.csv    id,result,...            from tools\\sweep_stages.ps1  (boot / crash / no-scene)
  tools/scene_kinds.json   {id: {kind, ...}}        from tools/classify_scene.py --json (codec/cutscene/gameplay)
  tools/scene_names.json   {id: {name, description}} written by hand from the contact sheets

Rules
  boot      -> kind from the classifier, visible, name/description if one was written
  crash     -> kind "broken", hidden: it dies in the port's own loader, nothing to show
  no-scene  -> kind "broken", hidden, noted as black rather than crashing
An explicit "kind" already in scene_info.json for a semantic category the classifier cannot see ("start",
"briefing") is kept: the classifier judges what is on screen, not what a scene is for.
"""
import os, sys, csv, json, io

HERE = os.path.dirname(os.path.abspath(__file__))
PROBE = os.path.join(HERE, "stage_probe.csv")
KINDS = os.path.join(HERE, "scene_kinds.json")
NAMES = os.path.join(HERE, "scene_names.json")
INFO = os.path.join(HERE, "scene_info.json")
# Kinds the classifier must not overwrite. These say what a scene is *for*, which a frame cannot show: a boss
# fight is only distinguishable from ordinary gameplay by the name on the second health bar, and reading text
# needs OCR that is not installed here. They are written by hand in scene_names.json instead.
KEEP_KIND = {"start", "briefing", "boss"}


def lit(sid, kinds):
    """Was anything on screen? A video shows a picture; a genuinely broken id shows black. 0.02 mean luma sits
    well below any real frame in the corpus (the darkest measures 0.038) and well above a black screen."""
    f = kinds.get(sid)
    return bool(f) and f.get("features", {}).get("dark", 0.0) > 0.02


def main():
    dry = "--dry" in sys.argv[1:]
    if not os.path.exists(PROBE):
        print("no %s - run the sweep first" % PROBE)
        return 1
    probe = {}
    with open(PROBE, newline="") as fh:
        for row in csv.DictReader(fh):
            probe[row["id"]] = row["result"]
    kinds = json.load(open(KINDS)) if os.path.exists(KINDS) else {}
    names = json.load(open(NAMES)) if os.path.exists(NAMES) else {}

    info = json.load(open(INFO, encoding="utf-8"))
    scenes = info.setdefault("scenes", {})

    counts = {"boot": 0, "crash": 0, "no-scene": 0}
    named = kinded = 0
    for sid, result in sorted(probe.items()):
        counts[result] = counts.get(result, 0) + 1
        e = scenes.get(sid)
        if e is not None and "sameAs" in e:
            continue                      # an alias row: it follows the id it duplicates
        e = scenes.setdefault(sid, {})
        if result == "boot":
            if e.get("kind") not in KEEP_KIND:
                k = kinds.get(sid, {}).get("kind")
                if k:
                    e["kind"] = k
                    kinded += 1
            e.pop("hidden", None)
            n = names.get(sid)
            if n:
                if n.get("name"):
                    e["name"] = n["name"]
                if n.get("description"):
                    e["description"] = n["description"]
                if n.get("kind"):          # a hand-assigned category wins over the measured one
                    e["kind"] = n["kind"]
                named += 1
        elif result == "no-scene" and lit(sid, kinds):
            # It ran the whole timeout showing something, and the engine never drew a 3D frame. That is what a
            # pre-rendered video looks like from out here: the picture is a decoded stream, not a rendered scene,
            # so the add-on reports no-3d throughout and scene detection never fires. Nothing to hide - it plays.
            if e.get("kind") not in KEEP_KIND:
                e["kind"] = "video"
                kinded += 1
            e.pop("hidden", None)
            n = names.get(sid)
            if n:
                if n.get("name"):
                    e["name"] = n["name"]
                if n.get("description"):
                    e["description"] = n["description"]
                if n.get("kind"):
                    e["kind"] = n["kind"]
                named += 1
        else:
            e["kind"] = "broken"
            e["hidden"] = True
            e.setdefault("description",
                         "Measured by tools\\sweep_stages.ps1: the game crashes in its own loader on this id."
                         if result == "crash" else
                         "Measured by tools\\sweep_stages.ps1: boots but never reaches a 3D frame, and the "
                         "screen stays black.")
        if not e:
            scenes.pop(sid, None)

    print("probe: %s" % ", ".join("%s %d" % kv for kv in sorted(counts.items())))
    print("kinds written: %d, names written: %d, scene_info entries: %d" % (kinded, named, len(scenes)))
    if dry:
        print("(dry run - nothing written)")
        return 0
    txt = json.dumps(info, indent=2, ensure_ascii=False, sort_keys=False)
    io.open(INFO, "w", encoding="utf-8", newline="\n").write(txt + "\n")
    print("wrote %s" % INFO)
    return 0


if __name__ == "__main__":
    sys.exit(main())
