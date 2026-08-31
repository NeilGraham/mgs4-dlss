"""Finds depth-of-field flash frames in a recording: frames where the picture's high-frequency (sharpness) energy
jumps against its neighbours - a lost/misplaced blur layer sharpens a region for a frame or two - and, for each hit,
where the change sits (quadrant profile), so a blur layer that collapsed into the top-left sub-rect is recognised.

  python dof_flash.py <file.mkv> [--ss S] [--t T] [--out DIR] [--thresh 0.18]

Prints one line per flash with the frame time, the sharpness jump, and the left/right + top/bottom split of the
change; writes the flagged frames (and one neighbour each side) as PNGs into --out for visual confirmation.
"""
import argparse, os, subprocess, sys
import numpy as np

FF = r"C:\Portable\ffmpeg-master-latest-win64-gpl-shared\bin\ffmpeg.exe"
W, H = 960, 540

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument("--ss", type=float, default=0.0)
    ap.add_argument("--t", type=float, default=1e9)
    ap.add_argument("--out", default="")
    ap.add_argument("--thresh", type=float, default=0.18, help="relative sharpness jump vs the window median")
    ap.add_argument("--fps", type=float, default=60.0)
    a = ap.parse_args()

    cmd = [FF, "-v", "error", "-ss", str(a.ss)] + (["-t", str(a.t)] if a.t < 1e8 else []) + [
        "-i", a.path, "-vf", f"fps={a.fps},scale={W}:{H},format=gray", "-f", "rawvideo", "-"]
    raw = subprocess.run(cmd, capture_output=True).stdout
    n = len(raw) // (W * H)
    if n < 10:
        print("too few frames decoded", n); sys.exit(1)
    frames = np.frombuffer(raw[: n * W * H], dtype=np.uint8).reshape(n, H, W).astype(np.float32)

    # per-frame high-frequency energy map: |laplacian|, then quadrant sums
    # (computed in slabs to keep memory sane)
    E = np.empty((n, H - 2, W - 2), dtype=np.float32)
    for i in range(0, n, 120):
        f = frames[i : i + 120]
        lap = f[:, 1:-1, 1:-1] * 4 - f[:, :-2, 1:-1] - f[:, 2:, 1:-1] - f[:, 1:-1, :-2] - f[:, 1:-1, 2:]
        E[i : i + 120] = np.abs(lap)
    e_full = E.mean(axis=(1, 2))                       # global sharpness per frame
    hw, hh = (W - 2) // 2, (H - 2) // 2
    e_tl = E[:, :hh, :hw].mean(axis=(1, 2))            # top-left quadrant
    e_rest = (E.sum(axis=(1, 2)) - E[:, :hh, :hw].sum(axis=(1, 2))) / ((H - 2) * (W - 2) - hh * hw)

    # a flash: e_full (or e_rest) jumps against the median of the surrounding +-6 frames
    hits = []
    for i in range(6, n - 6):
        wnd = np.concatenate([e_rest[i - 6 : i - 1], e_rest[i + 2 : i + 7]])
        med = np.median(wnd)
        if med < 0.3:
            continue
        rel = (e_rest[i] - med) / med
        if abs(rel) >= a.thresh:
            # where did it change: sharpness delta per quadrant against the same window
            med_tl = np.median(np.concatenate([e_tl[i - 6 : i - 1], e_tl[i + 2 : i + 7]]))
            rel_tl = (e_tl[i] - med_tl) / max(med_tl, 0.05)
            hits.append((i, rel, rel_tl))

    # merge runs
    runs, cur = [], []
    for h in hits:
        if cur and h[0] - cur[-1][0] <= 2:
            cur.append(h)
        else:
            if cur:
                runs.append(cur)
            cur = [h]
    if cur:
        runs.append(cur)

    print(f"{n} frames analysed, {len(hits)} flash frames in {len(runs)} runs (thresh {a.thresh})")
    for r in runs:
        i, rel, rel_tl = max(r, key=lambda x: abs(x[1]))
        t = a.ss + i / a.fps
        kind = "REST-SHARPENS(blur lost outside a rect?)" if rel > 0 and rel_tl < rel * 0.5 else (
            "whole-frame " + ("sharpens" if rel > 0 else "blurs"))
        print(f"  f{i:5d}  t={t:7.2f}s  rest {rel:+.2f}  TL {rel_tl:+.2f}  x{len(r)} frames  {kind}")
        if a.out:
            os.makedirs(a.out, exist_ok=True)
            for j in (i - 1, i, i + 1):
                tt = a.ss + j / a.fps
                out = os.path.join(a.out, f"t{tt:08.3f}_f{j}.png")
                subprocess.run([FF, "-v", "error", "-y", "-ss", f"{tt:.4f}", "-i", a.path,
                                "-frames:v", "1", out], capture_output=True)

if __name__ == "__main__":
    main()
