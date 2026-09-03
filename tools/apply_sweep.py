"""Folds the sweep's measurements into tools/scene_info.json, which is what the launcher reads.

  python tools/apply_sweep.py            merge and report
  python tools/apply_sweep.py --dry      say what would change, write nothing

Inputs
  tools/stage_probe.csv    id,result,...            from tools\\sweep_record.py  (boot / crash / no-scene)
  tools/scene_kinds.json   {id: {kind, ...}}        from tools/classify_scene.py --json (codec/cutscene/gameplay)
  tools/scene_names.json   {id: {name, description, kind?, order?, sortAs?}} written by hand from the contact sheets
  tools/scene_hud.json     {id: {boss, item}}        from tools/hud_read.py (OCR of the stills' HUD)
  the game's own data      the location banner per stage, via tools/stage_names.py ("Middle East, Ground Zero")

Rules
  boot      -> kind measured (measured_kind below), visible, name/description if one was written
  crash     -> kind "broken", hidden: it dies in the port's own loader, nothing to show
  no-scene  -> kind "broken", hidden, noted as black rather than crashing - unless it was lit the whole time
               (a pre-rendered video: kind "video") or mgs1.exe was running (the MGS1 dream: kind "gameplay")
An explicit "kind" already in scene_info.json for a semantic category the classifier cannot see ("start",
"briefing") is kept: the classifier judges what is on screen, not what a scene is for.
"""
import os, re, sys, csv, json, io

HERE = os.path.dirname(os.path.abspath(__file__))
PROBE = os.path.join(HERE, "stage_probe.csv")
KINDS = os.path.join(HERE, "scene_kinds.json")
NAMES = os.path.join(HERE, "scene_names.json")
HUD = os.path.join(HERE, "scene_hud.json")
INFO = os.path.join(HERE, "scene_info.json")
# Kinds the classifier must not overwrite. These say what a scene is *for*, which a frame cannot show: a boss
# fight is only distinguishable from ordinary gameplay by the name on the second health bar, and reading text
# needs OCR that is not installed here. They are written by hand in scene_names.json instead.
KEEP_KIND = {"start", "briefing"}
# The engine numbers an act's in-engine cutscenes in the 300s and keeps a 400 series for the ones that are drawn
# UI and schematics rather than a scene - Naomi's slides, EVA's ink sketches, the briefing diagrams (s02a50l_D2 =
# 432, s03a30l_D3/D5/D7 = 433-435, s03a90l_D2 = 436). Those the launcher calls "video".
DIAGRAM_DEMO = 400
# Compass points stay upper-case when the banner's capitals are folded: "MIDTOWN NW SECTOR" -> "Midtown NW Sector".
KEEP_UPPER = {"N", "S", "E", "W", "NE", "NW", "SE", "SW"}


def banner_case(text):
    return " ".join(w if w in KEEP_UPPER else w.capitalize() for w in text.split())


def locations():
    """{stage stem: "Region, Area"} from the game's data; empty when the game folder is not around."""
    try:
        import stage_names as sn
        game = sn.paths.require_game()
        lang = sn.read_lang(game, "en")
        scripts = sn.stage_scripts(game)
    except Exception as e:
        print("no locations (%s)" % e)
        return {}
    out = {}
    for stage, (pak, entry) in scripts.items():
        try:
            region, refs = sn.analyse(sn.vpak_read(pak, entry), lang)
        except Exception:
            continue
        if region is None:
            continue
        parts = [lang.get((region, sn.REGION_NAME), "")]
        area = sn.pick(refs)
        if area is not None and lang.get((region, area)):
            parts.append(lang[(region, area)])
        out[stage] = ", ".join(banner_case(p) for p in parts if p)
    return out


def lit(sid, kinds):
    """Was anything on screen? A video shows a picture; a genuinely broken id shows black. 0.02 mean luma sits
    well below any real frame in the corpus (the darkest measures 0.038) and well above a black screen."""
    f = kinds.get(sid)
    return bool(f) and f.get("features", {}).get("dark", 0.0) > 0.02


