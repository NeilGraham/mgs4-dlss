"""OBS control for the cutscene recorder (obs-websocket 5, obsws-python).

  python obs_control.py status              - connection / video / record state
  python obs_control.py setup               - create the "MGS4 Capture" scene with a Game Capture source (mgs4.exe),
                                              cropped to the game's 16:9 image and fitted to the 3840x2160 canvas
  python obs_control.py start               - start recording
  python obs_control.py stop                - stop recording, print the output file
"""
import sys, time, json
import obsws_python as obs

HOST, PORT, PASSWORD = "localhost", 4455, "8TqvlFmdbuWku33O"
SCENE = "MGS4 Capture"
GAME_SOURCE = "MGS4 Game Capture"
DISPLAY_SOURCE = "MGS4 Display Capture"
# The game recreates its swapchain while a stage loads, which makes OBS's Game Capture drop its hook and deliver
# black frames for several seconds - exactly when a cutscene starts. Display capture of the monitor the game runs on
# is immune to that, so it is the default for recording.
MONITOR_ID = None   # resolved at runtime from an existing monitor_capture source, or the first monitor OBS lists
CAPTURE = DISPLAY_SOURCE
AUDIO_SOURCE = "MGS4 Desktop Audio"
# the port always renders a 16:9 image; when the window is not 16:9 it letterboxes/pillarboxes it, so the capture is
# cropped back to 16:9 before being fitted to the 3840x2160 canvas


def client():
    return obs.ReqClient(host=HOST, port=PORT, password=PASSWORD, timeout=10)


def status(cl=None):
    cl = cl or client()
    v = cl.get_video_settings()
    r = cl.get_record_status()
    sc = cl.get_current_program_scene()
    out = {
        "canvas": f"{v.base_width}x{v.base_height}",
        "output": f"{v.output_width}x{v.output_height}",
        "fps": round(v.fps_numerator / v.fps_denominator, 2),
        "scene": getattr(sc, "current_program_scene_name", None) or getattr(sc, "scene_name", None),
        "recording": r.output_active,
        "rec_timecode": r.output_timecode,
        "rec_bytes": r.output_bytes,
    }
    return out


def ensure_scene(cl):
    scenes = [s["sceneName"] for s in cl.get_scene_list().scenes]
    if SCENE not in scenes:
        cl.create_scene(SCENE)
        print("created scene", SCENE)
    return SCENE


def ensure_sources(cl, window=None):
    ensure_scene(cl)
    inputs = [i["inputName"] for i in cl.get_input_list().inputs]
    settings = {
        "capture_mode": "window",
        "window": window or "METAL GEAR SOLID 4 GUNS OF THE PATRIOTS:SDL_app:mgs4.exe",
        "priority": 2,              # match by executable
        "capture_cursor": False,
        "allow_transparency": False,
        "premultiplied_alpha": False,
        "limit_framerate": False,
        "capture_overlays": False,
        "hook_rate": 0,             # fastest hook attempts
        "sli_compatibility": False,
    }
    if GAME_SOURCE not in inputs:
        cl.create_input(SCENE, GAME_SOURCE, "game_capture", settings, True)
        print("created", GAME_SOURCE)
    else:
        cl.set_input_settings(GAME_SOURCE, settings, True)
        items = {i["sourceName"]: i["sceneItemId"] for i in cl.get_scene_item_list(SCENE).scene_items}
        if GAME_SOURCE not in items:
            cl.create_scene_item(SCENE, GAME_SOURCE, True)
    monitor = MONITOR_ID
    if not monitor:   # reuse the monitor id of an existing display-capture source (the game's 4K display)
        for name in inputs:
            try:
                st = cl.get_input_settings(name).input_settings
            except Exception:
                continue
            if "monitor_id" in st and "SMKD1CE" in str(st["monitor_id"]):
                monitor = st["monitor_id"]; break
        if not monitor:
            for name in inputs:
                try:
                    st = cl.get_input_settings(name).input_settings
                except Exception:
                    continue
                if "monitor_id" in st:
                    monitor = st["monitor_id"]; break
    dsettings = {"capture_cursor": False}
    if monitor:
        dsettings["monitor_id"] = monitor
    if DISPLAY_SOURCE not in inputs:
        cl.create_input(SCENE, DISPLAY_SOURCE, "monitor_capture", dsettings, True)
        print("created", DISPLAY_SOURCE)
    else:
        cl.set_input_settings(DISPLAY_SOURCE, dsettings, True)
        items = {i["sourceName"]: i["sceneItemId"] for i in cl.get_scene_item_list(SCENE).scene_items}
        if DISPLAY_SOURCE not in items:
            cl.create_scene_item(SCENE, DISPLAY_SOURCE, True)
    if AUDIO_SOURCE not in inputs:
        cl.create_input(SCENE, AUDIO_SOURCE, "wasapi_output_capture", {"device_id": "default"}, True)
        print("created", AUDIO_SOURCE)
    # only one video source visible: the display capture
    items = {i["sourceName"]: i["sceneItemId"] for i in cl.get_scene_item_list(SCENE).scene_items}
    for name, item in items.items():
        if name in (GAME_SOURCE, DISPLAY_SOURCE):
            cl.set_scene_item_enabled(SCENE, item, name == CAPTURE)
    return True


