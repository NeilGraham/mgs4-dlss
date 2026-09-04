"""Reads the HUD text off the sweep's stills: a boss's name on the second health bar, and the ITEM ACQUIRED card.

  python tools/hud_read.py                    every id with stills under <MGS4_OUT>\\sweep -> tools/scene_hud.json
  python tools/hud_read.py --only s03a70l,... just those
  python tools/hud_read.py --force            re-read ids already in the file

Why OCR and not pixels: a boss fight differs from ordinary gameplay by one thing - a second bar under OLD SNAKE's
with the boss's name on it - and that bar occupies the same rows the STRESS readout does, so its shape says
nothing; only the text does. Windows ships an OCR engine (tools/ocr.ps1 wraps it), and on the top-left quarter of
a 1280-wide still it reads "OLD SNAKE RAGING RAVEN" well enough, typos and all: the names come back as "RRCäXNCä
RRVEN" or "CRYING BERUTY", so they are matched loosely, after folding the letters the engine confuses (R/A,
X/I, ä/G). The item card is the opposite case: a black screen with "ITEM ACQUIRED" over it, no HUD at all, which
the engine reads as a cutscene (s04a30l_D7) when it is a gameplay entry that opened on a pickup.

Every still of an id is read (_a at 5 s, _b at 9-18 s, _z the last frame), because the boss bar comes up a few
seconds after Snake's own and an item card is only there until a button is pressed.

Output: tools/scene_hud.json  {id: {"boss": "Raging Raven" | "", "item": "FaceCamo (Crying Beauty)" | ""}}
"""
import os, re, sys, json, io, difflib, subprocess, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import paths  # noqa: E402

SHOTS = os.path.join(paths.OUT_DIR, "sweep")
OUT = os.path.join(HERE, "scene_hud.json")
OCR = os.path.join(HERE, "ocr.ps1")

# The names MGS4 writes on a second health bar. Short generic words are deliberately absent: "FROG" matched the
# word "from" on a desktop that a recording's tail had drifted onto, and nothing in the game puts FROG on a bar.
BOSSES = ["LAUGHING OCTOPUS", "LAUGHING BEAUTY", "RAGING RAVEN", "RAGING BEAUTY", "CRYING WOLF", "CRYING BEAUTY",
          "SCREAMING MANTIS", "SCREAMING BEAUTY", "VAMP", "LIQUID OCELOT", "METAL GEAR RAY", "PSYCHO MANTIS"]
# The bar that is the player's own. A boss name only counts on a frame that also shows one of these: the second
# bar hangs under the first, and without the guard the desktop in a recording's tail once matched "FROG".
# METAL GEAR REX is the player's bar in the REX vs RAY fight (s04a70l), where there is no OLD SNAKE at all.
PLAYER_BARS = ["OLD SNAKE", "METAL GEAR REX"]
# letters the engine trades for each other on the HUD font; folded on both sides before comparing
FOLD = str.maketrans({"R": "A", "ä": "G", "Ä": "G", "X": "I", "B": "E", "0": "O", "1": "I", "5": "S", "8": "B"})


def fold(s):
    return re.sub(r"[^A-Z ]", "", s.upper().translate(FOLD))


def _windows(t, n):
    words = t.split()
    for i in range(len(words)):
        for j in range(i + 1, min(len(words), i + n) + 1):
            yield " ".join(words[i:j])


def _close(t, target, ratio=0.8):
    """Does some run of up to three words of the folded text match `target` loosely? (OLD SNAKE comes back as
    OLD SNAAE, METAL GEAR REX as METAL GEAA AEX.)"""
    return any(difflib.SequenceMatcher(None, w, target).ratio() >= ratio for w in _windows(t, 3))


def _strip(t, target, ratio=0.8):
    """The text with the single best loose match of `target` blanked - one bar per name per frame. Blanking every
    match at 0.8 took METAL GEAR RAY out along with METAL GEAR REX (they differ by one letter)."""
    words = t.split()
    best, at = 0.0, None
    for i in range(len(words)):
        for j in range(i + 1, min(len(words), i + 3) + 1):
            r = difflib.SequenceMatcher(None, " ".join(words[i:j]), target).ratio()
            if r > best:
                best, at = r, (i, j)
    if at is None or best < ratio:
        return t
    return " ".join(w for k, w in enumerate(words) if not (at[0] <= k < at[1]))


