"""Shows, from the recordings alone, what the detection decides for a set of ids - and why.

  python tools/verify_detection.py s01a30l s01a30l_1 s03a70l ...     the ids to demonstrate
  python tools/verify_detection.py --cases                            the cases from the 2026-09-03 review

Nothing here is read from scene_info.json or the sweep's live verdicts. Per id it goes back to the artifacts the
sweep left behind and re-derives everything:

  the video          <MGS4_OUT>\\sweep\\video\\<id>.mkv - frames are pulled out with ffmpeg at 5 s, 12 s and the last
                     second of the scene, then read by tools/classify_scene.py (the psyche gauge, the Codec
                     screen's ruled lines) and by the Windows OCR (tools/hud_read.py: the boss bar, ITEM ACQUIRED)
  the engine's log   <MGS4_OUT>\\sweep\\<id>.addon.log - the SCENE-ASSET lines: the demo number, the environment
                     bank, the video file; and the SCENE-STATE line the engine ended on
  the video pairs    tools/find_duplicates.py's fingerprint score between two ids that share an environment

and then applies the same rules tools/apply_sweep.py and tools/scene_identity.py use, printing the evidence next
to the verdict so each one can be checked by eye against the expectation.
"""
import os, re, sys, csv, json, subprocess, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import paths  # noqa: E402
import classify_scene as cs  # noqa: E402
import hud_read as hr  # noqa: E402
import sweep_record as sr  # noqa: E402

SWEEP = os.path.join(paths.OUT_DIR, "sweep")
VIDEO = os.path.join(SWEEP, "video")
PROBE = os.path.join(HERE, "stage_probe.csv")
DIAGRAM_DEMO = 400

# expected verdicts from the review; "merge" names the id it should fold into
CASES = [
    ("s01a00l_D", dict(kind="cutscene", before="s01a00l")),
    ("s01a00l", dict(kind="gameplay")),
    ("s01a10l", dict(kind="cutscene", before="s01a10l_1")),
    ("s01a10l_1", dict(kind="gameplay")),
    ("s01a30l", dict(kind="codec", merge="s01a30l_1", first_in_stage=True)),
    ("s01a30l_1", dict(kind="codec")),
    ("s02a10l_D2", dict(kind="codec")),
    ("s02a50l_D2", dict(kind="video")),
    ("s03a30l_D3", dict(kind="video")),
    ("s03a30l_D4", dict(kind="cutscene")),
    ("s03a30l_D5", dict(kind="video")),
    ("s03a30l_D7", dict(kind="video")),
    ("s03a90l_D2", dict(kind="video")),
    ("s03a35l", dict(kind="gameplay")),
    ("s03a40l", dict(kind="gameplay")),
    ("s03a60l", dict(kind="gameplay", no_merge="s03a10l")),
    ("s03a70l", dict(kind="boss")),
    ("s03a70l_1", dict(kind="boss")),
    ("s04a30l_D7", dict(kind="gameplay")),
    ("s04a30l_2", dict(kind="boss")),
    ("s04a30l_3", dict(kind="boss")),
    ("s02a50l_1", dict(kind="gameplay")),
    ("s04a70l", dict(kind="boss")),
    ("s01a05l_D", dict(merge="s01a05l")),
    ("s01a10l_D1", dict(merge="s01a10l")),
]


def duration(video):
    out = subprocess.run([paths.FFPROBE, "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0",
                          video], capture_output=True, text=True).stdout.strip()
    try:
        return float(out)
    except ValueError:
        return 0.0


def frame(video, at, path):
    subprocess.run([paths.FFMPEG, "-v", "error", "-ss", "%.2f" % max(0.0, at), "-i", video, "-frames:v", "1",
                    "-vf", "scale=1280:-2", "-q:v", "3", "-y", path], capture_output=True)
    return os.path.exists(path)


def engine(sid):
    """demo / env / movie / final state, from the add-on log the sweep kept."""
    p = os.path.join(SWEEP, sid + ".addon.log")
    if not os.path.exists(p):
        return None
    text = open(p, errors="replace").read()
    fp = sr.assets_of(text)
    hits = sr.STATE_RE.findall(text)
    fp["state"] = hits[-1][0] if hits else ""
    fp["first3d"] = "FIRST-3D-FRAME" in text
    return fp


