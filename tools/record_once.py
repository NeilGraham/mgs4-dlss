"""Records one boot-into-stage run: starts the OBS recording, boots the stage via launch_stage.ps1 (which presses
through the prompts until the first 3D frame), holds for --seconds of cutscene, stops the recording and prints the
output file path. The game is killed afterwards unless --keep-game.

  python record_once.py [--stage s00a00l] [--seconds 150] [--keep-game]
"""
import argparse, os, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import obs_control as obsc

GAME_DIR = r"C:\Program Files (x86)\Steam\steamapps\common\METAL GEAR SOLID 4\MGS4"

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stage", default="s00a00l")
    ap.add_argument("--seconds", type=float, default=150.0)
    ap.add_argument("--keep-game", action="store_true")
    a = ap.parse_args()

    cl = obsc.client()
    st = obsc.status(cl)
    print("OBS:", st, flush=True)
    if st["recording"]:
        print("already recording - stopping first", flush=True)
        cl.stop_record()
        time.sleep(2)
    if st["scene"] != obsc.SCENE:
        cl.set_current_program_scene(obsc.SCENE)
    cl.start_record()
    print("recording started", flush=True)
    t0 = time.time()
    r = subprocess.run(["powershell", "-ExecutionPolicy", "Bypass", "-File",
                        os.path.join(HERE, "launch_stage.ps1"), "-Stage", a.stage],
                       capture_output=True, text=True, timeout=180)
    print("launch_stage done rc=%d (%.0f s)" % (r.returncode, time.time() - t0), flush=True)
    for line in (r.stdout or "").splitlines()[-6:]:
        print("  ", line, flush=True)
    print("holding for %.0f s of scene..." % a.seconds, flush=True)
    time.sleep(a.seconds)
    out = cl.stop_record()
    path = getattr(out, "output_path", None)
    print("recording stopped:", path, flush=True)
    if not a.keep_game:
        subprocess.run(["taskkill", "/IM", "mgs4.exe", "/F"], capture_output=True)
        print("game killed", flush=True)
    # keep the add-on log next to the clip for correlation
    if path:
        import shutil
        try:
            shutil.copy(os.path.join(GAME_DIR, "logs", "mgs4_dlss.log"), os.path.splitext(path)[0] + ".addon.log")
            print("addon log copied", flush=True)
        except Exception as e:
            print("addon log copy failed:", e, flush=True)

if __name__ == "__main__":
    main()
