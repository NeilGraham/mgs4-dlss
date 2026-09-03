"""Rebuilds tools/stage_probe.csv from the sweep's console log.

The sweep prints a line per id and also appends one to the CSV; if the CSV lost rows (an early version dropped
every row that had no screenshots, which was exactly the crashes) the log is still complete. This reads it back.

  python tools/rebuild_probe_csv.py <sweep.out> [more logs...]

Rows already in the CSV win, so a rebuild never overwrites a real measurement with a parsed one.
"""
import os, re, sys, csv

HERE = os.path.dirname(os.path.abspath(__file__))
PROBE = os.path.join(HERE, "stage_probe.csv")
SHOTS_HINT = "{0}_a.jpg"

LINE = re.compile(r"^\[\s*\d+/\d+\]\s+(\S+)\s+(boot|crash|no-scene)\s+(\d+)s\s+shots:(\d)")


def main():
    logs = sys.argv[1:]
    if not logs:
        print(__doc__)
        return 1

    have = {}
    if os.path.exists(PROBE):
        with open(PROBE, newline="") as fh:
            for row in csv.DictReader(fh):
                have[row["id"]] = row

    added = 0
    for path in logs:
        with open(path, errors="replace") as fh:
            for line in fh:
                m = LINE.match(line.strip())
                if not m:
                    continue
                sid, result, secs, nshots = m.group(1), m.group(2), m.group(3), int(m.group(4))
                if sid in have:
                    continue
                have[sid] = {
                    "id": sid, "result": result, "seconds": secs,
                    "shot_a": ("%s_a.jpg" % sid) if nshots > 0 else "",
                    "shot_b": ("%s_b.jpg" % sid) if nshots > 1 else "",
                    "detail": "3D frame reached" if result == "boot" else
                              ("new minidump" if result == "crash" else "no 3D frame"),
                }
                added += 1

    order = ["id", "result", "seconds", "shot_a", "shot_b", "detail"]
    with open(PROBE, "w", newline="") as fh:
        wr = csv.DictWriter(fh, fieldnames=order, lineterminator="\n")
        wr.writeheader()
        for sid in sorted(have):
            wr.writerow({k: have[sid].get(k, "") for k in order})

    tally = {}
    for r in have.values():
        tally[r["result"]] = tally.get(r["result"], 0) + 1
    print("recovered %d rows from the log; %d total (%s) -> %s" %
          (added, len(have), ", ".join("%s %d" % kv for kv in sorted(tally.items())), PROBE))
    return 0


if __name__ == "__main__":
    sys.exit(main())
