#!/usr/bin/env python3
"""Each species' shiny colors, for the Shiny Pokémon run option.

  shiny_palettes.py UPSTREAM SPRITECOLLAB OUT.h

UPSTREAM is pret's pmd-red clone (build/upstream); SPRITECOLLAB a checkout of
github.com/PMDCollab/SpriteCollab with at least sprite/*/Walk-Anim.png and
sprite/*/0000/0001/Walk-Anim.png (the normal and the shiny walk sheets), and
the Idle-Anim.png ones where they're there (they're added in).

Red's species share 13 palettes of 16 colors (graphics/ax/pal). For each
species, the colors its own sprites use are matched to the nearest colors of
SpriteCollab's normal sheet; the shiny sheet, drawn pixel for pixel the same,
tells what each of those becomes. The result is a palette of 16 colors per
species: its shared palette with the colors it uses replaced (the others
unchanged). Species without a shiny sheet get none.
"""
import collections
import json
import os
import re
import sys

from PIL import Image


def species_folders(up):
    """Species ID -> graphics/ax/mon folder, through monster_files_table.c."""
    table = open(os.path.join(up, "src/monster_files_table.c"), encoding="utf-8").read()
    consts = {}
    for line in open(os.path.join(up, "include/constants/monster.h"), encoding="utf-8"):
        m = re.match(r"#define\s+(MONSTER_\w+)\s+(\d+)\s*$", line)
        if m:
            consts[m.group(1)] = int(m.group(2))
    sym_dir = {}
    axdir = os.path.join(up, "src/data/ax")
    for f in os.listdir(axdir):
        text = open(os.path.join(axdir, f), encoding="utf-8").read()
        m = re.search(r"const SiroArchive (gAx\w+) =", text)
        g = re.search(r"graphics/ax/mon/([\w-]+)/", text)
        if m and g:
            sym_dir[m.group(1)] = g.group(1)
    out = {}
    for m in re.finditer(r"\[(MONSTER_\w+) - 1\] = \{ \"ax\d+\", &(gAx\w+),\}", table):
        if m.group(2) in sym_dir:
            out[consts[m.group(1)]] = sym_dir[m.group(2)]
    return out


def read_pal(path):
    d = open(path, "rb").read()
    return [tuple(d[i * 4:i * 4 + 3]) for i in range(16)]


def used_indices(folder):
    count = collections.Counter()
    for f in os.listdir(folder):
        if f.endswith(".png"):
            im = Image.open(os.path.join(folder, f))
            if im.mode != "P":
                continue
            count.update(im.getdata())
    count.pop(0, None)  # (transparent)
    return count


SHEETS = ("Walk-Anim.png", "Idle-Anim.png")  # (some walks show only part: Diglett's is all dirt)


def color_map(sheets):
    """Normal RGB -> the shiny RGB it most often becomes, with its pixel count,
    over (normal, shiny) sheet pairs."""
    pairs = collections.defaultdict(collections.Counter)
    for normal, shiny in sheets:
        a = Image.open(normal).convert("RGBA")
        b = Image.open(shiny).convert("RGBA")
        if a.size != b.size:
            continue
        for p, q in zip(a.getdata(), b.getdata()):
            if p[3] > 0 and q[3] > 0:
                pairs[p[:3]][q[:3]] += 1
    if not pairs:
        return None
    return {n: (c.most_common(1)[0][0], sum(c.values())) for n, c in pairs.items()}


def dist(a, b):
    # (weighted toward green, as the eye is)
    return 2 * (a[0] - b[0]) ** 2 + 4 * (a[1] - b[1]) ** 2 + 3 * (a[2] - b[2]) ** 2


def main():
    up, sc, out = sys.argv[1:4]
    mons = json.load(open(os.path.join(up, "data/monster/monster_data.json"), encoding="utf-8"))
    folders = species_folders(up)
    pals = [read_pal(os.path.join(up, f"graphics/ax/pal/{i}.pmdpal")) for i in range(14)]
    rows = []
    made = 0
    for sid, e in enumerate(mons):
        dex = e["dexInternal"][0]
        sheets = [(os.path.join(sc, f"sprite/{dex:04d}/{n}"), os.path.join(sc, f"sprite/{dex:04d}/0000/0001/{n}"))
                  for n in SHEETS]
        sheets = [(a, b) for a, b in sheets if os.path.exists(a) and os.path.exists(b)]
        pal = pals[e["overworldPalette"]]
        new = None
        if sid in folders and dex > 0 and sheets:
            cmap = color_map(sheets)
            used = used_indices(os.path.join(up, "graphics/ax/mon", folders[sid]))
            if cmap and used:
                new = list(pal)
                changed = False
                for idx in used:
                    c = pal[idx]
                    n = min(cmap, key=lambda k: dist(k, c))
                    s = cmap[n][0]
                    # The same change, from the Red color: keeps Red's shading
                    s = tuple(max(0, min(255, c[i] + (s[i] - n[i]))) for i in range(3))
                    if s != c:
                        changed = True
                    new[idx] = s
                if not changed:
                    new = None
        if new:
            made += 1
        rows.append(new)
    with open(out, "w", encoding="utf-8") as f:
        f.write("// 3DS port, run option Shiny Pokémon: each species' shiny palette (16\n"
                "// colors, r g b), made by tools/shiny_palettes.py from Red's palettes and\n"
                "// sprites and PMDCollab SpriteCollab's normal and shiny sheets. All zero:\n"
                "// none (the species keeps its colors).\n")
        f.write(f"static const u8 sPortShinyPalettes[{len(rows)}][16][3] = {{\n")
        for sid, new in enumerate(rows):
            if new is None:
                continue
            cols = ", ".join("{%d,%d,%d}" % c for c in new)
            f.write(f"    [{sid}] = {{ {cols} }},\n")
        f.write("};\n")
    print(f"{out}: {made} of {len(rows)} species have a shiny palette")


if __name__ == "__main__":
    main()
