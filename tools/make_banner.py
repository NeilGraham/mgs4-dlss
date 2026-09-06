"""Cuts the banner behind the launcher's header out of the sweep's recording of the title screen.

  python tools/make_banner.py            -> tools/art/banner.jpg
  python tools/make_banner.py --width 2100 --quality 92

The title screen (`@collection` in the catalog) is Snake's face at the game's full 4K, with the logo down the
right-hand edge and the copyright line in the bottom-left corner. The frame is the one the scene's thumbnail is
cut from - the pick in tools/scene_thumbs.json, at the recording's own resolution rather than the thumbnail's -
cropped close round the face, bandana to the frame's foot, the way Steam's own key art frames it: the logo is
outside the crop and the copyright line, which is inside it, is painted over (it sits on black). The right-hand
quarter is faded to black, so the smoke behind the hair ends in nothing rather than on a line, and so is the
bottom, where the shoulder would otherwise stop on the frame's edge; the header band paints black under the art
(Art.cs draws it a little taller than the band, lifted so the mouth is in the band, and starts the logo past the
face).

Steam's key art (library_hero.jpg, 1920x620) is the same face at a third of the size; the window falls back to it
when this file is not built in.
"""
import os, sys, subprocess, tempfile
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import paths  # noqa: E402
import pick_thumbs as pt  # noqa: E402

SCENE = "@collection"
CROP = (650, 300, 2750, 2160)        # left, top, right, bottom in the 3840x2160 frame: bandana down, cheek to hair
COPYRIGHT = (0, 1950, 1200, 2160)    # the copyright line and the version, bottom-left, on black
FADE_FROM = 0.78                     # the right-hand edge goes to black from here, as a fraction of the crop's width
FADE_BOTTOM = 0.70                   # and the foot from here, as a fraction of its height - long, under the band's own
OUT = os.path.join(HERE, "art", "banner.jpg")


def fade_edges(im, right_from, bottom_from):
    w, h = im.size
    mask = Image.new("L", (w, h), 255)
    d = ImageDraw.Draw(mask)
    x0 = int(w * right_from)
    for x in range(x0, w):
        t = (x - x0) / float(w - x0)
        d.line([(x, 0), (x, h)], fill=int(255 * (1 - t) ** 1.5))
    y0 = int(h * bottom_from)
    for y in range(y0, h):
        t = (y - y0) / float(h - y0)
        k = 1 - t * t * (3 - 2 * t)                          # smoothstep: no corner where the fade begins
        row = mask.crop((0, y, w, y + 1)).point(lambda v, k=k: int(v * k))
        mask.paste(row, (0, y))
    return Image.composite(im, Image.new("RGB", (w, h), (0, 0, 0)), mask)


def main():
    argv = sys.argv[1:]
    width = int(argv[argv.index("--width") + 1]) if "--width" in argv else 2100
    quality = int(argv[argv.index("--quality") + 1]) if "--quality" in argv else 90
    rows, _ = pt.recorded()
    r = rows.get(SCENE)
    p = pt.load_picks().get(SCENE)
    if r is None or p is None:
        sys.exit("%s has no recording or no pick in tools/scene_thumbs.json" % SCENE)
    video = os.path.join(pt.VIDEO, r["video"])
    at = float(r.get("scene_at") or 0) + float(p["offset"])
    frame = os.path.join(tempfile.gettempdir(), "mgs4_banner_frame.png")
    subprocess.run([paths.FFMPEG, "-v", "error", "-ss", "%.2f" % max(0.0, at), "-i", video, "-frames:v", "1",
                    "-y", frame], check=True)
    im = Image.open(frame).convert("RGB")
    if im.size != (3840, 2160):
        sys.exit("the recording is %dx%d; the crop is laid out for 3840x2160" % im.size)
    ImageDraw.Draw(im).rectangle([COPYRIGHT[0], COPYRIGHT[1], COPYRIGHT[2], COPYRIGHT[3]], fill=(0, 0, 0))
    im = fade_edges(im.crop(CROP), FADE_FROM, FADE_BOTTOM)
    if width < im.width:
        im = im.resize((width, round(width * im.height / float(im.width))), Image.LANCZOS)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    im.save(OUT, "JPEG", quality=quality, optimize=True)
    os.remove(frame)
    print("banner: %s at %.1fs -> %s (%dx%d, %d KB)" % (SCENE, at, OUT, im.width, im.height, os.path.getsize(OUT) // 1024))


if __name__ == "__main__":
    main()
