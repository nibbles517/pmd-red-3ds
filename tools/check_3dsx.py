#!/usr/bin/env python3
"""Checks a .3dsx the way Azahar's and the 3DS's loaders process it.

Replays the relocation pass of Azahar's 3DSX loader (src/core/loader/3dsx.cpp)
and reports anything it would reject, plus the memory the program needs.

usage: check_3dsx.py FILE.3dsx [ELF for symbol names]
"""
import bisect, struct, subprocess, sys

BASE = 0x00100000


def main():
    path = sys.argv[1]
    d = open(path, 'rb').read()
    magic, hsize, rsize, ver, flags, code, ro, data, bss = struct.unpack_from('<4sHHIIIIII', d, 0)
    assert magic == b'3DSX', 'not a 3dsx'
    segs = [(s + 0xFFF) & ~0xFFF for s in (code, ro, data)]
    print(f'code {code:#x}  rodata {ro:#x}  data {data:#x} (bss {bss:#x})')
    print(f'memory for the program image: {sum(segs) / 2**20:.1f} MiB')
    nt = rsize // 4
    off = hsize
    rel = [struct.unpack_from(f'<{nt}I', d, off + i * rsize) for i in range(3)]
    off += 3 * rsize
    image = bytearray()
    for size, padded in ((code, segs[0]), (ro, segs[1]), (data - bss, segs[2])):
        image += d[off:off + size] + bytes(padded - size)
        off += size
    seg_start = [0, segs[0], segs[0] + segs[1]]
    offsets = [segs[0], segs[0] + segs[1]]

    syms = []
    if len(sys.argv) > 2:
        nm = subprocess.check_output(['arm-none-eabi-nm', '-n', sys.argv[2]], text=True)
        syms = [(int(p[0], 16), p[2]) for p in (l.split() for l in nm.splitlines()) if len(p) == 3]
    addrs = [s[0] for s in syms]

    def name(a):
        i = bisect.bisect_right(addrs, a) - 1
        return f'{syms[i][1]}+{a - syms[i][0]:#x}' if i >= 0 else hex(a)

    bad = 0
    for seg in range(3):
        pos = seg_start[seg] // 4
        end = (seg_start[seg] + segs[seg]) // 4
        for table in range(nt):
            n = rel[seg][table]
            entries = struct.unpack_from(f'<{2 * n}H', d, off)
            off += 4 * n
            if table >= 2:
                continue
            for k in range(n):
                skip, patch = entries[2 * k], entries[2 * k + 1]
                pos += skip
                for _ in range(patch):
                    if pos >= end:
                        break
                    v, = struct.unpack_from('<I', image, pos * 4)
                    sub = v >> 28
                    if (table == 0 and sub != 0) or (table == 1 and sub > 1):
                        bad += 1
                        if bad <= 10:
                            where = BASE + pos * 4
                            print(f'REJECTED: {"absolute" if table == 0 else "relative"} reloc at '
                                  f'{where:#010x} ({name(where)}) stored {v:#010x}')
                    pos += 1
    print(f'{bad} relocations a loader would reject' if bad else 'relocations OK')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
