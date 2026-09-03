"""The location banner of every stage, read out of the game's own data - no OCR, no play-through.

When a stage starts the game draws "EASTERN EUROPE / MIDTOWN CENTRAL SECTOR" (region / area). This prints that
pair for every stage in tools\\scenes.csv, straight from the stage scripts inside the pak files.

  python tools\\stage_names.py                       stage<TAB>region<TAB>area for every stage in scenes.csv
  python tools\\stage_names.py --json out.json       also write {stage: {region, area, act, region_id, area_id}}
  python tools\\stage_names.py --lang fr             the French banner (en de es fr it ja pt)
  python tools\\stage_names.py --locations loc.json  dump every region's ordered area table (id -> name)
  python tools\\stage_names.py --verbose             show every area id a stage refers to and why it was (not) picked

Where the names live, and how a stage gets to them (everything below was measured on the Steam build):

* `common\\localization\\lang\\lang_<xx>` is a flat string table: u16 version, u32 count, then records of three u64
  ids, a u32 length and the NUL-terminated UTF-8 string. The ids are 24-bit StrCode hashes (the MGS hash: rotate
  left 5, add the byte, mask 24 bits). For the banner strings the middle id is always 0x5a6764 - the hash-name of
  the layout file `cache/005a6764.la2` that draws the banner - the first id names the *region table* (one per act:
  0xba3d3a MIDDLE EAST, 0x642614 SOUTH AMERICA, 0xb715c9 EASTERN EUROPE, 0xaba6e6 SHADOW MOSES, 0xcf5d51 OUTER
  HAVEN) and the third id is the *area* within it. Area ids are shared between acts (0xb1a458 is the first area
  of every act: RED ZONE NW SECTOR, COVE VALLEY VILLAGE, MIDTOWN S SECTOR, SNOWFIELD, SHIP BOW); the region id
  picks which. 0x493612 under each region is the region's own name.
* `ww\\stage\\stage_data_compressed.*.pak` is a VPAK: 16-byte header (`VPAK`, u16 3, u16 1, u32 entry count,
  u32 table size), LZ4 block data, and the file table in the last `table size` bytes. Each table record is
  u32 name length, u16 flags (0x100 = LZ4, 0 = stored), the path, then u64 zero, u64 size, u64 compressed size,
  u64 offset, u32 chunk size (1 MiB), u32 chunk count, u64 zero, and (chunk count - 1) u64 compressed offsets of
  the chunk boundaries. Each chunk is one raw LZ4 block.
* Every stage has one script, `ww/stage/stageNN/<stage>/cache/00180720.gcx` (GCX = compiled GCL, the MGS
  scripting language; format after Jayveer/Gcx). It sets the region with `(... -set [region] ...)` and the area
  with `(... -sub [area])`, either directly or through a small wrapper proc called as `@procN [area]`. The
  scripts also hold the same strings as resources, so the names could be read without the lang file.
* A stage that spans several areas refers to all of them. Area-change *traps* (the zone triggers that swap the
  banner as you walk) call a wrapper that also plays the transition sound (it references object 0x690610);
  those are ignored, and the lowest remaining id is the stage's start - the ids are numbered in play order
  (0xb1a458, 0xb1a459, ... then +0x8000 for the next block). Stages with no gameplay (cutscene-only) usually set
  no area; a few reference one (GROUND ZERO) and that is reported.

Stages that have no script in the paks (s03a80l, s04a05l, s99a99l) print empty fields.
"""
import argparse
import json
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import paths  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
SCENES_CSV = os.path.join(HERE, "scenes.csv")

BANNER_LAYOUT = 0x5a6764            # cache/005a6764.la2 - the middle id of every banner string
REGION_NAME = 0x493612              # the area slot that holds the region's own name
REGIONS = {                         # region table id -> act number (act names are not in the lang table)
    0xba3d3a: 1, 0x642614: 2, 0xb715c9: 3, 0xaba6e6: 4, 0xcf5d51: 5,
}
ACT_NAMES = {1: "Liquid Sun", 2: "Solid Sun", 3: "Third Sun", 4: "Twin Suns", 5: "Old Sun"}
TRANSITION_OBJ = 0x690610           # referenced by the area-change wrapper that plays the transition sound
STAGE_PAKS = [r"ww\stage\stage_data_compressed.1.pak", r"ww\stage\stage_data_compressed.2.pak"]


def strcode(s):
    """The MGS 24-bit string hash."""
    h = 0
    for c in s.encode("utf-8"):
        h = (((h >> 19) | (h << 5)) + c) & 0xffffff
    return h or 1


