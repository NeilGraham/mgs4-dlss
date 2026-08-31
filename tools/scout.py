"""Boots a list of cutscenes one by one and grabs a couple of frames from each, so the interesting ones can be
picked before spending time recording them.

  python scout.py                      - scout the default candidate list
  python scout.py s03a30l_D1,s04a10l_D1

Frames land in <MGS4_OUT>\\scout\\<stage>_a.jpg (about 12 s in) and _b.jpg (about 45 s in).
"""
import os, sys, time, json, subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from winfocus import find_window, focus, is_foreground, abort_requested   # noqa: E402
from ds4 import DS4                                          # noqa: E402
import obs_control as obsc                                   # noqa: E402
from record_cutscenes import (GAME_DIR, GAME_EXE, ADDON_LOG, Tail, STATE_RE,  # noqa: E402
                              kill_game, start_game, log as rlog)
import paths                                             # noqa: E402

OUT = os.path.join(paths.OUT_DIR, "scout")

# a spread over the whole game: faces, foliage, snow, interiors, action, briefings
CANDIDATES = [
    "s00a10l_D4",   # epilogue: Big Boss and Zero at the grave
    "s01a10l_D2",   # Act 1: Metal Gear Mk. II introduced
    "s01a40l_D3",   # Act 1: Snake meets Meryl
    "s01a60l_D1",   # Act 1 finale / Nomad
    "s02a25l_D1",   # Act 2: jungle, the monkey
    "s02a50l_D1",   # Act 2: Naomi in the lab
    "s02a70l_D",    # Act 2: Laughing Octopus
    "s02a95l_D",    # Act 2 ending
    "s03a00l_D1",   # Act 3: Eastern Europe streets
    "s03a30l_D1",   # Act 3: mid-act
    "s03a70l_D1",   # Act 3: climax
    "s03a90l_D1",   # Act 3 ending
    "s04a10l_D1",   # Act 4: Shadow Moses arrival (snow)
    "s04a30l_D1",   # Act 4: inside the base
    "s04a60l_D1",   # Act 4: REX / RAY
    "s04a75l_D1",   # Act 4 ending
    "s05a20l_D1",   # Act 5: Outer Haven
    "s05a50l_D",    # Act 5: climax
    "s20a00l_D1",   # briefing
]


def shot(cl, path, width=900):
    try:
        cl.save_source_screenshot(obsc.CAPTURE, "jpg", path, width, int(width * 9 / 16), 80)
        return True
    except Exception as e:
        print("   screenshot failed:", e)
        return False


def scout_one(cl, pad, stage, start_timeout=100):
    os.makedirs(OUT, exist_ok=True)
    tail = Tail(ADDON_LOG)
    print(f"--- {stage}", flush=True)
    kill_game()
    hwnd = start_game(stage)
    if not hwnd:
        return {"stage": stage, "status": "no-window"}
    state, t0 = "unknown", time.time()
    while time.time() - t0 < start_timeout:
        if abort_requested():
            print("   Escape pressed", flush=True); kill_game(); return {"stage": stage, "status": "aborted"}
        if not is_foreground(hwnd):
            focus(hwnd)
        pad.tap(hold=0.07)
        for line in tail.new_lines():
            m = STATE_RE.search(line)
            if m:
                state = m.group(1)
        if state == "cutscene":
            break
        time.sleep(0.16)
    if state != "cutscene":
        print("   no cutscene", flush=True)
        kill_game()
        return {"stage": stage, "status": "no-cutscene"}
    t_cut = time.time()
    ok = False
    for mark, tag in ((15, "a"), (55, "b"), (110, "c")):   # act title cards can run for a minute
        while time.time() - t_cut < mark:
            if not is_foreground(hwnd):
                focus(hwnd)
            pad.tap(hold=0.07); time.sleep(0.16)
            if find_window() is None:
                break
        if find_window() is None:
            print("   game exited", flush=True); break
        ok = shot(cl, os.path.join(OUT, f"{stage}_{tag}.jpg")) or ok
    kill_game()
    print("   captured", flush=True)
    return {"stage": stage, "status": "ok" if ok else "partial"}


def main():
    stages = sys.argv[1].split(",") if len(sys.argv) > 1 else CANDIDATES
    cl = obsc.client()
    obsc.ensure_sources(cl)
    cl.set_current_program_scene(obsc.SCENE)
    pad = DS4()
    results = []
    try:
        for st in stages:
            results.append(scout_one(cl, pad, st.strip()))
            json.dump(results, open(os.path.join(OUT, "scout.json"), "w"), indent=1)
    finally:
        pad.close(); kill_game()
    ok = [r for r in results if r["status"] == "ok"]
    print(f"scouted {len(ok)}/{len(results)} -> {OUT}")


if __name__ == "__main__":
    main()
