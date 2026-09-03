"""Which stage ids are the same scene - decided from what the engine loaded, not from the picture.

  python tools/scene_identity.py               report the groups, write nothing
  python tools/scene_identity.py --apply       write sameAs (and the fingerprint) into scene_info.json
  python tools/scene_identity.py --no-video    never fall back to the video comparison
  python tools/scene_identity.py --scores      print every video score considered

The sweep (tools/sweep_record.py, with AssetTrace=1 in mgs4_dlss.ini) records for every id the named files the
game opened while the scene came up. Three of them are the scene's identity, in this order of strength:

  demo    e_d###.bank - MGS4's own number for a cutscene. Two ids that load the same demo at boot start the same
          cutscene, whatever their names say. Exact: no threshold, no alignment, no recording needed.
  env     env_<stage>_NN.bank - a gameplay entry's environment. Ids that share one are the same place; whether
          they are the same *spawn* the engine does not say, so a group that shares an environment is handed to
          the video comparison (tools/find_duplicates.py) to split, and only then.
  movie   BK2\\*.bk2 - a pre-rendered video, keyed by its file name.

A cutscene that chains a second demo in later is not a different scene from one that stops before it, so only the
demos opened within ten seconds of the first 3D frame count (the sweep's `demo` column; `demo_late` is the rest).

Why this beats the video method it replaces: the recordings do not have to be in step, the loading screen cannot
leak into the comparison, and there is nothing to threshold. The video score is measured on the four s01a40l ids
at 4.98 for the one true duplicate against 24 for the nearest non-duplicate - fine, but it is a guess with a
margin, where the demo number is a fact. What the engine cannot settle (two spawns in one environment) is exactly
what the video can, so that is the one place it is still used.

The primary of a group is the shortest id, which is the bare stage entry where there is one. In scene_info.json an
alias becomes {"sameAs": primary} and nothing else - the launcher reads sameAs before anything else and folds the
row away, so a stale name left beside it would only mislead. Every measured id also gets a "fingerprint" string
("demo 321", "env s01a40l_02", ...) so the reason two rows merged can be read off the file.
"""
import os, re, sys, csv, io, json, itertools

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import paths  # noqa: E402

PROBE = os.path.join(HERE, "stage_probe.csv")
INFO = os.path.join(HERE, "scene_info.json")
# The video comparison's cut-off. Measured on s01a40l: 4.98 for the true duplicate, 24.09 for the nearest
# non-duplicate. 10 sits well inside that gap without drifting toward the far side of it.
VIDEO_MATCH = 10.0


def key_of(row):
    """(strength, value) - the engine's identity for this recording, or None when it said nothing usable."""
    if row.get("demo"):
        return ("demo", row["demo"])
    # the video before the environment: an act opening plays its video and then loads the stage behind it, so
    # the long recording of s01a10l carries an env the 3-second recording of its repeat s01a10l_D1 never reached
    if row.get("movie"):
        return ("movie", row["movie"])
    if row.get("env"):
        return ("env", row["env"])
    return None


def fingerprint_text(row):
    k = key_of(row)
    if not k:
        return ""
    txt = "%s %s" % k
    if k[0] == "demo" and row.get("env"):
        txt += " in env %s" % row["env"]
    if row.get("demo_late"):
        txt += ", then demo %s" % row["demo_late"]
    return txt


class Groups:
    def __init__(self, ids):
        self.parent = {i: i for i in ids}

    def find(self, x):
        while self.parent[x] != x:
            self.parent[x] = self.parent[self.parent[x]]
            x = self.parent[x]
        return x

    def union(self, a, b):
        ra, rb = self.find(a), self.find(b)
        if ra != rb:
            self.parent[ra] = rb

    def groups(self, recorded):
        """Members sorted primary-first: an id with a recording before one without (the sweep cuts a repeat off
        before it has one), then the shortest id, which is the bare stage entry where there is one."""
        out = {}
        for i in self.parent:
            out.setdefault(self.find(i), []).append(i)
        return {k: sorted(v, key=lambda s: (0 if s in recorded else 1, len(s), s)) for k, v in out.items()}


