"""Cuts a recording into stills at a fixed interval and lays them out as contact sheets for reading.

  python tools/scene_sheet.py <video> --start 13 --interval 5
  python tools/scene_sheet.py <video> --start 13 --interval 5 --cols 3 --per-sheet 12 --out D:\\out

A **grid**, deliberately, not one wide strip. Whatever reads these gets a fixed pixel budget and scales the image
to fit it, so an extreme aspect ratio spends the whole budget on width: 24 frames in a single row come out about
65px each, which is too small to read a subtitle or a HUD label. The same 24 frames as 4x6 are about 390px each
and legible. Legibility is the point - the subtitles are what identify a scene ("the war economy" rather than
"a Codec call"), so anything that makes them unreadable defeats the exercise.

Each tile is captioned with its timestamp into the scene, so a description can cite when something happens.
"""
import os, sys, subprocess, math

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import paths  # noqa: E402
from PIL import Image, ImageDraw, ImageFont  # noqa: E402


def font(size):
    for f in (r"C:\Windows\Fonts\segoeui.ttf", r"C:\Windows\Fonts\arial.ttf"):
        if os.path.exists(f):
            try:
                return ImageFont.truetype(f, size)
            except Exception:
                pass
    return ImageFont.load_default()


def duration(video):
    out = subprocess.run([paths.FFPROBE, "-v", "error", "-show_entries", "format=duration",
                          "-of", "csv=p=0", video], capture_output=True, text=True).stdout.strip()
    try:
        return float(out)
    except ValueError:
        return 0.0


def grab(video, at, path, width):
    subprocess.run([paths.FFMPEG, "-v", "error", "-ss", str(at), "-i", video, "-frames:v", "1",
                    "-vf", "scale=%d:-2" % width, "-q:v", "3", "-y", path], capture_output=True)
    return os.path.exists(path)


def main():
    argv = sys.argv[1:]
    if not argv:
        print(__doc__)
        return 1
    video = argv[0]
    # 4 columns of 480px tiles, 16 to a sheet: measured, not guessed. At 480 the subtitle line in a 4K frame
    # survives the downscale at about 7px tall and reads cleanly; at 5+ columns it falls under ~5px and
    # breaks up. Grid shape itself costs nothing - the budget is per sheet, so more columns only buys
    # smaller tiles. 3-4s suits dialogue (every subtitle line lands on at least one frame); 5s is plenty
    # for gameplay, where nothing is being said.
    start, interval, cols, per_sheet, tile = 0.0, 4.0, 4, 16, 480
    out_dir = os.path.join(paths.OUT_DIR, "sweep", "sheets")
    i = 1
    while i < len(argv):
        a = argv[i]
        if a == "--start":       start = float(argv[i + 1]); i += 2
        elif a == "--interval":  interval = float(argv[i + 1]); i += 2
        elif a == "--cols":      cols = int(argv[i + 1]); i += 2
        elif a == "--per-sheet": per_sheet = int(argv[i + 1]); i += 2
        elif a == "--tile":      tile = int(argv[i + 1]); i += 2
        elif a == "--out":       out_dir = argv[i + 1]; i += 2
        else: i += 1

    dur = duration(video)
    if dur <= 0:
        print("cannot read %s" % video)
        return 1
    stem = os.path.splitext(os.path.basename(video))[0]
    tmp = os.path.join(out_dir, "_frames")
    os.makedirs(tmp, exist_ok=True)

    marks = []
    t = start
    while t < dur - 0.3:
        marks.append(t)
        t += interval
    print("%s: %.1fs, %d frames from %.1fs every %.1fs" % (stem, dur, len(marks), start, interval))

    shots = []
    for t in marks:
        p = os.path.join(tmp, "%s_%06.1f.jpg" % (stem, t))
        if grab(video, t, p, tile):
            shots.append((t - start, p))

    th = int(tile * 9 / 16)
    # The timestamp is drawn *on* the frame rather than on a strip above it. A caption band costs a row of pixels
    # per row of tiles and carries almost no information; those pixels are worth more spent on the picture, since
    # what has to survive the downscale is small text inside the frame (subtitles, HUD labels).
    pad = 2
    made = 0
    f = font(max(14, tile // 26))
    for s in range(0, len(shots), per_sheet):
        chunk = shots[s:s + per_sheet]
        rows = math.ceil(len(chunk) / cols)
        W = cols * (tile + pad) + pad
        H = rows * (th + pad) + pad
        sheet = Image.new("RGB", (W, H), (16, 16, 18))
        d = ImageDraw.Draw(sheet)
        for n, (off, p) in enumerate(chunk):
            x = pad + (n % cols) * (tile + pad)
            y = pad + (n // cols) * (th + pad)
            try:
                sheet.paste(Image.open(p).convert("RGB").resize((tile, th)), (x, y))
            except Exception:
                pass
            label = "+%0.0fs" % off
            # outlined, so it stays readable over a bright frame as well as a dark one
            for dx, dy in ((-1, 0), (1, 0), (0, -1), (0, 1)):
                d.text((x + 6 + dx, y + 4 + dy), label, font=f, fill=(0, 0, 0))
            d.text((x + 6, y + 4), label, font=f, fill=(255, 236, 140))
        out = os.path.join(out_dir, "%s_sheet%02d.jpg" % (stem, s // per_sheet))
        sheet.save(out, quality=86)
        made += 1
        print("  ", out)
    print("%d sheet(s), %d frames" % (made, len(shots)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