def fit_game(cl):
    """Crop the letterbox bars and stretch the remaining 16:9 image over the whole 4K canvas."""
    v = cl.get_video_settings()
    items = {i["sourceName"]: i["sceneItemId"] for i in cl.get_scene_item_list(SCENE).scene_items}
    if CAPTURE not in items:
        print("capture source not in scene yet"); return False
    item = items[CAPTURE]
    tr = cl.get_scene_item_transform(SCENE, item).scene_item_transform
    sw, sh = tr["sourceWidth"], tr["sourceHeight"]
    if not sw or not sh:
        print("source has no size yet (game not captured); leaving transform alone"); return False
    target = 16.0 / 9.0
    top = bottom = left = right = 0
    if sw / sh < target - 1e-3:          # letterboxed (window taller than 16:9)
        bars = (sh - sw / target) / 2.0
        top = int(bars + 0.5); bottom = int(sh - sw / target - top + 0.5)
    elif sw / sh > target + 1e-3:        # pillarboxed
        bars = (sw - sh * target) / 2.0
        left = int(bars + 0.5); right = int(sw - sh * target - left + 0.5)
    vis_w, vis_h = sw - left - right, sh - top - bottom
    cl.set_scene_item_transform(SCENE, item, {
        "positionX": 0.0, "positionY": 0.0, "rotation": 0.0,
        "cropTop": top, "cropBottom": bottom, "cropLeft": left, "cropRight": right,
        "scaleX": v.base_width / vis_w, "scaleY": v.base_height / vis_h,
        "alignment": 5,   # top-left
        "boundsType": "OBS_BOUNDS_NONE",
    })
    print(f"fitted {sw}x{sh} (crop l{left} r{right} t{top} b{bottom} -> {vis_w}x{vis_h}) to {v.base_width}x{v.base_height}")
    return True


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "status"
    cl = client()
    if cmd == "status":
        print(json.dumps(status(cl), indent=2))
    elif cmd == "setup":
        window = sys.argv[2] if len(sys.argv) > 2 else None
        ensure_sources(cl, window)
        cl.set_current_program_scene(SCENE)
        time.sleep(1.5)
        fit_game(cl)
        print(json.dumps(status(cl), indent=2))
    elif cmd == "fit":
        cl.set_current_program_scene(SCENE)
        fit_game(cl)
    elif cmd == "shot":
        path = sys.argv[2] if len(sys.argv) > 2 else r"D:\mgs4-dlss5\shot.png"
        width = int(sys.argv[3]) if len(sys.argv) > 3 else 1280
        cl.save_source_screenshot(CAPTURE, "png", path, width, int(width * 9 / 16), -1)
        print("saved", path)
    elif cmd == "start":
        cl.start_record(); time.sleep(1)
        print(json.dumps(status(cl), indent=2))
    elif cmd == "stop":
        r = cl.stop_record()
        print("saved:", getattr(r, "output_path", "?"))
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
