#!/usr/bin/env python3
"""The Pokémon data as an editable spreadsheet, and back.

  pokemon_sheet.py export UPSTREAM OUT.xlsx   # the game's data -> a sheet
  pokemon_sheet.py import UPSTREAM IN.xlsx    # a sheet -> the game's data

UPSTREAM is pret's pmd-red clone (build/upstream). The data it reads and
writes there:
  data/monster/monster_data.json           each species' stats and evolution
  data/monster/learnset/learnset_data.json each species' level-up and TM/HM moves
  data/monster/monster_names.s             the names and categories
The build turns the two JSON files into game data (data_monster.mk). The
import changes those files only (patches/pmd-red.patch then carries them).

The sheet's tabs: Pokémon (a row a species, by its ID), Level-Up Moves and
TM-HM Moves (a row a move), Lists (the codes the dropdowns offer) and Read Me.
A blank cell is a setting the game leaves at its default (as the JSON leaves
it out). Rows can be edited, not added yet: a new species needs more than this
data (its sprites, portraits, constants...), which a later step adds.
"""
import json
import os
import re
import sys

import openpyxl
from openpyxl.styles import Alignment, Font, PatternFill
from openpyxl.utils import get_column_letter
from openpyxl.worksheet.datavalidation import DataValidation

FONT = "Arial"

# (column title, how it's read from / written to a species' JSON entry)
# kinds: str int bool list0 list1 (a list's first/second item) pair0 pair1
# (a 2-number list's), evo (preEvolution's key), req (evolutionRequirements'
# key), name/category (the text of the label), dex
COLUMNS = [
    ("ID", "id", None, "The species' number in the game (not editable)."),
    ("Constant", "constant", None, "The species' code in the game's source (not editable)."),
    ("Name", "name", "name", "Its name, as the game shows it (10 letters at most)."),
    ("Category", "category", "category", "\"___ Pokémon\": the word or words before Pokémon."),
    ("Type 1", "list0", "types", "First type."),
    ("Type 2", "list1", "types", "Second type (blank: none)."),
    ("Ability 1", "list0", "abilities", "First ability."),
    ("Ability 2", "list1", "abilities", "Second ability (blank: none)."),
    ("Base HP", "int", "baseHP", "HP at level 1 (before level-up gains)."),
    ("Attack", "pair0", "baseAtkSpAtk", "Base Attack."),
    ("Sp. Atk", "pair1", "baseAtkSpAtk", "Base Sp. Atk."),
    ("Defense", "pair0", "baseDefSpDef", "Base Defense."),
    ("Sp. Def", "pair1", "baseDefSpDef", "Base Sp. Def."),
    ("EXP Yield", "int", "expYield", "Experience for defeating it."),
    ("Recruit Rate", "int", "recruitRate", "Recruitment chance (tenths of a percent; -999: never recruitable)."),
    ("Friend Area", "str", "friendArea", "Where it lives once recruited."),
    ("Evolves From", "evo", "evolveFrom", "The species it evolves from (blank: doesn't evolve from one)."),
    ("Evolve Method", "evo", "evolveType", "Level, IQ, Item, or Link Cable."),
    ("Evolve Requirement", "req", "mainRequirement",
     "Level: the level. IQ: the IQ. Item: the item's code (ITEM_...). Link Cable: 0."),
    ("Extra Requirement", "req", "additionalRequirement",
     "4 also needs a Link Cable; 5 Attack > Defense; 6 Attack < Defense; 7 Attack = Defense; "
     "8 Sun Ribbon; 9 Lunar Ribbon; 10 Beauty Scarf; 11 / 12 Wurmple's two branches."),
    ("Movement Type", "str", "movementType", "Terrain it can cross: blank normal, or water, lava, chasm, wall."),
    ("Body Size", "int", "bodySize", "Team slots it takes (1-4)."),
    ("Movement Speed", "int", "movementSpeed", "Turn speed: 0 slow, 1 normal, 2 fast..."),
    ("Shadow Size", "int", "shadowSize", "0 small, 1 medium, 2 large."),
    ("Chance Asleep", "int", "chanceAsleep", "Percent chance it's asleep when it appears."),
    ("Can Move", "bool", "canMove", "FALSE: stays put unless it's the leader (Kakuna, Metapod...)."),
    ("Weight", "int", "weight", "Weight (in-game units)."),
    ("Size", "int", "size", "Height (in-game units)."),
    ("Dex No", "pair0", "dexInternal", "National Pokédex number."),
    ("Internal No", "pair1", "dexInternal", "Internal number."),
    ("Alphabet No", "pair0", "alphabetParent", "Position when sorted by name."),
    ("Parent No", "pair1", "alphabetParent", "Its base species' ID (for forms)."),
    ("Overworld Palette", "int", "overworldPalette", "Which palette its sprite uses."),
    ("Portraits", "str", "dialogueSprites",
     "Which portrait emotions it has, as bits (0b...; the rightmost is Normal). Blank: none."),
    ("Regen Speed", "int", "regenSpeed", "HP regeneration (100 for all)."),
    ("Can Throw Items", "bool", "canThrowItems", "TRUE for all."),
    ("Skip Friend Area Check", "bool", "unk12", "Rarely set; leave as is."),
    ("Unknown 30", "int", "unk30", "10 for all; leave as is."),
    ("Unknown 31", "int", "unk31", "10 for all; leave as is."),
    ("Unknown 32", "int", "unk32", "24 for all; leave as is."),
]

