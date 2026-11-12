#!/usr/bin/env python3
"""Each portrait's shiny colors, for the Shiny Pokémon run option.

  shiny_portraits.py UPSTREAM SPRITECOLLAB OUT.h

UPSTREAM is pret's pmd-red clone (build/upstream, with baserom.gba);
SPRITECOLLAB a checkout of github.com/PMDCollab/SpriteCollab with
portrait/<dex>/*.png and portrait/<dex>/0000/0001/*.png (the normal and the
shiny portraits) for the species that have portraits in Red.

Red's portraits each have their own 16 colors (data/kao/*.inc: one palette
per expression). SpriteCollab's normal and shiny portraits are drawn pixel for
pixel the same, so they tell what each color becomes; each of Red's colors is
matched to the nearest of the normal portrait's and changed the same way (as
tools/shiny_palettes.py does for the sprites). Expressions SpriteCollab has no
shiny one of use the Normal one's colors. Red's own pictures are kept.
"""
import collections
import json
import os
import re
import sys

from PIL import Image

# Red's expressions, in the order of its portrait tables, and SpriteCollab's names
EMOTIONS = ["Normal", "Happy", "Pain", "Angry", "Worried", "Sad", "Crying", "Shouting", "Teary-Eyed",
            "Determined", "Joyous", "Inspired", "Surprised"]
SLOTS = 16


def kao_tables(up):
    """Red species ID -> [(rom offset of the palette or None) per slot]."""
    labels = {}  # label -> rom offset of the .incbin after it
    tables = {}  # gKao symbol -> [pal label per slot]
    kdir = os.path.join(up, "data/kao")
    for f in sorted(os.listdir(kdir)):
        lines = open(os.path.join(kdir, f), encoding="utf-8").read().splitlines()
        sym = None
        for i, line in enumerate(lines):
            m = re.match(r"(\w+):\s*$", line)
            if m:
                for nxt in lines[i + 1:i + 3]:
                    n = re.match(r'\s*\.incbin "baserom.gba", (0x[0-9A-Fa-f]+), (0x[0-9A-Fa-f]+)', nxt)
                    if n:
                        labels[m.group(1)] = int(n.group(1), 16)
                        break
            m = re.match(r"\.global (gKao\w+)", line)
            if m:
                sym = m.group(1)
            m = re.match(r"\s*kao_portrait (\w+), (\w+)", line)
            if m and sym:
                tables.setdefault(sym, []).append(None if m.group(1) == "0" else m.group(1))
    files = open(os.path.join(up, "src/monster_files_table.c"), encoding="utf-8").read()
    out = {}
    for sid, sym in re.findall(r'\{ "kao(\d+)", (gKao\w+),\}', files):
        if sym in tables:
            out[int(sid)] = [labels.get(lbl) if lbl else None for lbl in tables[sym][:SLOTS]]
    return out


def color_map(normal, shiny):
    a = Image.open(normal).convert("RGBA")
    b = Image.open(shiny).convert("RGBA")
    if a.size != b.size:
        return None
    pairs = collections.defaultdict(collections.Counter)
    for p, q in zip(a.getdata(), b.getdata()):
        if p[3] > 0 and q[3] > 0:
            pairs[p[:3]][q[:3]] += 1
    return {n: c.most_common(1)[0][0] for n, c in pairs.items()} or None


def dist(a, b):
    return 2 * (a[0] - b[0]) ** 2 + 4 * (a[1] - b[1]) ** 2 + 3 * (a[2] - b[2]) ** 2


def main():
    up, sc, out = sys.argv[1:4]
    rom = open(os.path.join(up, "baserom.gba"), "rb").read()
    mons = json.load(open(os.path.join(up, "data/monster/monster_data.json"), encoding="utf-8"))
    rows = {}
    for sid, offs in sorted(kao_tables(up).items()):
        dex = mons[sid]["dexInternal"][0]
        base = os.path.join(sc, f"portrait/{dex:04d}")
        maps = {}
        for e in EMOTIONS:
            n, s = os.path.join(base, e + ".png"), os.path.join(base, "0000/0001", e + ".png")
            if os.path.exists(n) and os.path.exists(s):
                maps[e] = color_map(n, s)
        if not maps.get("Normal"):
            continue
        pals = []
        changed = False
        for slot, off in enumerate(offs):
            if off is None:
                pals.append(None)
                continue
            pal = [tuple(rom[off + i * 4:off + i * 4 + 3]) for i in range(16)]
            cmap = maps.get(EMOTIONS[slot] if slot < len(EMOTIONS) else "Normal") or maps["Normal"]
            new = []
            for c in pal:
                k = min(cmap, key=lambda k: dist(k, c))
                s = cmap[k]
                s = tuple(max(0, min(255, c[i] + (s[i] - k[i]))) for i in range(3))
                changed |= s != c
                new.append(s)
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