def main():
    argv = sys.argv[1:]
    apply, no_video, scores = "--apply" in argv, "--no-video" in argv, "--scores" in argv
    if not os.path.exists(PROBE):
        print("no %s - run the sweep first" % PROBE)
        return 1
    rows = [r for r in csv.DictReader(open(PROBE, newline="")) if r.get("result") in ("boot", "no-scene")]
    rows = [r for r in rows if key_of(r)]
    print("%d recorded ids with an engine fingerprint" % len(rows))

    byid = {r["id"]: r for r in rows}
    g = Groups(list(byid))
    by_key = {}
    for r in rows:
        by_key.setdefault(key_of(r), []).append(r["id"])
    # Environments overlap rather than match: an entry's `env` is every environment bank it opened while it ran,
    # so the same Codec call recorded for 10 s (s01a30l: env _02) and for 20 s (s01a30l_1: env _01+_02) do not
    # share a key. Ids whose env lists share any bank are one candidate group; the video still decides.
    env_groups = Groups([k for k in by_key if k[0] == "env"])
    bank_owner = {}
    for k in env_groups.parent:
        for bank in k[1].split("+"):
            if bank in bank_owner:
                env_groups.union(k, bank_owner[bank])
            else:
                bank_owner[bank] = k
    merged_env = {}
    for k, members in by_key.items():
        if k[0] == "env":
            merged_env.setdefault(env_groups.find(k), []).extend(members)
    by_key = {k: v for k, v in by_key.items() if k[0] != "env"}
    by_key.update(merged_env)

    video_pairs = 0
    for k, members in sorted(by_key.items()):
        if len(members) < 2:
            continue
        if k[0] in ("demo", "movie"):
            for m in members[1:]:
                g.union(members[0], m)
            continue
        # a shared environment: the same place, maybe not the same spawn. Ask the video.
        if no_video:
            print("   env %-16s shared by %s - not merged (--no-video)" % (k[1], ", ".join(members)))
            continue
        import find_duplicates as fd
        fps = {}
        for m in members:
            fps[m] = fd.fingerprint(m, float(byid[m].get("scene_at") or 0))
        for a, b in itertools.combinations(members, 2):
            if fps.get(a) is None or fps.get(b) is None:
                continue
            # The picture is only asked to tell two entries of *one* stage apart. Two ten-second clips from
            # different stages can score alike (s03a60l's chase leg against s03a10l's street: both open dim and
            # still), and no two stages are the same scene.
            if a.split("_")[0] != b.split("_")[0]:
                continue
            d, shift = fd.best_score(fps[a], fps[b])
            video_pairs += 1
            if scores or d < VIDEO_MATCH:
                print("   env %-16s %-14s %-14s video %6.2f (shift %+.1fs)%s"
                      % (k[1], a, b, d, shift, "  <- same" if d < VIDEO_MATCH else ""))
            if d < VIDEO_MATCH:
                g.union(a, b)

    groups = g.groups({r["id"] for r in rows if r.get("video")})
    dups = {k: v for k, v in groups.items() if len(v) > 1}
    total = sum(len(v) - 1 for v in dups.values())
    print("\n%d video pairs compared; %d groups, folding %d ids into another row\n" % (video_pairs, len(dups), total))
    for members in sorted(dups.values(), key=lambda v: v[0]):
        print("   %-16s <- %-40s  [%s]" % (members[0], ", ".join(members[1:]), fingerprint_text(byid[members[0]])))

    if not apply:
        print("\n(report only - pass --apply to write scene_info.json)")
        return 0

    info = json.load(open(INFO, encoding="utf-8"))
    scenes = info.setdefault("scenes", {})
    aliases = {m: v[0] for v in dups.values() for m in v[1:]}
    wrote = cleared = 0
    for sid in byid:
        if sid in aliases:
            scenes[sid] = {"sameAs": aliases[sid]}
            wrote += 1
            continue
        e = scenes.setdefault(sid, {})
        if "sameAs" in e:
            # measured this time and found to be its own scene: whatever merged it before no longer holds
            e.pop("sameAs"); cleared += 1
        e["fingerprint"] = fingerprint_text(byid[sid])
    io.open(INFO, "w", encoding="utf-8", newline="\n").write(
        json.dumps(info, indent=2, ensure_ascii=False) + "\n")
    print("\nwrote %d sameAs entries (%d earlier ones cleared), fingerprints on %d ids -> %s"
          % (wrote, cleared, len(byid) - wrote, INFO))
    return 0


if __name__ == "__main__":
    sys.exit(main())
