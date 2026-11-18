#!/usr/bin/env python3
"""Takes the ROM's own data out of the game's objects, for a .3dsx to share.

For each object (pret's data, songs, game C files), each section's bytes are
compared with the ROM where pret's matching build puts that object's section
(its map: pmd_red.map). Runs that are the ROM's own bytes (not counting the
words the linker fills in) are zeroed in a copy of the object, and listed in
a table the game reads at start to copy them back from the player's ROM
(port/core/romdata.c):

    rom_scrub.py map ROM out_dir table.s obj... [--strip PREFIX]

A ROM is needed only to make the map's offsets (the same everywhere); the
cloud build has none, so it reads the offsets from a list made here instead:

    rom_scrub.py --list list.txt map ROM obj...         (here: make the list)
    rom_scrub.py --from-list list.txt out_dir table.s obj... (anywhere)

Each list line: "<object> <section> <offset> <rom offset> <length>". An object
is named by its path from the game's object folder (data/x.o, songs/seq_000.o,
src files as x.o).
"""
import lzma
import os
import struct
import subprocess
import sys

MIN_RUN = 8  # shorter matches are left (chance agreement; not worth a table entry)


# --- A small ELF32 (little-endian) reader/writer: sections and relocations --

class Elf:
    def __init__(self, path):
        self.path = path
        self.data = bytearray(open(path, 'rb').read())
        d = self.data
        if d[:4] != b'\x7fELF' or d[4] != 1 or d[5] != 1:
            raise ValueError(path + ': not ELF32 LE')
        self.shoff = struct.unpack_from('<I', d, 0x20)[0]
        self.shentsize, self.shnum, self.shstrndx = struct.unpack_from('<HHH', d, 0x2E)
        self.sections = []
        for i in range(self.shnum):
            o = self.shoff + i * self.shentsize
            name, typ, flags, addr, off, size, link, info, align, entsize = struct.unpack_from('<10I', d, o)
            self.sections.append(dict(idx=i, name_off=name, type=typ, flags=flags, off=off, size=size,
                                      link=link, info=info, entsize=entsize))
        strtab = self.sections[self.shstrndx]
        for s in self.sections:
            n = s['name_off']
            end = d.index(b'\0', strtab['off'] + n)
            s['name'] = d[strtab['off'] + n:end].decode()

    def symbols(self):
        """(name, section index, value, size, local, file) of each symbol."""
        out = []
        for s in self.sections:
            if s['type'] != 2:  # SHT_SYMTAB
                continue
            strtab = self.sections[s['link']]
            cur_file = None
            for k in range(s['size'] // 16):
                name, value, size, info, other, shndx = struct.unpack_from('<IIIBBH', self.data, s['off'] + k * 16)
                end = self.data.index(b'\0', strtab['off'] + name)
                nm = self.data[strtab['off'] + name:end].decode('latin-1')
                typ, bind = info & 0xF, info >> 4
                if typ == 4:  # STT_FILE
                    cur_file = os.path.basename(nm)
                    continue
                out.append((nm, shndx, value, size, bind == 0, cur_file, typ))
        return out

    def reloc_bytes(self, sec_idx):
        """The byte offsets in a section the linker writes (its relocations)."""
        cover = set()
        for s in self.sections:
            if s['type'] in (9, 4) and s['info'] == sec_idx:  # SHT_REL, SHT_RELA
                ent = 8 if s['type'] == 9 else 12
                for k in range(s['size'] // ent):
                    r_off, r_info = struct.unpack_from('<II', self.data, s['off'] + k * ent)
                    rtype = r_info & 0xFF
                    width = 2 if rtype in (10, 11) else 1 if rtype == 8 else 4  # (THM_CALL... words otherwise)
                    if rtype in (10,):  # R_ARM_THM_CALL: two halfwords
                        width = 4
                    for b in range(width):
                        cover.add(r_off + b)
        return cover

    def save(self, path):
        open(path, 'wb').write(self.data)


def parse_map(path):
    """pret's map: (object, section) -> GBA address, for the ROM's sections."""
    out = {}
    pending = None
    for line in open(path, encoding='latin-1'):
        parts = line.split()
        if not parts:
            pending = None
            continue
        if len(parts) == 1 and line.startswith(' .') and not line.startswith('  '):
            pending = parts[0]  # (a long section name: the rest on the next line)
            continue
        if pending is not None and len(parts) == 3 and parts[0].startswith('0x'):
            sec, addr, size, obj = pending, parts[0], parts[1], parts[2]
        elif len(parts) == 4 and line.startswith(' .') and parts[1].startswith('0x'):
            sec, addr, size, obj = parts
        else:
            pending = None
            continue
        pending = None
        a, n = int(addr, 16), int(size, 16)
        if 0x08000000 <= a < 0x0A000000 and n > 0:
            out[(obj, sec)] = (a - 0x08000000, n)
    return out


MAP_OBJECTS = set()


def map_name(rel):
    """The object's name in pret's map, from its path in the port's object
    folder (pret's data/*.s are data/x.o there too; src/data/*.c are not)."""
    if rel.startswith('songs/'):
        return 'sound/' + rel
    if rel.startswith('data/') and rel in MAP_OBJECTS:
        return rel
    return 'src/' + rel


# Section names: the port's (-ffunction-sections / -fdata-sections) to pret's
def map_section(name):
    for base in ('.rodata', '.data', '.text'):
        if name == base or name.startswith(base + '.'):
            return base
    return name


def runs_for(elf, s, rom, rom_off, rom_len, first=0, relocs=None, min_run=MIN_RUN):
    """Where the section's bytes (from `first`, rom_len of them) are the ROM's
    at rom_off (relocated words left out): (offset in the section, length)."""
    data = elf.data[s['off']:s['off'] + s['size']]
    if relocs is None:
        relocs = elf.reloc_bytes(s['idx'])
    n = min(len(data) - first, rom_len, len(rom) - rom_off)
    runs, start = [], None
    for i in range(n + 1):
        same = i < n and (first + i) not in relocs and data[first + i] == rom[rom_off + i]
        if same and start is None:
            start = i
        elif not same and start is not None:
            if i - start >= min_run:
                runs.append((first + start, i - start))
            start = None
    return runs


def find_in(elf, s, value, size, relocs, rom, where, obj):
    """A symbol pret's build has no name for (agbcc keeps no static ones):
    found by its bytes, in that object's own part of the ROM."""
    data = elf.data[s['off'] + value:s['off'] + value + size]
    # its longest stretch without relocated words
    best, start = (0, 0), None
    for i in range(size + 1):
        ok = i < size and (value + i) not in relocs
        if ok and start is None:
            start = i
        elif not ok and start is not None:
            if i - start > best[1] - best[0]:
                best = (start, i)
            start = None
    a, b = best
    if b - a < 16:
        return None
    pattern = bytes(data[a:min(b, a + 64)])
    for sec in ('.rodata', '.data'):
        if (obj, sec) not in where:
            continue
        lo, n = where[(obj, sec)]
        at = rom.find(pattern, lo, lo + n)
        if at >= 0:
            return at - a
    return None


class RomIndex:
    """Every place in the ROM, sorted by the 8 bytes there: finds a piece of
    data anywhere in the ROM at once (bytes.find over 32 MiB is slow)."""
    def __init__(self, rom):
        import numpy as np
        self.np, self.rom = np, rom
        a = np.frombuffer(rom + bytes(8), dtype=np.uint8)
        keys = np.zeros(len(rom), dtype=np.uint64)
        for k in range(8):  # big-endian: sorted keys sort as the bytes do
            keys |= a[k:k + len(rom)].astype(np.uint64) << np.uint64(8 * (7 - k))
        self.order = np.argsort(keys, kind='stable')
        self.keys = keys[self.order]

    def find(self, piece, limit=64):
        np = self.np
        key = np.uint64(int.from_bytes(piece[:8], 'big'))
        lo = np.searchsorted(self.keys, key, 'left')
        hi = np.searchsorted(self.keys, key, 'right')
        if hi - lo > limit:  # (a common start: the slow way)
            return self.rom.find(piece)
        for i in range(lo, hi):
            at = int(self.order[i])
            if self.rom[at:at + len(piece)] == piece:
                return at
        return -1


ROM_INDEX = None


def fill_gaps(elf, s, syms, rom, where, obj, found):
    """The rest of the section: any stretch whose bytes are somewhere in the
    ROM (the same bytes, so any place that has them will do), looked for in
    the object's own part of the ROM first."""
    data = bytes(elf.data[s['off']:s['off'] + s['size']])
    relocs = elf.reloc_bytes(s['idx'])
    done = bytearray(len(data))
    for off, _, length in found:
        done[off:off + length] = b'\1' * length
    cuts = set(v for nm, shndx, v, sz, *_ in syms if shndx == s['idx'])
    near = [where[(obj, sec)] for sec in ('.rodata', '.data') if (obj, sec) in where]
    out = []

    def look(a, b):
        piece = data[a:b]
        if len(set(piece)) <= 1:  # (all one byte: nothing of the ROM's)
            return
        for lo, n in near:
            at = rom.find(piece, lo, lo + n)
            if at >= 0:
                out.append((a, at, b - a))
                return
        at = ROM_INDEX.find(piece)
        if at >= 0:
            out.append((a, at, b - a))
            return
        if b - a >= 2 * 16:
            m = (a + b) // 2
            look(a, m)
            look(m, b)

    start = None
    for i in range(len(data) + 1):
        free = i < len(data) and not done[i] and i not in relocs and not (i in cuts and start is not None)
        if i < len(data) and i in cuts and start is not None:
            if i - start >= MIN_RUN:
                look(start, i)
            start = None
            free = not done[i] and i not in relocs
        if free and start is None:
            start = i
        elif not free and start is not None:
            if i - start >= MIN_RUN:
                look(start, i)
            start = None
    return out


def parse_gba_symbols(elf_path):
    """pret's matching build: global symbol -> ROM offset; (file, local) too."""
    elf = Elf(elf_path)
    glob, loc = {}, {}
    for nm, shndx, value, size, local, f, typ in elf.symbols():
        if not (0x08000000 <= value < 0x0A000000) or typ == 2 or nm.startswith('$'):  # (not functions or ARM mapping)
            continue
        if local:
            loc[(f, nm)] = value - 0x08000000
        else:
            glob[nm] = value - 0x08000000
    return glob, loc


def run_sum(rom, at, length):
    """A run's check (romdata.c checks the sum of all of them): FNV-1a of its
    bytes, started from its ROM offset."""
    h = (2166136261 ^ at) & 0xFFFFFFFF
    for c in rom[at:at + length]:
        h = ((h ^ c) * 16777619) & 0xFFFFFFFF
    return h


def scrub_section(elf, s, runs):
    for off, length in runs:
        elf.data[s['off'] + off:s['off'] + off + length] = bytes(length)


def write_table(path, entries, check):
    """The table for romdata.c. Per section (in the order of where they start
    in the ROM): its start, its number of runs (ULEB128), then each run as
    ULEB128 bytes from the last run's end, SLEB128 ROM offset from the last
    run's ROM end (the previous section's, for its first), ULEB128 length."""
    groups = {}
    for sym, off, rom_off, length in entries:
        groups.setdefault(sym, []).append((off, rom_off, length))
    order = sorted(groups, key=lambda g: min(r[1] for r in groups[g]))
    with open(path, 'w') as f:
        f.write('@ made by tools/rom_scrub.py: the ROM data to copy back at start\n')
        f.write('.section .rodata.port_romdata,"a"\n.balign 4\n')
        f.write('.global gPortRomDataCount\ngPortRomDataCount:\n.4byte %d\n' % len(entries))
        f.write('.global gPortRomDataSum\ngPortRomDataSum:\n.4byte 0x%08x\n' % check)
        f.write('.global gPortRomDataGroups\ngPortRomDataGroups:\n.4byte %d\n' % len(order))
        f.write('.global gPortRomData\ngPortRomData:\n')
        rom_end = 0
        for sym in order:
            runs = sorted(groups[sym])
            f.write('.4byte %s\n.uleb128 %d\n' % (sym, len(runs)))
            end = 0
            vals = []
            for off, rom_off, length in runs:
                vals.append('.uleb128 %d\n.sleb128 %d\n.uleb128 %d\n' % (off - end, rom_off - rom_end, length))
                end, rom_end = off + length, rom_off + length
            f.write(''.join(vals))
            f.write('.balign 4\n')


def main():
    args = sys.argv[1:]
    strip = None
    if '--strip' in args:
        i = args.index('--strip')
        strip = args[i + 1]
        del args[i:i + 2]

    def rel_name(p):
        return os.path.relpath(p, strip) if strip else p

    if args[0] == '--list':  # here, with the ROM: the offsets list
        list_path, map_path, rom_path, objs = args[1], args[2], args[3], args[4:]
        rom = open(rom_path, 'rb').read()
        global ROM_INDEX
        ROM_INDEX = RomIndex(rom)
        where = parse_map(map_path)
        MAP_OBJECTS.update(obj for obj, sec in where)
        gba_glob, gba_loc = parse_gba_symbols(os.path.splitext(map_path)[0] + '.elf')
        total = matched = check = 0
        with open(list_path, 'w') as f:
            f.write('# made by tools/rom_scrub.py --list: <object> <section> <offset> <ROM offset> <length>\n')
            for p in objs:
                elf = Elf(p)
                rel = rel_name(p)
                syms = elf.symbols()
                for s in elf.sections:
                    if s['type'] != 1 or not (s['flags'] & 2) or s['size'] == 0:  # PROGBITS, ALLOC
                        continue
                    if s['flags'] & 4:  # (code: not the ROM's data)
                        continue
                    total += s['size']
                    key = (map_name(rel), map_section(s['name']))
                    found = []
                    if key in where and s['name'] == key[1]:  # the object's whole section, as pret's
                        rom_off, rom_len = where[key]
                        found = [(off, rom_off + off - 0, length) for off, length in runs_for(elf, s, rom, rom_off, rom_len, min_run=1 if rel in MAP_OBJECTS else MIN_RUN)]
                    else:  # each of its symbols, where pret's build put it
                        relocs = elf.reloc_bytes(s['idx'])
                        for nm, shndx, value, size, local, fl, typ in syms:
                            if shndx != s['idx'] or size == 0:
                                continue
                            addr = gba_loc.get((fl, nm)) if local else gba_glob.get(nm)
                            if addr is None:
                                addr = find_in(elf, s, value, size, relocs, rom, where, map_name(rel))
                            if addr is None:
                                continue
                            for off, length in runs_for(elf, s, rom, addr, size, value, relocs):
                                found.append((off, addr + off - value, length))
                    found += fill_gaps(elf, s, syms, rom, where, map_name(rel), found)
                    for off, rom_at, length in found:
                        f.write('%s %s %d %d %d\n' % (rel, s['name'], off, rom_at, length))
                        matched += length
                        check = (check + run_sum(rom, rom_at, length)) & 0xFFFFFFFF
            f.write('# sum 0x%08x\n' % check)
        print('rom_scrub: %d of %d data bytes are the ROM\'s' % (matched, total))
        return

    if args[0] == '--from-list':  # anywhere: zero the listed runs, and the table
        objcopy = 'arm-none-eabi-objcopy'
        if '--objcopy' in args:
            k = args.index('--objcopy')
            objcopy = args[k + 1]
            del args[k:k + 2]
        list_path, out_dir, table_path, objs = args[1], args[2], args[3], args[4:]
        listed, check = {}, 0
        opener = lzma.open if list_path.endswith('.xz') else open
        for line in opener(list_path, 'rt'):
            if line.startswith('# sum '):
                check = int(line.split()[2], 16)
            if line.startswith('#') or not line.strip():
                continue
            rel, sec, off, rom_off, length = line.split()
            listed.setdefault(rel, []).append((sec, int(off), int(rom_off), int(length)))
        entries = []
        for n, p in enumerate(objs):
            rel = rel_name(p)
            out = os.path.join(out_dir, rel)
            os.makedirs(os.path.dirname(out), exist_ok=True)
            elf = Elf(p)
            by_sec = {}
            for sec, off, rom_off, length in listed.pop(rel, []):
                by_sec.setdefault(sec, []).append((off, rom_off, length))
            opts = []
            for s in elf.sections:
                if s['name'] not in by_sec:
                    continue
                runs = by_sec[s['name']]
                for off, rom_off, length in runs:
                    if off + length > s['size']:
                        sys.exit('rom_scrub: %s %s: the list does not fit this object (not built the same way?)' % (rel, s['name']))
                scrub_section(elf, s, [(o, l) for o, _, l in runs])
                # a symbol at the section's start, for the table; and read-only
                # sections made writable (.data.rom...), to be written at start
                sym = 'port_romdata_%d_%d' % (n, s['idx'])
                opts.append('--add-symbol=%s=%s:0,global' % (sym, s['name']))
                for off, rom_off, length in runs:
                    entries.append((sym, off, rom_off, length))
            elf.save(out)
            if opts:
                subprocess.check_call([objcopy] + opts + [out])
                renames = []
                for s in elf.sections:
                    if s['name'] not in by_sec:
                        continue
                    whole = sum(l for _, _, l in by_sec[s['name']]) == s['size']
                    if whole and s['name'].startswith(('.rodata', '.data')):
                        # all of it the ROM's: no bytes in the .3dsx at all,
                        # only room (zeroed at load) for romdata.c to fill
                        renames.append('--rename-section=%s=.bss.rom%s,alloc' % (s['name'], s['name']))
                    elif not (s['flags'] & 1):
                        renames.append('--rename-section=%s=.data.rom%s,alloc,load,contents,data' % (s['name'], s['name']))
                if renames:
                    subprocess.check_call([objcopy] + renames + [out])
        if listed:
            sys.exit('rom_scrub: objects in the list not given: ' + ' '.join(sorted(listed)[:10]))
        write_table(table_path, entries, check)
        print('rom_scrub: %d objects, %d runs, %d bytes left to the ROM' %
              (len(objs), len(entries), sum(e[3] for e in entries)))
        return

    print(__doc__)
    sys.exit(1)


if __name__ == '__main__':
    main()
