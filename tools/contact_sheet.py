"""Lays the sweep's frames out as labelled contact sheets, so a few hundred scenes can be read in a couple of
dozen images instead of a few hundred.

  python tools/contact_sheet.py                        every booting scene, 8 per sheet
  python tools/contact_sheet.py --rows 6 --width 300   smaller sheets
  python tools/contact_sheet.py --only s02a20l_1,...   just those ids

One row per scene: the 10 s frame beside the 20 s frame, captioned with the id and the kind the classifier gave it
(tools/classify_scene.py). Sheets land in <MGS4_OUT>\\sweep\\sheets\\sheet_NN.jpg.
"""
import os, sys, csv, json
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import paths  # noqa: E402

SHOTS = os.path.join(paths.OUT_DIR, "sweep")
SHEETS = os.path.join(SHOTS, "sheets")
PROBE = os.path.join(HERE, "stage_probe.csv")
KINDS = os.path.join(HERE, "scene_kinds.json")


def font(size):
    for f in (r"C:\Windows\Fonts\segoeui.ttf", r"C:\Windows\Fonts\arial.ttf"):
        if os.path.exists(f):
            try:
                return ImageFont.truetype(f, size)
            except Exception:
                pass
    return ImageFont.load_default()


def booting_ids():
    if not os.path.exists(PROBE):
        return []
    out = []
    with open(PROBE, newline="") as fh:
        for row in csv.DictReader(fh):
            if row.get("result") == "boot":
                out.append(row["id"])
    return out


def main():
    argv = sys.argv[1:]
    rows_per, width, only = 8, 340, None
    while argv:
        if argv[0] == "--rows":
            rows_per, argv = int(argv[1]), argv[2:]
        elif argv[0] == "--width":
            width, argv = int(argv[1]), argv[2:]
        elif argv[0] == "--only":
            only, argv = set(argv[1].split(",")), argv[2:]
        else:
            argv = argv[1:]

    kinds = json.load(open(KINDS)) if os.path.exists(KINDS) else {}
    ids = [i for i in booting_ids() if not only or i in only]
    if not ids:
        print("no booting ids in %s - run the sweep first" % PROBE)
        return 1

    os.makedirs(SHEETS, exist_ok=True)
    cap_h, pad = 26, 6
    thumb_h = round(width * 9 / 16)
    row_h = thumb_h + cap_h + pad
    sheet_w = width * 2 + pad * 3
    f_id, f_sub = font(17), font(14)
    made = 0

    for start in range(0, len(ids), rows_per):
        chunk = ids[start:start + rows_per]
        sheet = Image.new("RGB", (sheet_w, row_h * len(chunk) + pad), (18, 18, 20))
        d = ImageDraw.Draw(sheet)
        for r, sid in enumerate(chunk):
            y = pad + r * row_h
            for c, suf in enumerate(("a", "b")):
                p = os.path.join(SHOTS, "%s_%s.jpg" % (sid, suf))
                x = pad + c * (width + pad)
                if os.path.exists(p):
                    im = Image.open(p).convert("RGB").resize((width, thumb_h), Image.BILINEAR)
                    sheet.paste(im, (x, y + cap_h))
                else:
                    d.rectangle([x, y + cap_h, x + width, y + cap_h + thumb_h], fill=(40, 40, 44))
                    d.text((x + 8, y + cap_h + thumb_h // 2), "(no frame)", font=f_sub, fill=(150, 150, 150))
            k = kinds.get(sid, {})
            label = "%s   [%s]" % (sid, k.get("kind", "?"))
            d.text((pad + 2, y + 4), label, font=f_id, fill=(235, 235, 240))
            d.text((pad + width + 10, y + 6), "left 10s / right 20s", font=f_sub, fill=(130, 130, 140))
        out = os.path.join(SHEETS, "sheet_%02d.jpg" % (start // rows_per))
        sheet.save(out, quality=78)
        made += 1
        print("%s  (%d scenes)" % (out, len(chunk)))
    print("\n%d sheets covering %d scenes -> %s" % (made, len(ids), SHEETS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