PARAM_SET = strcode("set")
PARAM_SUB = strcode("sub")


# ---------------------------------------------------------------- lang_<xx>
def read_lang(game_dir, lang="en"):
    """{(table_id, area_id): string} for every banner string in lang_<lang>."""
    path = os.path.join(game_dir, "common", "localization", "lang", "lang_" + lang)
    data = open(path, "rb").read()
    _ver, count = struct.unpack_from("<HI", data, 0)
    off = 6
    out = {}
    for _ in range(count):
        a, b, c, n = struct.unpack_from("<QQQI", data, off)
        off += 28
        s = data[off:off + n]
        off += n
        if b == BANNER_LAYOUT:
            out[(a, c)] = s.rstrip(b"\0").decode("utf-8", "replace").strip()
    return out


# ---------------------------------------------------------------- VPAK
def vpak_table(path):
    """[(name, flags, size, csize, offset, chunk, chunk_bounds)] for a VPAK."""
    size = os.path.getsize(path)
    with open(path, "rb") as f:
        magic, _v1, _v2, count, tsize = struct.unpack("<4sHHII", f.read(16))
        if magic != b"VPAK":
            raise ValueError("%s is not a VPAK" % path)
        f.seek(size - tsize)
        t = f.read()
    off = 0
    entries = []
    for _ in range(count):
        n, flags = struct.unpack_from("<IH", t, off)
        name = t[off + 6:off + 6 + n].decode("latin1")
        off += 6 + n
        _z, usize, csize, offset, chunk, nchunks, _z2 = struct.unpack_from("<QQQQIIQ", t, off)
        off += 48
        bounds = struct.unpack_from("<%dQ" % (nchunks - 1), t, off)
        off += 8 * (nchunks - 1)
        entries.append((name, flags, usize, csize, offset, chunk, list(bounds)))
    return entries


def lz4_block(src):
    """Decode one raw LZ4 block."""
    dst = bytearray()
    i, n = 0, len(src)
    while i < n:
        tok = src[i]
        i += 1
        ll = tok >> 4
        if ll == 15:
            while True:
                b = src[i]
                i += 1
                ll += b
                if b != 255:
                    break
        dst += src[i:i + ll]
        i += ll
        if i >= n:
            break
        off = src[i] | (src[i + 1] << 8)
        i += 2
        ml = tok & 15
        if ml == 15:
            while True:
                b = src[i]
                i += 1
                ml += b
                if b != 255:
                    break
        ml += 4
        start = len(dst) - off
        if off >= ml:
            dst += dst[start:start + ml]
        else:
            for k in range(ml):
                dst.append(dst[start + k])
    return bytes(dst)


def vpak_read(path, entry):
    name, flags, usize, csize, offset, chunk, bounds = entry
    with open(path, "rb") as f:
        f.seek(offset)
        raw = f.read(csize)
    if not flags & 0x100:
        return raw[:usize]
    edges = [0] + bounds + [csize]
    out = b"".join(lz4_block(raw[edges[k]:edges[k + 1]]) for k in range(len(edges) - 1))
    return out[:usize]


def stage_scripts(game_dir):
    """{stage: (pak path, entry)} for every stage script in the stage paks."""
    found = {}
    for rel in STAGE_PAKS:
        pak = os.path.join(game_dir, rel)
        if not os.path.exists(pak):
            continue
        for e in vpak_table(pak):
            m = re.match(r"ww/stage/stage\d\d/([^/]+)/cache/00180720\.gcx$", e[0])
            if m:
                found[m.group(1)] = (pak, e)
    return found


# ---------------------------------------------------------------- GCX
class Gcx:
    """Just enough of a GCX to find its procs: timestamp, proc offset table (-1 terminated), block header."""

    def __init__(self, data):
        self.data = data
        i = 4
        procs = []
        while True:
            v, = struct.unpack_from("<i", data, i)
            i += 4
            if v == -1:
                break
            procs.append(v & 0xffffff)
        self.procs = procs
        proc_off, = struct.unpack_from("<I", data, i)
        self.proc_start = i + proc_off + 4

    def proc_body(self, idx):
        """The bytes of proc idx (an 0x8x tagged block)."""
        if not 0 <= idx < len(self.procs):
            return b""
        p = self.proc_start + self.procs[idx]
        d = self.data
        size = d[p] & 0x0f
        p += 1
        if size == 0x0d:
            size = d[p]
            p += 1
        elif size == 0x0e:
            size = struct.unpack_from("<H", d, p)[0]
            p += 2
        return d[p:p + size]


def u24(b):
    return b[0] | (b[1] << 8) | (b[2] << 16)