def measured_kind(sid, live, state, kinds, hud, demos, envs):
    """What the scene is, from everything measured about it, strongest signal first.

    * A Codec call is read off the still (the call screen's ruled lines): it wins over the engine's "gameplay",
      which is what the engine says under a call (s01a30l), and over the psyche gauge, which stays up beside it.
    * A boss fight is the boss's name on the second health bar, OCR'd from any of the stills (tools/hud_read.py).
      It comes up a few seconds after Snake's own bar, which is why the last frame is read too.
    * A 400-series demo is drawn UI and schematics: "video".
    * The ITEM ACQUIRED card with no HUD reads as a cutscene to the engine; it is a gameplay entry that opened on
      a pickup (s04a30l_D7).
    * An environment bank opened at boot with no demo and no video is a playable entry, whatever the picture
      shows in its first seconds: the motorcycle legs open on a HUD-less shot (s03a35l), the Stryker escape on
      the roof gun (s02a78l). A cutscene entry loads its environment only when it hands over. 65 of 65 such
      recordings so far are gameplay or a Codec call, and the call is caught above.
    * A boss the OCR found beats a hand-written "cutscene"/"gameplay" too: the bar is a fact, the hand is a guess.
    * Otherwise the sweep's live reading (cutscene / gameplay / codec / video), except a demo-at-boot "cutscene"
      whose still shows the gauge while the engine ended in gameplay - an item pickup's one-line demo ahead of
      ordinary play (s02a50l_1). The still's own verdict is the fallback.
    """
    still = kinds.get(sid, {}).get("kind")
    h = hud.get(sid, {})
    k = live.get(sid) if live.get(sid) in ("cutscene", "gameplay", "codec", "video") else None
    if still == "codec" or k == "codec":
        return "codec"
    if h.get("boss"):
        return "boss"
    demo = demos.get(sid, "")
    if demo and all(int(d) >= DIAGRAM_DEMO for d in demo.split("+") if d.isdigit()):
        return "video"
    if h.get("item") and k in ("cutscene", None) and still != "gameplay":
        return "gameplay"
    if k == "cutscene" and still == "gameplay" and state.get(sid) == "gameplay":
        return "gameplay"
    if envs.get(sid) and not demo and k != "video":
        return "gameplay"
    return k or still


def story_order(probe, live, demos, envs, scenes):
    """{id: order} - the position of each measured id inside its stage, on the demo-number scale.

    The engine numbers an act's cutscenes in story order, so a cutscene sorts by the demo it loads at boot. A
    gameplay entry has no demo, but the cutscene that hands over to it loads the same environment bank it does
    (s01a00l_D: demo 307, env s01a00l_00 -> s01a00l: env s01a00l_00), so it sorts right after that cutscene.
    Otherwise the stage's own entry sorts first and a numbered section last, as the launcher always did. A
    400-series (diagram) demo interleaves with the in-engine ones by id, so it takes the position just after
    the highest lower-numbered sibling - the church chain s03a30l_D2..D8 alternates 349, 433, 350, 434, ...
    """
    def stage(sid):
        return sid.split("_")[0]

    def boot_demos(sid):
        return [int(d) for d in demos.get(sid, "").split("+") if d.isdigit()]

    def is_scene(sid):
        return live.get(sid) in ("cutscene", "video") or bool(demos.get(sid))

    by_stage = {}
    for sid, result in probe.items():
        if result in ("boot", "no-scene") and "sameAs" not in (scenes.get(sid) or {}):
            by_stage.setdefault(stage(sid), []).append(sid)
    out = {}
    for st, ids in by_stage.items():
        ids = sorted(ids, key=lambda x: (0 if "_" not in x else 1, x))
        order = {}
        for sid in ids:                                   # in-engine cutscenes, by demo number
            low = [d for d in boot_demos(sid) if d < DIAGRAM_DEMO]
            if low:
                order[sid] = float(min(low))
        for i, sid in enumerate(ids):                     # diagram demos, just after the earlier-id siblings
            if sid in order or not boot_demos(sid):
                continue
            earlier = [order[x] for x in ids[:i] if x in order and order[x] < DIAGRAM_DEMO]
            order[sid] = (max(earlier) if earlier else 0.0) + 0.5
        # gameplay entries: after the cutscene that hands over into the environment they *boot* into (the first
        # bank in their env list - later ones are areas reached within the recording). The stage's own entry
        # opens the stage unless it is gameplay handed over from a cutscene (s01a00l_D -> s01a00l); a Codec call
        # or cutscene there is the arrival itself (s01a30l: the call on entering the Urban Ruins comes before the
        # stage's ambush demo, though they share the environment). A `_D` id with no demo keeps the old middle
        # slot; other numbered sections go last.
        for sid in ids:
            if sid in order:
                continue
            first_env = (envs.get(sid, "").split("+") or [""])[0]
            bare = "_" not in sid
            handover = None
            decided = (scenes.get(sid) or {}).get("kind")      # the measured kind, not the sweep's live guess
            if first_env and (not bare or decided == "gameplay"):
                for other in ids:
                    if other == sid or not is_scene(other) or other not in order:
                        continue
                    theirs = set(envs.get(other, "").split("+")) - {""}
                    if first_env in theirs and (handover is None or order[other] > order[handover]):
                        handover = other
            if handover is not None:
                order[sid] = order[handover] + 0.1
            elif bare:
                order[sid] = 0.0
            elif re.match(r".*_D\d*$", sid):
                order[sid] = 500.0
            else:
                order[sid] = 1000000.0
        out.update(order)
    return out


