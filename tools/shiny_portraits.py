#!/usr/bin/env python3
"""Each portrait's shiny colors, for the Shiny Pokémon run option.

  shiny_portraits.py UPSTREAM SPRITECOLLAB OUT.h

UPSTREAM is pret's pmd-red clone (build/upstream, with baserom.gba);
SPRITECOLLAB a checkout of github.com/PMDCollab/SpriteCollab with
portrait/<dex>/*.png and portrait/<dex>/0000/0001/*.png (the normal and the
shiny portraits) for the species that have portraits in Red.

Red's portraits each have their own 16 colors (data/kao/*.inc: one palette
per expression). SpriteCollab keeps Red's portraits (under its own names:
its Special1 can be Red's Worried) with a shiny version of each drawn pixel
for pixel the same; so for each of Red's expressions the SpriteCollab
portrait drawn like it is found, and each color becomes the shiny one at the
same pixels. Colors that doesn't settle take the shiny one of the same color
in another of the Pokémon's portraits; the rest (backgrounds) stay as they
are. Red's own pictures are kept.
"""
import collections
import json
import os
import re
import sys

from PIL import Image

# Red's expressions, in the order of its portrait tables (SpriteCollab's names
# for them can differ: its "Special1" is sometimes Red's Worried, ...)
EMOTIONS = ["Normal", "Happy", "Pain", "Angry", "Worried", "Sad", "Crying", "Shouting", "Teary-Eyed",
            "Determined", "Joyous", "Inspired", "Surprised"]
SLOTS = 16
CLOSE = 8      # how near two colors are to count as the same (per channel)
MATCHED = 0.6  # the share of pixels a SpriteCollab portrait must have like Red's to be Red's


def decompress_at(src, off):
    """The game's DecompressAT (src/decompress_at.c): an AT4PX/AT3PX at off."""
    s = lambda i: src[off + i]
    clen = s(5) | (s(6) << 8)
    if src[off:off + 4] == b"AT4P":
        i = 0x12
    elif src[off:off + 4] == b"AT3P":
        i = 0x10
    else:
        return None
    flags = [s(7 + k) + 3 for k in range(9)]
    out = bytearray()
    bit, cur = 8, 0

    def put(a, b):
        out.append(((a & 15) << 4) | (b & 15))

    while i < clen:
        if bit == 8:
            cur, bit = s(i), 0
            i += 1
        if not cur & 0x80:
            cmd = (s(i) >> 4) + 3
            tmp = (s(i) & 15) << 8
            for k in range(9):
                if cmd == flags[k]:
                    cmd = 0x1F - k
            c = s(i) & 15
            pairs = {0x1F: ((c, c), (c, c)), 0x1E: ((c, c + 1), (c + 1, c + 1)), 0x1D: ((c, c - 1), (c, c)),
                     0x1C: ((c, c), (c - 1, c)), 0x1B: ((c, c), (c, c - 1)), 0x1A: ((c, c - 1), (c - 1, c - 1)),
                     0x19: ((c, c + 1), (c, c)), 0x18: ((c, c), (c + 1, c)), 0x17: ((c, c), (c, c + 1))}
            if cmd in pairs:
                put(*pairs[cmd][0])
                put(*pairs[cmd][1])
                i += 1
            else:
                tmp += s(i + 1) + len(out) - 0x1000
                i += 2
                for _ in range(cmd):
                    out.append(out[tmp])
                    tmp += 1
        else:
            out.append(s(i))
            i += 1
        bit += 1
        cur = (cur << 1) & 0xFF
    return bytes(out)


