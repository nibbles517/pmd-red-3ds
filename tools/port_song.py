#!/usr/bin/env python3
"""Rewrites a pmd-red song (sound/songs/*.s) for the 3DS port.

In m4a song tracks, GOTO and PATT are followed by a 4-byte jump target at
whatever offset the byte stream reaches, so most are unaligned. The .3dsx
format can only relocate aligned pointers, so the port stores these targets
self-relative instead (`target - .`), which the assembler resolves to a
constant with no relocation. The driver adds the field's address back
(m4a_1.s, ply_goto_1, under PLATFORM_3DS). Song headers keep normal pointers.

usage: port_song.py IN.s OUT.s
"""
import re, sys

JUMP = re.compile(r'^\s*\.byte\s+(GOTO|PATT)\s*(@.*)?$')
WORD = re.compile(r'^(\s*)\.word(\s+)(.+?)\s*(@.*)?$')


def main():
    src, dst = sys.argv[1], sys.argv[2]
    out, after_jump, count = [], False, 0
    for line in open(src):
        if after_jump:
            m = WORD.match(line.rstrip('\n'))
            if not m:
                sys.exit(f'{src}: expected a .word after GOTO/PATT, got: {line.strip()}')
            out.append(f'{m.group(1)}.word{m.group(2)}({m.group(3)}) - .\n')
            count += 1
            after_jump = False
            continue
        out.append(line)
        after_jump = bool(JUMP.match(line.rstrip('\n')))
    open(dst, 'w').writelines(out)


if __name__ == '__main__':
    main()