def main():
    dry = "--dry" in sys.argv[1:]
    if not os.path.exists(PROBE):
        print("no %s - run the sweep first" % PROBE)
        return 1
    probe, live, state, demos, envs = {}, {}, {}, {}, {}
    with open(PROBE, newline="") as fh:
        for row in csv.DictReader(fh):
            probe[row["id"]] = row["result"]
            live[row["id"]] = row.get("kind_live", "")
            state[row["id"]] = row.get("state", "")
            demos[row["id"]] = row.get("demo", "")
            envs[row["id"]] = row.get("env", "")
    kinds = json.load(open(KINDS)) if os.path.exists(KINDS) else {}
    hud = json.load(open(HUD, encoding="utf-8")) if os.path.exists(HUD) else {}
    names = json.load(open(NAMES)) if os.path.exists(NAMES) else {}

    info = json.load(open(INFO, encoding="utf-8"))
    scenes = info.setdefault("scenes", {})
    where = locations()

    counts = {"boot": 0, "crash": 0, "no-scene": 0}
    named = kinded = 0
    for sid, result in sorted(probe.items()):
        counts[result] = counts.get(result, 0) + 1
        e = scenes.get(sid)
        if e is not None and "sameAs" in e:
            continue                      # an alias row: it follows the id it duplicates
        e = scenes.setdefault(sid, {})
        if result == "boot":
            # an id that was measured broken before and boots now: the old verdict's text has to go
            if e.get("description", "").startswith("Measured by tools"):
                e.pop("description", None)
            if e.get("kind") not in KEEP_KIND:
                # The sweep's own reading first: a demo loaded at boot makes a cutscene whatever the ~18 s still
                # shows (by then it may have handed over to gameplay), and it saw the HUD stay up for gameplay.
                # The still decides only what the sweep could not - and a Codec call is read off the still anyway.
                k = measured_kind(sid, live, state, kinds, hud, demos, envs)
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
                if n.get("kind") and not (hud.get(sid, {}).get("boss") and n["kind"] in ("cutscene", "gameplay")):
                    e["kind"] = n["kind"]  # a hand-assigned category wins over the measured one - except a boss bar
                named += 1
        elif result == "no-scene" and lit(sid, kinds):
            # It ran the whole timeout showing something, and the engine never drew a 3D frame. That is what a
            # pre-rendered video looks like from out here: the picture is a decoded stream, not a rendered scene,
            # so the add-on reports no-3d throughout and scene detection never fires. Nothing to hide - it plays.
            if e.get("kind") not in KEEP_KIND:
                # the bundled MGS1 is not a video: the sweep saw mgs1.exe running (s04a05l) - it is played
                e["kind"] = "gameplay" if live.get(sid) == "mgs1" else "video"
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
                         "Measured by tools\\sweep_record.py: the game crashes in its own loader on this id."
                         if result == "crash" else
                         "Measured by tools\\sweep_record.py: boots but never reaches a 3D frame, and the "
                         "screen stays black.")
        if not e:
            scenes.pop(sid, None)

    # the location goes on every entry whose stage the game names, measured or not - it is the game's own word
    located = 0
    for sid, e in scenes.items():
        if "sameAs" in e:
            continue
        loc = where.get(sid.split("_")[0])
        if loc:
            e["location"] = loc
            located += 1
    # story order inside each stage, from the demo numbers (story_order); hand-written order / sortAs win
    ordered = story_order(probe, live, demos, envs, scenes)
    for sid, o in ordered.items():
        e = scenes.get(sid)
        if e is not None and "sameAs" not in e:
            e["order"] = o
    for sid, n in names.items():
        e = scenes.get(sid)
        if e is None or "sameAs" in e or not isinstance(n, dict):
            continue
        for key in ("order", "sortAs"):
            if key in n:
                e[key] = n[key]
    print("probe: %s" % ", ".join("%s %d" % kv for kv in sorted(counts.items())))
    print("story order measured for %d ids" % len(ordered))
    print("locations from the game's data: %d entries" % located)
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
