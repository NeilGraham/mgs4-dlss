"""Cinematic-letterbox probe: MGS4 draws black bars top and bottom during in-game cutscenes and none in gameplay,
so a tiny screenshot of the game capture tells the two apart directly from the picture.

Returns (letterboxed, top, bottom, middle) where the three numbers are mean luminance 0..255, or None when the
frame is unusable (fade to black, capture not ready).
"""
import os, tempfile

PROBE_W, PROBE_H = 128, 72
BAR_ROWS = 4          # ~6 % of the height: the bars are about 7 %
DARK = 12             # a bar is essentially black
LIT = 26              # the picture between the bars has to be clearly brighter


def read_ppm(path):
    """Minimal binary PPM (P6) reader -> (w, h, bytes)."""
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6"):
        return None
    fields, i = [], 2
    while len(fields) < 3:
        while i < len(data) and data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":
            while i < len(data) and data[i] != 0x0A:
                i += 1
            continue
        j = i
        while j < len(data) and not data[j:j + 1].isspace():
            j += 1
        fields.append(int(data[i:j])); i = j
    i += 1
    w, h, _maxv = fields
    return w, h, data[i:i + w * h * 3]


def row_luma(px, w, row):
    base = row * w * 3
    total = 0
    for x in range(w):
        r, g, b = px[base + x * 3], px[base + x * 3 + 1], px[base + x * 3 + 2]
        total += (r * 299 + g * 587 + b * 114) // 1000
    return total / w


def probe(cl, source, path=None):
    path = path or os.path.join(tempfile.gettempdir(), "mgs4_letterbox.ppm")
    try:
        cl.save_source_screenshot(source, "ppm", path, PROBE_W, PROBE_H, -1)
        img = read_ppm(path)
    except Exception:
        return None
    if not img:
        return None
    w, h, px = img
    if len(px) < w * h * 3:
        return None
    top = sum(row_luma(px, w, r) for r in range(1, 1 + BAR_ROWS)) / BAR_ROWS
    bottom = sum(row_luma(px, w, r) for r in range(h - 1 - BAR_ROWS, h - 1)) / BAR_ROWS
    middle = sum(row_luma(px, w, r) for r in range(h // 2 - 6, h // 2 + 6)) / 12
    if middle < LIT:          # fade to black / dark room: no verdict
        return (None, top, bottom, middle)
    return (top < DARK and bottom < DARK, top, bottom, middle)


if __name__ == "__main__":
    import sys, time
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import obs_control as obsc
    cl = obsc.client()
    for _ in range(int(sys.argv[1]) if len(sys.argv) > 1 else 5):
        r = probe(cl, obsc.GAME_SOURCE)
        print("letterboxed=%s top=%.1f bottom=%.1f middle=%.1f" % r if r else "no frame")
        time.sleep(2)
