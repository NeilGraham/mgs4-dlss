"""Cuts the banner behind the launcher's header out of the sweep's recording of the title screen.

  python tools/make_banner.py            -> tools/art/banner.jpg
  python tools/make_banner.py --width 3060 --quality 92

The title screen (`@collection` in the catalog) is Snake's face at the game's full 4K, with the logo down the
right-hand edge and the copyright line in the bottom-left corner. The frame is the one the scene's thumbnail is
cut from - the pick in tools/scene_thumbs.json, at the recording's own resolution rather than the thumbnail's -
cropped to the face and the smoke behind it: the logo and the text are outside the crop, the rest of the edges
are black, which is what the header band paints under the art anyway (Art.cs draws it on the left at the band's
height and the logo starts past the face).

Steam's key art (library_hero.jpg, 1920x620) is the same face at a third of the size; the window falls back to it
when this file is not built in.
"""
import os, sys, subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import paths  # noqa: E402
import pick_thumbs as pt  # noqa: E402

SCENE = "@collection"
CROP = (3060, 1950, 0, 0)            # w, h, x, y in the 3840x2160 frame: everything left of the logo, above the text
OUT = os.path.join(HERE, "art", "banner.jpg")


def main():
    argv = sys.argv[1:]
    width = int(argv[argv.index("--width") + 1]) if "--width" in argv else 2560
    quality = int(argv[argv.index("--quality") + 1]) if "--quality" in argv else 90
    rows, _ = pt.recorded()
    r = rows.get(SCENE)
    p = pt.load_picks().get(SCENE)
    if r is None or p is None:
        sys.exit("%s has no recording or no pick in tools/scene_thumbs.json" % SCENE)
    video = os.path.join(pt.VIDEO, r["video"])
    at = float(r.get("scene_at") or 0) + float(p["offset"])
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    # -q:v runs 2 (best) to 31; a JPEG quality of 90 is about 3 on that scale.
    q = max(2, min(31, int(round((100 - quality) / 3.3))))
    subprocess.run([paths.FFMPEG, "-v", "error", "-ss", "%.2f" % max(0.0, at), "-i", video, "-frames:v", "1",
                    "-vf", "crop=%d:%d:%d:%d,scale=%d:-2" % (CROP + (width,)), "-q:v", str(q), "-y", OUT],
                   check=True)
    print("banner: %s at %.1fs -> %s (%d KB)" % (SCENE, at, OUT, os.path.getsize(OUT) // 1024))


if __name__ == "__main__":
    main()