def pixels(gfx):
    """A 40x40 portrait's color indexes (5x5 tiles of 8x8, 4 bits a pixel), [y][x]."""
    px = [[0] * 40 for _ in range(40)]
    for t in range(25):
        tx, ty = t % 5, t // 5
        for y in range(8):
            for x in range(8):
                b = gfx[t * 32 + y * 4 + x // 2]
                px[ty * 8 + y][tx * 8 + x] = (b >> 4) if x & 1 else b & 15
    return px


def kao_tables(up):
    """Red species ID -> [(rom offset of the palette, of the picture) or None per slot]."""
    labels = {}  # label -> rom offset of its data (an AT4PX picture's "AT4PX" header included)
    tables = {}  # gKao symbol -> [(pal label, gfx label) per slot]
    kdir = os.path.join(up, "data/kao")
    for f in sorted(os.listdir(kdir)):
        lines = open(os.path.join(kdir, f), encoding="utf-8").read().splitlines()
        sym = None
        for i, line in enumerate(lines):
            m = re.match(r"(\w+):\s*$", line)
            if m:
                header = '.string "AT' in (lines[i + 1] if i + 1 < len(lines) else "")
                for nxt in lines[i + 1:i + 3]:
                    n = re.match(r'\s*\.incbin "baserom.gba", (0x[0-9A-Fa-f]+), (0x[0-9A-Fa-f]+)', nxt)
                    if n:
                        labels[m.group(1)] = int(n.group(1), 16) - (5 if header else 0)
                        break
            m = re.match(r"\.global (gKao\w+)", line)
            if m:
                sym = m.group(1)
            m = re.match(r"\s*kao_portrait (\w+), (\w+)", line)
            if m and sym:
                tables.setdefault(sym, []).append((m.group(1), m.group(2)))
    files = open(os.path.join(up, "src/monster_files_table.c"), encoding="utf-8").read()
    out = {}
    for sid, sym in re.findall(r'\{ "kao(\d+)", (gKao\w+),\}', files):
        if sym in tables:
            out[int(sid)] = [(labels.get(p), labels.get(g)) if p != "0" and g != "0" else None
                             for p, g in tables[sym][:SLOTS]]
    return out


def close(a, b):
    return max(abs(a[0] - b[0]), abs(a[1] - b[1]), abs(a[2] - b[2])) <= CLOSE


def load(path):
    im = Image.open(path).convert("RGBA")
    return im if im.size == (40, 40) else None


def shiny_palette(pal, px, pairs):
    """Red's 16 colors (pal, its picture px) made shiny from SpriteCollab's
    portraits: pairs = [(normal, shiny)] (40x40 RGBA). The pair drawn like Red's
    picture gives each color the shiny one at the same pixels; colors it
    doesn't settle take the shiny one of the same color in any pair; the
    others (backgrounds) stay."""
    def matches(n):
        return sum(1 for y in range(40) for x in range(40)
                   if n.getpixel((x, y))[3] and close(n.getpixel((x, y))[:3], pal[px[y][x]]))

    scored = sorted(((matches(n), k) for k, (n, s) in enumerate(pairs)), reverse=True)
    votes = [collections.Counter() for _ in range(16)]
    if scored and scored[0][0] >= MATCHED * 1600:
        n, s = pairs[scored[0][1]]
        for y in range(40):
            for x in range(40):
                p, q = n.getpixel((x, y)), s.getpixel((x, y))
                v = px[y][x]
                if p[3] and q[3] and close(p[:3], pal[v]):
                    votes[v][q[:3]] += 1
                elif p[3] and q[3] and close(p[:3], q[:3]):
                    votes[v][pal[v]] += 1  # (drawn a bit differently there, but not changed in the shiny one)
    colors = collections.defaultdict(collections.Counter)  # normal color -> shiny ones, every pair
    for n, s in pairs:
        for p, q in zip(n.get_flattened_data(), s.get_flattened_data()):
            if p[3] and q[3]:
                colors[p[:3]][q[:3]] += 1
    new = []
    for v, c in enumerate(pal):
        if votes[v]:
            # (a color Red uses for two things, one shiny and one not, as a
            # background and a body both: the one most of its pixels are,
            # but it stays unless the shiny one is at least 2 in 3)
            same = sum(k for col, k in votes[v].items() if close(col, c))
            moved = collections.Counter({col: k for col, k in votes[v].items() if not close(col, c)})
            q = c
            if moved and moved.most_common(1)[0][1] * 3 >= 2 * (same + sum(moved.values())):
                q = moved.most_common(1)[0][0]
        else:
            near = [k for k in colors if close(k, c)]
            if near:
                k = min(near, key=lambda k: sum((k[i] - c[i]) ** 2 for i in range(3)))
                q = colors[k].most_common(1)[0][0]
                q = tuple(max(0, min(255, c[i] + q[i] - k[i])) for i in range(3))
            else:
                q = c
        new.append(tuple(q))
    return new


def main():
    up, sc, out = sys.argv[1:4]
    rom = open(os.path.join(up, "baserom.gba"), "rb").read()
    mons = json.load(open(os.path.join(up, "data/monster/monster_data.json"), encoding="utf-8"))
    rows = {}
    for sid, slots in sorted(kao_tables(up).items()):
        dex = mons[sid]["dexInternal"][0]
        base = os.path.join(sc, f"portrait/{dex:04d}")
        pairs = []
        if os.path.isdir(base):
            for f in sorted(os.listdir(base)):
                n, s = os.path.join(base, f), os.path.join(base, "0000/0001", f)
                if f.endswith(".png") and os.path.exists(s):
                    a, b = load(n), load(s)
                    if a and b:
                        pairs.append((a, b))
        if not pairs:
            continue
        pals = []
        changed = False
        for slot in slots:
            if slot is None or None in slot:
                pals.append(None)
                continue
            po, go = slot
            pal = [tuple(rom[po + i * 4:po + i * 4 + 3]) for i in range(16)]
            new = shiny_palette(pal, pixels(decompress_at(rom, go)), pairs)
            changed |= new != pal
            pals.append(new)
        if changed:
            rows[sid] = pals
    with open(out, "w", encoding="utf-8") as f:
        f.write("// 3DS port, run option Shiny Pokémon: the shiny colors of each portrait (one\n"
                "// palette per expression slot, r g b), made by tools/shiny_portraits.py from\n"
                "// Red's portraits and PMDCollab SpriteCollab's normal and shiny ones.\n")
        f.write(f"#define PORT_SHINY_FACE_SLOTS {SLOTS}\n")
        f.write(f"static const u8 sPortShinyFaces[{len(rows)}][{SLOTS}][16][3] = {{\n")
        for sid, pals in rows.items():
            f.write(f"    {{ // species {sid}\n")
            for p in pals:
                f.write("        { " + (", ".join("{%d,%d,%d}" % c for c in p) if p else "{0}") + " },\n")
            f.write("    },\n")
        f.write("};\n")
        f.write("static const struct { u16 species; u16 row; } sPortShinyFaceSpecies[] = {\n")
        for i, sid in enumerate(rows):
            f.write(f"    {{ {sid}, {i} }},\n")
        f.write("};\n")
    print(f"{out}: {len(rows)} species with shiny portraits")


if __name__ == "__main__":
    main()