def analyse(script, lang):
    """(region_id, {area_id: how}) - how is 'set' (start-style), 'trap' (area-change trap) or 'ref' (mentioned)."""
    g = Gcx(script)
    d = script
    param_set = struct.pack("<I", PARAM_SET)[:3]
    param_sub = struct.pack("<I", PARAM_SUB)[:3]

    regions = [u24(d[m.end():m.end() + 3]) for m in re.finditer(re.escape(b"s" + param_set + b"\x06"), d)]
    regions = [r for r in regions if r in REGIONS]
    region = regions[-1] if regions else None
    if region is None:
        return None, {}
    table = {c for (a, c) in lang if a == region and c != REGION_NAME}

    refs = {}
    # (... -sub [area]) set directly
    for m in re.finditer(re.escape(b"s" + param_sub + b"\x06"), d):
        c = u24(d[m.end():m.end() + 3])
        if c in table:
            refs[c] = "set"
    # @procN [area] - a wrapper; the area-change trap wrapper plays the transition sound
    # (0x7d / 0x7e carry the block size in the next 1 / 2 bytes, then the i16 proc number, then the args)
    for m in re.finditer(rb"(?:[\x71-\x7c]|\x7d.|\x7e..)(..)\x06(...)", d, re.S):
        c = u24(m.group(2))
        if c not in table:
            continue
        n, = struct.unpack("<h", m.group(1))
        body = g.proc_body(n - 1)              # @procN is proc table entry N-1
        how = "trap" if struct.pack("<I", TRANSITION_OBJ)[:3] in body else "set"
        if refs.get(c) != "set":
            refs[c] = how
    # any other mention of a table id as a literal
    for c in table:
        if c not in refs and (b"\x06" + struct.pack("<I", c)[:3]) in d:
            refs[c] = "ref"
    return region, refs


def pick(refs):
    for how in ("set", "trap", "ref"):
        ids = [c for c, h in refs.items() if h == how]
        if ids:
            return min(ids)
    return None


def scene_stages():
    stems = []
    with open(SCENES_CSV, encoding="utf-8-sig") as f:
        next(f)
        for line in f:
            stem = line.split(",", 1)[0].strip().split("_", 1)[0]
            if stem and stem not in stems:
                stems.append(stem)
    return stems


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--json", metavar="FILE", help="write {stage: {region, area, act, ...}}")
    ap.add_argument("--lang", default="en", help="language table to read (default en)")
    ap.add_argument("--locations", metavar="FILE", help="write every region's area table")
    ap.add_argument("--all", action="store_true", help="every stage in the paks, not just scenes.csv")
    ap.add_argument("--verbose", "-v", action="store_true")
    args = ap.parse_args()

    game = paths.require_game()
    lang = read_lang(game, args.lang)
    scripts = stage_scripts(game)

    if args.locations:
        loc = {}
        for region, act in sorted(REGIONS.items(), key=lambda kv: kv[1]):
            areas = sorted(c for (a, c) in lang if a == region and c != REGION_NAME)
            loc["%06x" % region] = {
                "act": act, "act_name": ACT_NAMES[act], "region": lang.get((region, REGION_NAME), ""),
                "areas": [{"id": "%06x" % c, "name": lang[(region, c)]} for c in areas],
            }
        with open(args.locations, "w", encoding="utf-8") as f:
            json.dump(loc, f, indent=2, ensure_ascii=False)

    stages = sorted(scripts) if args.all else scene_stages()
    out = {}
    for stage in stages:
        rec = {"region": "", "area": "", "act": "", "region_id": "", "area_id": ""}
        if stage in scripts:
            pak, entry = scripts[stage]
            region, refs = analyse(vpak_read(pak, entry), lang)
            if region is not None:
                act = REGIONS[region]
                rec.update(region=lang.get((region, REGION_NAME), ""), act=ACT_NAMES[act],
                           region_id="%06x" % region)
                area = pick(refs)
                if area is not None:
                    rec.update(area=lang.get((region, area), ""), area_id="%06x" % area)
                if args.verbose:
                    rec["refs"] = {"%06x" % c: {"how": h, "name": lang.get((region, c), "")}
                                   for c, h in sorted(refs.items())}
        out[stage] = rec
        line = "%s\t%s\t%s" % (stage, rec["region"], rec["area"])
        if args.verbose and rec.get("refs"):
            line += "\t" + "; ".join("%s %s=%s" % (h["how"], c, h["name"]) for c, h in rec["refs"].items())
        print(line)

    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(out, f, indent=2, ensure_ascii=False)


if __name__ == "__main__":
    main()
