"""What is on screen, decided from the picture rather than from engine counters.

Two signals, both measured as a fraction of pixels rather than an average (an average is fooled by MGS4's sepia
scenes; counting saturated HUD-orange pixels is not):

  gameplay   - the solid orange "OLD SNAKE" bar in the top-left corner. Measured on real frames: 0.0 % of that
               region is HUD-orange in cutscenes (four samples), 10-14 % in gameplay.
  end screen - the loading/continue screen: the frame is essentially black except the red MGS4 emblem in the
               bottom-left. Measured: emblem region peaks at 169 luma on an otherwise black frame, while dark
               cutscene frames peak at 42-63.
"""

# regions as fractions of the frame: (x0, y0, x1, y1)
OLD_SNAKE = (0.04, 0.03, 0.22, 0.085)
EMBLEM = (0.04, 0.82, 0.28, 0.92)

GAMEPLAY_MIN_FRAC = 0.03      # 3 % of the OLD SNAKE region orange -> the HUD is up
EMBLEM_MIN_PEAK = 100.0       # emblem is bright against
EMBLEM_MIN_RB = 12.0          # ...a red logo
DARK_FRAME_MAX = 28.0         # ...on an essentially black screen


def _iter(px, w, h, region):
    x0, y0, x1, y1 = region
    for y in range(int(h * y0), max(int(h * y0) + 1, int(h * y1))):
        base = y * w * 3
        for x in range(int(w * x0), max(int(w * x0) + 1, int(w * x1))):
            i = base + x * 3
            yield px[i], px[i + 1], px[i + 2]


def hud_orange_fraction(px, w, h, region=OLD_SNAKE):
    """Fraction of pixels that are the HUD's saturated orange."""
    n = hud = 0
    for r, g, b in _iter(px, w, h, region):
        n += 1
        if r > 100 and r - b > 45 and 0.55 * r < g < 0.95 * r:
            hud += 1
    return hud / max(1, n)


def region_stats(px, w, h, region):
    n = 0; peak = 0.0; rb = 0.0; lum = 0.0
    for r, g, b in _iter(px, w, h, region):
        n += 1
        y = 0.299 * r + 0.587 * g + 0.114 * b
        peak = max(peak, y); lum += y; rb += r - b
    n = max(1, n)
    return {"peak": peak, "mean": lum / n, "rb": rb / n}


def frame_mean_luma(px, w, h):
    tot = 0
    n = w * h
    for i in range(0, n * 3, 3 * 7):        # every 7th pixel is plenty for a brightness check
        tot += 0.299 * px[i] + 0.587 * px[i + 1] + 0.114 * px[i + 2]
    return tot / max(1, len(range(0, n * 3, 3 * 7)))


def classify(px, w, h):
    """Returns ("gameplay" | "end-screen" | "scene", details)."""
    hud = hud_orange_fraction(px, w, h)
    if hud >= GAMEPLAY_MIN_FRAC:
        return "gameplay", {"hud": hud}
    em = region_stats(px, w, h, EMBLEM)
    dark = frame_mean_luma(px, w, h)
    if dark < DARK_FRAME_MAX and em["peak"] > EMBLEM_MIN_PEAK and em["rb"] > EMBLEM_MIN_RB:
        return "end-screen", {"frame_luma": dark, "emblem_peak": em["peak"], "emblem_rb": em["rb"]}
    return "scene", {"hud": hud, "frame_luma": dark, "emblem_peak": em["peak"]}