def look(sid, row, tmp):
    """The three frames of the recording, read by the classifier and the OCR."""
    v = os.path.join(VIDEO, sid + ".mkv")
    if not os.path.exists(v):
        return None
    start = float(row.get("scene_at") or 0)
    dur = duration(v)
    # the early frames say what the scene is; the last ten seconds are sampled every two for what shows up late
    # (a boss bar that is only up for a moment around a handover, s03a70l)
    marks = [("5s", start + 5.0), ("12s", start + 12.0)]
    marks += [("end-%d" % back, max(start, dur - 1.0 - back)) for back in (8, 6, 4, 2)]
    marks += [("end", max(start, dur - 1.0))]
    frames = []
    for tag, at in marks:
        if at > dur - 0.2:
            continue
        p = os.path.join(tmp, "%s_%s.jpg" % (sid, tag))
        if frame(v, at, p):
            frames.append((tag, p))
    hud = hr.ocr([p for _, p in frames], 0.0, 0.3, 0.0, 0.5)
    card = hr.ocr([p for _, p in frames], 0.1, 0.6, 0.2, 0.8)
    out = []
    for tag, p in frames:
        f = cs.features(p)
        k = cs.classify(f)[0]
        key = os.path.splitext(os.path.basename(p))[0].lower()
        at = dict(marks)[tag] - start
        out.append(dict(tag=tag, at=at, still=k, bar=f["bar"], ruled=f["ruled"], boss=hr.boss_in(hud.get(key, "")),
                        item=hr.item_in(card.get(key, "")), hud_text=hud.get(key, "")[:40]))
    return out


def decide(eng, frames):
    """apply_sweep.measured_kind, restated over what was just read off the recording.

    What a scene *is* is read from its first twenty seconds - the still at 5 s and 12 s - because a long cutscene
    hands over to gameplay before its recording ends (s01a00l_D: 244 s of cutscene, then the gauge) and a video
    can end on a Codec call (s01a10l). The last frame is read only for what shows up late: the boss's bar, the
    item card.
    """
    early = [f for f in frames if f["at"] <= 20.0]
    stills = [f["still"] for f in early]
    if "codec" in stills:
        return "codec", "Codec screen on an early still (ruled %s)" % max(f["ruled"] for f in early)
    boss = next((f["boss"] for f in frames if f["boss"]), "")
    if boss:
        return "boss", "boss bar reads %r" % boss
    demo = eng.get("demo", "")
    if demo and all(int(d) >= DIAGRAM_DEMO for d in demo.split("+") if d.isdigit()):
        return "video", "demo %s is in the 400 (diagram) series" % demo
    item = next((f["item"] for f in frames if f["item"]), "")
    gauge = any(f["still"] == "gameplay" for f in early)
    if item and not gauge:
        return "gameplay", "ITEM ACQUIRED card (%s), no cutscene" % item
    if eng.get("movie") and not eng.get("first3d"):
        return "video", "video %s opened and no 3D frame was ever drawn" % eng["movie"]
    if demo and gauge and eng.get("state") == "gameplay":
        return "gameplay", "demo %s at boot but the psyche gauge is up early and the engine ended in gameplay" % demo
    if demo or eng.get("movie"):
        return "cutscene", "demo %s%s at boot" % (demo, (" / video " + eng["movie"]) if eng.get("movie") else "")
    if gauge:
        return "gameplay", "psyche gauge on an early still (bar %.2f)" % max(f["bar"] for f in early)
    if eng.get("env"):
        return "gameplay", "environment bank %s opened at boot with no demo: a playable entry" % eng["env"]
    return "cutscene", "no demo, no gauge, no Codec screen in the first 20 s"


def identity_key(eng):
    if eng.get("demo"):
        return "demo " + eng["demo"]
    if eng.get("movie"):
        return "movie " + eng["movie"]
    if eng.get("env"):
        return "env " + eng["env"]
    return ""