# Columns validated against a list on the Lists tab
DROPDOWNS = {
    "Type 1": "Types", "Type 2": "Types", "Ability 1": "Abilities", "Ability 2": "Abilities",
    "Friend Area": "Friend Areas", "Evolves From": "Species", "Evolve Method": "Evolve Methods",
    "Movement Type": "Movement Types", "Can Move": "Booleans", "Can Throw Items": "Booleans",
    "Skip Friend Area Check": "Booleans",
}


def defines(path, prefix, skip=()):
    out = []
    for line in open(path, encoding="utf-8"):
        m = re.match(r"\s*#define\s+(" + prefix + r"\w+)\s+(\S+)", line)
        if m and m.group(1) not in skip:
            out.append(m.group(1))
    return out


def species_constants(up):
    names = {}
    for line in open(os.path.join(up, "include/constants/monster.h"), encoding="utf-8"):
        m = re.match(r"#define\s+(MONSTER_\w+)\s+(\d+)\s*$", line)
        if m and int(m.group(2)) not in names:
            names[int(m.group(2))] = m.group(1)
    return names


def read_names(up):
    """monster_names.s: label -> text, and the labels in order."""
    text = open(os.path.join(up, "data/monster/monster_names.s"), encoding="utf-8").read()
    labels = {}
    # .string "Text\0" or .asciz "Text"
    for m in re.finditer(r"^(\w+):\n\.(?:string|asciz) \"((?:[^\"\\]|\\.)*?)(?:\\0)?\"", text, re.M):
        labels[m.group(1)] = m.group(2)
    return text, labels


def paths(up):
    return (os.path.join(up, "data/monster/monster_data.json"),
            os.path.join(up, "data/monster/learnset/learnset_data.json"))


def lists(up):
    inc = os.path.join(up, "include/constants")
    sp = species_constants(up)
    return {
        "Types": defines(os.path.join(inc, "type.h"), "TYPE_"),
        "Abilities": defines(os.path.join(inc, "ability.h"), "ABILITY_"),
        "Friend Areas": defines(os.path.join(inc, "friend_area.h"), "FRIEND_AREA_"),
        "Species": [sp[i] for i in sorted(sp)],
        "Evolve Methods": defines(os.path.join(inc, "evolve_type.h"), "EVOLVE_TYPE_"),
        "Movement Types": defines(os.path.join(inc, "walkable_tile.h"), "MOVEMENT_TYPE_"),
        "Moves": defines(os.path.join(inc, "move_id.h"), "MOVE_"),
        "Items": defines(os.path.join(inc, "item.h"), "ITEM_"),
        "Booleans": ["TRUE", "FALSE"],
    }


