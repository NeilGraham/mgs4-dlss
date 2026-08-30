"""Frame-pacing check of a recording: per second, duplicated frames (diff ~ 0) and doubled steps (a frame-to-frame
change > 1.7x the second's median, below the cut threshold), plus totals. Usage: python pace_check.py <file> [ss] [t]"""
import subprocess, sys, numpy as np
FF = r"C:\Portable\ffmpeg-master-latest-win64-gpl-shared\bin\ffmpeg.exe"
W, H = 480, 270
path = sys.argv[1]; ss = float(sys.argv[2]) if len(sys.argv) > 2 else 0.0; t = float(sys.argv[3]) if len(sys.argv) > 3 else 60.0
raw = subprocess.run([FF, "-v", "error", "-ss", str(ss), "-t", str(t), "-i", path, "-vf", "fps=60,scale=%d:%d,format=gray" % (W, H), "-f", "rawvideo", "-"], capture_output=True).stdout
n = len(raw) // (W * H); a = np.frombuffer(raw[:n * W * H], dtype=np.uint8).reshape(n, H, W).astype(np.float32)
d = np.abs(np.diff(a, axis=0)).mean(axis=(1, 2))
tot_dup = tot_dbl = 0; bad_secs = 0
for s in range(n // 60):
    x = d[s * 60:(s + 1) * 60]; med = np.median(x)
    dup = int((x < 0.05).sum()); dbl = int(((x > 1.7 * med) & (x < 12) & (med > 0.5)).sum())
    tot_dup += dup; tot_dbl += dbl; bad_secs += (dup + dbl) >= 6
    print("  t=%3ds dup %2d dbl %2d med %.2f" % (s, dup, dbl, med), end="\n" if s % 3 == 2 else "   |   ")
print("\n%s: %d frames, duplicates %d (%.1f%%), doubled steps %d, seconds with >=6 pacing faults: %d of %d" % (path, n, tot_dup, 100.0 * tot_dup / max(1, n), tot_dbl, bad_secs, n // 60))