def video_score(a, b, rows):
    import find_duplicates as fd
    fa = fd.fingerprint(a, float(rows[a].get("scene_at") or 0))
    fb = fd.fingerprint(b, float(rows[b].get("scene_at") or 0))
    if fa is None or fb is None:
        return None
    return fd.best_score(fa, fb)[0]


def main():
    argv = sys.argv[1:]
    if not argv:
        print(__doc__)
        return 1
    rows = {r["id"]: r for r in csv.DictReader(open(PROBE, newline=""))}
    cases = CASES if "--cases" in argv else [(i, {}) for i in argv]
    tmp = tempfile.mkdtemp(prefix="verify_")
    ok = bad = 0
    results = {}
    for sid, expect in cases:
        row = rows.get(sid)
        eng = engine(sid)
        if row is None or eng is None:
            print("%-12s no recording on disk" % sid)
            if expect.get("merge") and eng:
                pass
            continue
        frames = look(sid, row, tmp) or []
        kind, why = decide(eng, frames) if frames else ("", "no video")
        key = identity_key(eng)
        results[sid] = dict(kind=kind, key=key, eng=eng, frames=frames)
        verdict = ""
        if expect.get("kind"):
            hit = kind == expect["kind"]
            ok += hit; bad += not hit
            verdict = "OK" if hit else "EXPECTED %s" % expect["kind"]
        print("%-12s -> %-9s %-14s %s" % (sid, kind or "(dup)", verdict, why))
        print("               engine: demo=%s env=%s movie=%s state=%s" % (eng["demo"] or "-", eng["env"] or "-",
                                                                          eng["movie"] or "-", eng["state"] or "-"))
        for f in frames:
            print("               frame %-4s (%5.1fs) still=%-9s bar=%.2f ruled=%.3f %s%s" % (
                f["tag"], f["at"], f["still"], f["bar"], f["ruled"],
                ("boss=%s " % f["boss"]) if f["boss"] else "", ("item=%s" % f["item"]) if f["item"] else ""))
    # the merges: same key = same scene; a shared environment is asked of the video
    print()
    for sid, expect in cases:
        other = expect.get("merge") or expect.get("no_merge")
        if not other:
            continue
        ea, eb = engine(sid), engine(other)
        if ea is None or eb is None:
            print("%-12s vs %-12s: a log is missing" % (sid, other)); continue
        ka, kb = identity_key(ea), identity_key(eb)
        same_stage = sid.split("_")[0] == other.split("_")[0]
        if ka and ka == kb and not ka.startswith("env"):
            merged, why = True, "same key: %s" % ka
        elif ka.startswith("env") and kb.startswith("env") and (set(ea["env"].split("+")) & set(eb["env"].split("+"))):
            if not same_stage:
                merged, why = False, "share an environment but are different stages - the picture is not asked"
            elif sid in rows and other in rows and rows[sid].get("video") and rows[other].get("video"):
                d = video_score(sid, other, rows)
                merged = d is not None and d < 10.0
                why = "share an environment; video score %.2f against 10.0" % (d if d is not None else -1)
            else:
                merged, why = False, "share an environment but one has no recording"
        else:
            merged, why = False, "different keys: %s / %s" % (ka or "-", kb or "-")
        want = bool(expect.get("merge"))
        hit = merged == want
        ok += hit; bad += not hit
        print("%-12s %s %-12s %-3s %s" % (sid, "==" if merged else "!=", other, "OK" if hit else "BAD", why))
    # the order: a gameplay entry after the cutscene that hands over into its boot environment
    print()
    for sid, expect in cases:
        b = expect.get("before")
        if not b:
            continue
        ea, eb = engine(sid), engine(b)
        if ea is None or eb is None:
            continue
        first_env = (eb.get("env", "").split("+") or [""])[0]
        hit = bool(first_env) and first_env in set(ea.get("env", "").split("+"))
        ok += hit; bad += not hit
        print("%-12s before %-12s %-3s %s boots into env %s, which %s hands over to (env %s)" % (
            sid, b, "OK" if hit else "BAD", b, first_env or "-", sid, ea.get("env") or "-"))
    print("\n%d checks passed, %d failed" % (ok, bad))
    return 0 if not bad else 2


if __name__ == "__main__":
    sys.exit(main())