def style_header(ws, widths):
    for i, w in enumerate(widths, 1):
        c = ws.cell(row=1, column=i)
        c.font = Font(name=FONT, bold=True, color="FFFFFF")
        c.fill = PatternFill("solid", fgColor="305496")
        c.alignment = Alignment(wrap_text=True, vertical="center")
        ws.column_dimensions[get_column_letter(i)].width = w
    ws.freeze_panes = "C2" if ws.title == "Pokémon" else "B2"
    ws.row_dimensions[1].height = 30


def export(up, out):
    mpath, lpath = paths(up)
    mons = json.load(open(mpath, encoding="utf-8"))
    learn = json.load(open(lpath, encoding="utf-8"))
    sp = species_constants(up)
    _, labels = read_names(up)
    lst = lists(up)

    wb = openpyxl.Workbook()
    rd = wb.active
    rd.title = "Read Me"
    readme = [
        ("Pokémon data", True),
        ("Every species' data in Pokémon Mystery Dungeon: Red Rescue Team, as the 3DS port builds it.", False),
        ("Edit any white cell; gray columns (ID, Constant) identify the row and can't be changed.", False),
        ("A blank cell leaves that setting at the game's default.", False),
        ("Codes (TYPE_..., ABILITY_..., MOVE_...) must be ones from the Lists tab; most columns offer them as a dropdown.", False),
        ("Level-Up Moves and TM-HM Moves: one row per move. Add or delete rows freely; keep each Pokémon's", False),
        ("level-up rows in level order. The Pokémon column must match a Constant on the Pokémon tab.", False),
        ("Adding brand-new species (new rows on the Pokémon tab) isn't supported yet: those also need sprites,", False),
        ("portraits and more, which come in a later step.", False),
        ("", False),
        ("Columns (Pokémon tab)", True),
    ] + [(f"{t}: {doc}", False) for t, _, _, doc in COLUMNS]
    for r, (t, bold) in enumerate(readme, 1):
        c = rd.cell(row=r, column=1, value=t)
        c.font = Font(name=FONT, bold=bold, size=13 if r == 1 else 10)
    rd.column_dimensions["A"].width = 120

    # Lists tab (dropdown sources)
    li = wb.create_sheet("Lists")
    ranges = {}
    for col, (title, values) in enumerate(lst.items(), 1):
        li.cell(row=1, column=col, value=title)
        for r, v in enumerate(values, 2):
            li.cell(row=r, column=col, value=v).font = Font(name=FONT)
        L = get_column_letter(col)
        ranges[title] = f"Lists!${L}$2:${L}${len(values) + 1}"
    style_header(li, [28] * len(lst))
    li.freeze_panes = "A2"

    # Pokémon tab
    ws = wb.create_sheet("Pokémon", 1)
    for c, (t, *_r) in enumerate(COLUMNS, 1):
        ws.cell(row=1, column=c, value=t)
    gray = PatternFill("solid", fgColor="D9D9D9")
    for i, e in enumerate(mons):
        row = i + 2
        for c, (title, kind, key, _doc) in enumerate(COLUMNS, 1):
            v = None
            if kind == "id":
                v = i
            elif kind == "constant":
                v = sp.get(i, "")
            elif kind in ("name", "category"):
                v = labels.get(e[key], e[key])
            elif kind == "str":
                v = e.get(key)
            elif kind == "int":
                v = e.get(key)
            elif kind == "bool":
                v = None if key not in e else ("TRUE" if e[key] else "FALSE")
            elif kind in ("list0", "list1"):
                lv = e.get(key, [])
                n = 0 if kind == "list0" else 1
                v = lv[n] if len(lv) > n else None
            elif kind in ("pair0", "pair1"):
                v = e[key][0 if kind == "pair0" else 1]
            elif kind == "evo":
                v = e.get("preEvolution", {}).get(key)
            elif kind == "req":
                v = e.get("evolutionRequirements", {}).get(key)
            cell = ws.cell(row=row, column=c, value=v)
            cell.font = Font(name=FONT)
            if kind in ("id", "constant"):
                cell.fill = gray
    style_header(ws, [5, 26, 12, 12] + [16] * (len(COLUMNS) - 4))
    n = len(mons) + 1
    for title, lname in DROPDOWNS.items():
        col = get_column_letter([t for t, *_ in COLUMNS].index(title) + 1)
        dv = DataValidation(type="list", formula1=ranges[lname], allow_blank=True)
        dv.add(f"{col}2:{col}{n}")
        ws.add_data_validation(dv)
    ws.auto_filter.ref = f"A1:{get_column_letter(len(COLUMNS))}{n}"

    # Level-Up Moves and TM/HM Moves (learnset entry k is species k + 1)
    lv = wb.create_sheet("Level-Up Moves", 2)
    tm = wb.create_sheet("TM-HM Moves", 3)
    for t, s in ((("Pokémon", "Name", "Level", "Move"), lv), (("Pokémon", "Name", "Move"), tm)):
        for c, h in enumerate(t, 1):
            s.cell(row=1, column=c, value=h)
    r1 = r2 = 2
    for k, e in enumerate(learn):
        const = sp[k + 1]
        name = labels.get(mons[k + 1]["name"], "")
        for m in e["levelUpMoves"]:
            for c, v in enumerate((const, name, m["level"], m["move"]), 1):
                lv.cell(row=r1, column=c, value=v).font = Font(name=FONT)
            r1 += 1
        for m in e["HMTMMoves"]:
            for c, v in enumerate((const, name, m), 1):
                tm.cell(row=r2, column=c, value=v).font = Font(name=FONT)
            r2 += 1
    style_header(lv, [26, 12, 8, 26])
    style_header(tm, [26, 12, 26])
    for s, col, last in ((lv, "D", r1 + 2000), (tm, "C", r2 + 2000)):
        dv = DataValidation(type="list", formula1=ranges["Moves"], allow_blank=True)
        dv.add(f"{col}2:{col}{last}")
        s.add_data_validation(dv)
        dv2 = DataValidation(type="list", formula1=ranges["Species"], allow_blank=True)
        dv2.add(f"A2:A{last}")
        s.add_data_validation(dv2)
    lv.auto_filter.ref = f"A1:D{r1 - 1}"
    tm.auto_filter.ref = f"A1:C{r2 - 1}"
    wb.save(out)
    print(f"{out}: {len(mons)} species, {r1 - 2} level-up moves, {r2 - 2} TM/HM moves")