def boss_in(text):
    """The boss name the text most plausibly carries, or ''. OLD SNAKE and STRESS are stripped first so the
    comparison is against the bar's own line."""
    t = fold(text)
    # no player's bar on the frame, no boss - whatever else the text says
    if not any(_close(t, fold(pb)) for pb in PLAYER_BARS):
        return ""
    for noise in PLAYER_BARS + ["STRESS", "CAMO"]:
        t = _strip(t, fold(noise))
    t = " ".join(t.split())
    if len(t) < 4:
        return ""
    best, score = "", 0.0
    for b in BOSSES:
        fb = fold(b)
        # the name may sit among other words: score the best window of the same length
        words = t.split()
        for i in range(len(words)):
            for j in range(i + 1, min(len(words), i + 3) + 1):
                cand = " ".join(words[i:j])
                r = difflib.SequenceMatcher(None, cand, fb).ratio()
                if r > score:
                    best, score = b, r
    # a short name needs a closer match than a long one: "VAMP" is four letters and the HUD is full of them
    return best.title() if score >= (0.85 if len(best) <= 6 else 0.72) else ""


def item_in(text):
    """The item named on an ITEM ACQUIRED card, or ''. The heading is drawn in a stencil face the engine reads as
    "X TEM RCCOUXRED", so it is matched loosely over the first few words; the item's name follows it."""
    words = text.split()
    target = fold("ITEM ACQUIRED")
    for i in range(min(len(words), 4)):
        for j in range(i + 1, min(len(words), i + 4) + 1):
            if difflib.SequenceMatcher(None, fold(" ".join(words[i:j])), target).ratio() >= 0.75:
                rest = " ".join(words[j:])
                rest = re.split(r"[.]|\s(?:A|An|The|This|It)\s", rest, maxsplit=1)[0].strip()
                return (rest[:48] or "yes")
    return ""


def ocr(images, top, bottom, left, right):
    """{path: text} for every image, one PowerShell process."""
    if not images:
        return {}
    lst = tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False, encoding="utf-8")
    lst.write("\n".join(images)); lst.close()
    out = lst.name + ".json"
    subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", OCR, "-List", lst.name,
                    "-Json", out, "-Top", str(top), "-Bottom", str(bottom), "-Left", str(left), "-Right", str(right)],
                   capture_output=True)
    try:
        data = json.load(open(out, encoding="utf-8-sig"))
    except Exception:
        data = {}
    for p in (lst.name, out):
        try:
            os.remove(p)
        except OSError:
            pass
    # ocr.ps1 keys its output by the image's name without the extension
    res = {}
    if isinstance(data, dict):
        for k, v in data.items():
            res[os.path.basename(k).lower()] = v if isinstance(v, str) else (v.get("text", "") if isinstance(v, dict) else "")
    return res


def main():
    argv = sys.argv[1:]
    only, force = None, "--force" in argv
    if "--only" in argv:
        only = set(argv[argv.index("--only") + 1].split(","))
    have = json.load(open(OUT, encoding="utf-8")) if os.path.exists(OUT) else {}
    stills = {}
    for n in sorted(os.listdir(SHOTS)):
        m = re.match(r"(.+)_([abz])\.jpg$", n)
        if not m:
            continue
        sid = m.group(1)
        if only and sid not in only:
            continue
        if sid in have and not force:
            continue
        stills.setdefault(sid, []).append(os.path.join(SHOTS, n))
    if not stills:
        print("nothing new to read (%d ids in %s)" % (len(have), OUT))
        return 0
    paths_all = [p for v in stills.values() for p in v]
    hud = ocr(paths_all, 0.0, 0.3, 0.0, 0.5)          # the bars, top-left
    card = ocr(paths_all, 0.1, 0.6, 0.2, 0.8)         # the item card, centre
    for sid, imgs in stills.items():
        boss, item, texts = "", "", []
        for p in imgs:
            k = os.path.splitext(os.path.basename(p))[0].lower()
            t = hud.get(k, "")
            texts.append(os.path.basename(p)[-5] + ":" + t)
            boss = boss or boss_in(t)
            c = card.get(k, "")
            if not item:
                item = item_in(c)
        # the verdicts only: the raw text once carried a desktop the recording's tail had drifted onto
        have[sid] = {"boss": boss, "item": item}
        if boss or item:
            print("%-14s %s%s" % (sid, ("boss " + boss) if boss else "", (" item " + item) if item else ""))
    io.open(OUT, "w", encoding="utf-8", newline="\n").write(json.dumps(have, indent=1, ensure_ascii=False, sort_keys=True) + "\n")
    print("%d ids read -> %s" % (len(stills), OUT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