def cell_value(v):
    if isinstance(v, str):
        v = v.strip()
        if v == "":
            return None
    return v


def as_int(v, where):
    if isinstance(v, bool):
        raise ValueError(f"{where}: expected a number, got {v!r}")
    if isinstance(v, float) and v.is_integer():
        return int(v)
    if isinstance(v, int):
        return v
    if isinstance(v, str) and re.fullmatch(r"-?\d+", v):
        return int(v)
    raise ValueError(f"{where}: expected a number, got {v!r}")


def ident(text):
    return re.sub(r"[^A-Za-z0-9]", "", text.title()) or "X"


def import_(up, src):
    mpath, lpath = paths(up)
    mons = json.load(open(mpath, encoding="utf-8"))
    learn = json.load(open(lpath, encoding="utf-8"))
    sp = species_constants(up)
    by_const = {v: k for k, v in sp.items()}
    names_text, labels = read_names(up)
    lst = lists(up)
    allowed = {"str:types": set(lst["Types"]), "str:abilities": set(lst["Abilities"])}
    errors = []
    wb = openpyxl.load_workbook(src, data_only=True)

    ws = wb["Pokémon"]
    header = [cell_value(c.value) for c in ws[1]]
    want = [t for t, *_ in COLUMNS]
    if header[:len(want)] != want:
        raise SystemExit("Pokémon tab: the column titles don't match (were columns moved or renamed?)")
    label_users = {}
    for e in mons:
        for k in ("name", "category"):
            label_users[e[k]] = label_users.get(e[k], 0) + 1
    new_labels = {}  # label -> text, to add or change in monster_names.s
    seen = set()
    for row in ws.iter_rows(min_row=2, values_only=True):
        row = [cell_value(v) for v in row[:len(COLUMNS)]]
        if all(v is None for v in row):
            continue
        i = row[0]
        try:
            i = as_int(i, "ID")
        except ValueError:
            errors.append(f"Pokémon tab: a row's ID {i!r} isn't a number")
            continue
        if i < 0 or i >= len(mons) or row[1] != sp.get(i):
            errors.append(f"Pokémon tab: row ID {i} / {row[1]} isn't an existing species "
                          "(adding species isn't supported yet)")
            continue
        seen.add(i)
        e = mons[i]
        new = {}
        pre, req = {}, {}
        lists_ = {}
        pairs = {}
        for (title, kind, key, _doc), v in zip(COLUMNS, row):
            where = f"{sp[i]} / {title}"
            try:
                if kind in ("id", "constant"):
                    continue
                if kind in ("name", "category"):
                    if v is None:
                        raise ValueError(f"{where}: can't be blank")
                    v = str(v)
                    label = e[key]
                    if labels.get(label) != v:
                        prefix = "MonsterName" if kind == "name" else "MonsterCategory"
                        same = [l for l, t in labels.items() if l.startswith(prefix) and t == v]
                        if kind == "category" and same:
                            label = same[0]
                        elif label_users.get(label, 0) == 1 and label in labels:
                            new_labels[label] = v  # (only this species uses it: change its text)
                        else:
                            label = prefix + ident(v)
                            while label in labels and labels[label] != v:
                                label += "X"
                            new_labels[label] = v
                            labels[label] = v
                    new[key] = label
                elif v is None:
                    continue
                elif kind == "str":
                    new[key] = str(v)
                elif kind == "int":
                    new[key] = as_int(v, where)
                elif kind == "bool":
                    s = str(v).upper()
                    if s not in ("TRUE", "FALSE", "1", "0"):
                        raise ValueError(f"{where}: expected TRUE or FALSE, got {v!r}")
                    new[key] = s in ("TRUE", "1")
                elif kind in ("list0", "list1"):
                    if str(v) not in allowed[f"str:{key}"]:
                        raise ValueError(f"{where}: {v!r} isn't on the Lists tab")
                    lists_.setdefault(key, [None, None])[0 if kind == "list0" else 1] = str(v)
                elif kind in ("pair0", "pair1"):
                    pairs.setdefault(key, [0, 0])[0 if kind == "pair0" else 1] = as_int(v, where)
                elif kind == "evo":
                    pre[key] = str(v)
                elif kind == "req":
                    if key == "mainRequirement" and isinstance(v, str) and v.startswith("ITEM_"):
                        if v not in lst["Items"]:
                            raise ValueError(f"{where}: {v!r} isn't an item code")
                        req[key] = v
                    else:
                        req[key] = as_int(v, where)
            except ValueError as ex:
                errors.append(str(ex))
        for key, (a, b) in lists_.items():
            if a is None:
                errors.append(f"{sp[i]}: {key}: the first one can't be blank")
                continue
            new[key] = [a] if b is None else [a, b]
        for key in ("types", "abilities"):
            if key not in new:
                errors.append(f"{sp[i]}: needs at least one of {key}")
        new.update(pairs)
        for key in ("baseAtkSpAtk", "baseDefSpDef", "dexInternal", "alphabetParent"):
            if key not in new:
                errors.append(f"{sp[i]}: {key} can't be blank")
        if pre:
            if set(pre) != {"evolveFrom", "evolveType"}:
                errors.append(f"{sp[i]}: Evolves From and Evolve Method go together")
            elif pre["evolveFrom"] not in by_const or pre["evolveType"] not in lst["Evolve Methods"]:
                errors.append(f"{sp[i]}: unknown Evolves From / Evolve Method")
            new["preEvolution"] = pre
            new["evolutionRequirements"] = req if req else {"mainRequirement": 0}
        elif req:
            errors.append(f"{sp[i]}: an evolution requirement without Evolves From")
        # The JSON's own key order (so an unchanged sheet gives the same file)
        order = list(e.keys()) + [k for k in new if k not in e]
        mons[i] = {k: new[k] for k in order if k in new}
    missing = [sp[i] for i in range(len(mons)) if i not in seen]
    if missing:
        errors.append(f"Pokémon tab: rows missing for {', '.join(missing[:8])}{'...' if len(missing) > 8 else ''}")

    # Moves
    moves = set(lst["Moves"])
    lvl = {k: [] for k in range(len(learn))}
    tms = {k: [] for k in range(len(learn))}
    for tab, has_level in (("Level-Up Moves", True), ("TM-HM Moves", False)):
        for r, row in enumerate(wb[tab].iter_rows(min_row=2, values_only=True), 2):
            row = [cell_value(v) for v in row[:4 if has_level else 3]]
            if all(v is None for v in row):
                continue
            const, move = row[0], row[-1]
            k = by_const.get(const, -1) - 1
            if k < 0 or k >= len(learn):
                errors.append(f"{tab} row {r}: {const!r} isn't a species with moves")
                continue
            if move not in moves:
                errors.append(f"{tab} row {r}: {move!r} isn't a move code")
                continue
            if has_level:
                try:
                    level = as_int(row[2], f"{tab} row {r} / Level")
                except ValueError as ex:
                    errors.append(str(ex))
                    continue
                lvl[k].append({"move": move, "level": level})
            else:
                tms[k].append(move)
    for k, e in enumerate(learn):
        e["levelUpMoves"] = lvl[k]
        e["HMTMMoves"] = tms[k]

    if errors:
        print(f"{len(errors)} problem(s); nothing was written:")
        for x in errors[:60]:
            print("  " + x)
        raise SystemExit(1)

    with open(mpath, "w", encoding="utf-8") as f:
        json.dump(mons, f, indent=4, ensure_ascii=False)
        f.write("\n")
    with open(lpath, "w", encoding="utf-8") as f:
        json.dump(learn, f, indent=4, ensure_ascii=False)
        f.write("\n")
    if new_labels:
        text = names_text
        for label, value in new_labels.items():
            v = value.replace("\\", "\\\\").replace('"', '\\"')
            pat = re.compile(r"^(" + re.escape(label) + r":\n\.(?:string|asciz) \")(?:[^\"\\]|\\.)*?((?:\\0)?\")", re.M)
            if pat.search(text):
                text = pat.sub(lambda m: m.group(1) + v + m.group(2), text)
            else:
                text = text.rstrip("\n") + f"\n\n.global {label}\n{label}:\n.string \"{v}\\0\"\n.align 2,0\n"
        open(os.path.join(up, "data/monster/monster_names.s"), "w", encoding="utf-8").write(text)
    print(f"{src}: written ({len(new_labels)} name/category text change(s))")


def main():
    if len(sys.argv) != 4 or sys.argv[1] not in ("export", "import"):
        raise SystemExit(__doc__)
    (export if sys.argv[1] == "export" else import_)(sys.argv[2], sys.argv[3])


if __name__ == "__main__":
    main()
