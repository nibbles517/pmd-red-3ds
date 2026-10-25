#!/usr/bin/env python3
"""Runs the pmd-red-3ds test build under the Unicorn CPU emulator.

The test build is the same game and platform code as the 3DS build, linked
with a tiny backend (port/harness/backend.c) that talks to this script through
fake I/O registers at 0xF0000000. Lets the port be booted, driven with button
presses, and checked frame by frame without a 3DS.

usage:
  harness.py ELF --frames N [--shot F ...] [--shot-every K] [--out DIR]
             [--press FRAME:KEYS:DURATION ...] [--save FILE] [--wav FILE]

  --press 300:A:5   hold A for 5 frames starting at frame 300
  keys: A B SELECT START RIGHT LEFT UP DOWN R L, combine with '+'
needs: libunicorn (LIBUNICORN_PATH) and its Python bindings on PYTHONPATH
"""
import argparse, os, struct, sys, time, wave, zlib

from unicorn import Uc, UcError, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_MEM_WRITE, UC_HOOK_MEM_READ, \
    UC_HOOK_MEM_UNMAPPED, UC_PROT_ALL
from unicorn.arm_const import UC_ARM_REG_PC, UC_ARM_REG_LR, UC_ARM_REG_SP, UC_CPU_ARM_1176

IO_BASE = 0xF0000000
KEYS = {'A': 0, 'B': 1, 'SELECT': 2, 'START': 3, 'RIGHT': 4, 'LEFT': 5, 'UP': 6, 'DOWN': 7, 'R': 8, 'L': 9}


def load_elf(uc, path):
    data = open(path, 'rb').read()
    assert data[:4] == b'\x7fELF' and data[4] == 1, 'need a 32-bit ELF'
    e_entry, e_phoff = struct.unpack_from('<II', data, 24)
    e_phentsize, e_phnum = struct.unpack_from('<HH', data, 42)
    lo, hi = None, 0
    segs = []
    for i in range(e_phnum):
        p_type, p_offset, p_vaddr, _, p_filesz, p_memsz = struct.unpack_from('<6I', data, e_phoff + i * e_phentsize)
        if p_type != 1 or p_memsz == 0:
            continue
        segs.append((p_offset, p_vaddr, p_filesz, p_memsz))
        lo = p_vaddr if lo is None else min(lo, p_vaddr)
        hi = max(hi, p_vaddr + p_memsz)
    lo &= ~0xFFFFF
    hi = (hi + 0x3FFFFF) & ~0xFFFFF  # covers .bss, heap and stack after the last segment
    uc.mem_map(lo, hi - lo, UC_PROT_ALL)
    for off, vaddr, filesz, memsz in segs:
        uc.mem_write(vaddr, data[off:off + filesz])
    return e_entry


def symbols(path):
    """name -> address, from the ELF symbol table (for crash reports)."""
    data = open(path, 'rb').read()
    e_shoff, = struct.unpack_from('<I', data, 32)
    e_shentsize, e_shnum = struct.unpack_from('<HH', data, 46)
    secs = [struct.unpack_from('<10I', data, e_shoff + i * e_shentsize) for i in range(e_shnum)]
    out = []
    for s in secs:
        if s[1] != 2:  # SHT_SYMTAB
            continue
        strtab = secs[s[6]]
        for j in range(s[5] // 16):
            st_name, st_value, st_size, st_info = struct.unpack_from('<IIIB', data, s[4] + j * 16)
            if st_info & 0xF in (1, 2) and st_name:  # object or function
                name = data[strtab[4] + st_name:data.index(b'\0', strtab[4] + st_name)].decode()
                out.append((st_value & ~1, st_size, name))
    out.sort()
    return out


def where(syms, addr):
    best = None
    for v, size, name in syms:
        if v <= addr:
            best = (v, name)
        else:
            break
    return f'{best[1]}+{addr - best[0]:#x}' if best else hex(addr)


def write_png(path, frame_bytes):
    rows = []
    for y in range(160):
        row = bytearray(b'\0')
        for x in range(240):
            c, = struct.unpack_from('<H', frame_bytes, (y * 240 + x) * 2)
            r, g, b = c & 31, (c >> 5) & 31, (c >> 10) & 31
            row += bytes(((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2)))
        rows.append(bytes(row))
    raw = zlib.compress(b''.join(rows), 9)

    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 240, 160, 8, 2, 0, 0, 0)) + \
        chunk(b'IDAT', raw) + chunk(b'IEND', b'')
    open(path, 'wb').write(png)


class Harness:
    def __init__(self, args):
        self.args = args
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        self.uc.ctl_set_cpu_model(UC_CPU_ARM_1176)
        self.entry = load_elf(self.uc, args.elf)
        self.syms = symbols(args.elf)
        self.uc.mem_map(IO_BASE, 0x1000)
        self.uc.hook_add(UC_HOOK_MEM_WRITE, self.on_write, begin=IO_BASE, end=IO_BASE + 0xFFF)
        self.uc.hook_add(UC_HOOK_MEM_READ, self.on_read, begin=IO_BASE, end=IO_BASE + 0xFFF)
        self.regs = [0] * 16
        self.line = bytearray()
        self.frame = 0
        self.last_frame = None
        self.exit_code = None
        self.presses = []
        for p in args.press:
            f, keys, dur = p.split(':')
            mask = 0
            for k in keys.split('+'):
                mask |= 1 << KEYS[k.upper()]
            self.presses.append((int(f), int(f) + int(dur), mask))
        self.shots = set(args.shot)
        self.audio = bytearray()
        self.audio_rate = 0
        os.makedirs(args.out, exist_ok=True)

    def keys(self):
        m = 0
        for start, end, mask in self.presses:
            if start <= self.frame < end:
                m |= mask
        return m

    def on_read(self, uc, access, addr, size, value, user):
        reg = (addr - IO_BASE) // 4
        val = self.keys() if reg == 2 else self.regs[reg]
        uc.mem_write(addr, struct.pack('<I', val)[:size])

    def on_write(self, uc, access, addr, size, value, user):
        reg = (addr - IO_BASE) // 4
        self.regs[reg] = value
        if reg == 0:  # putchar
            if value == 10:
                print('[game]', self.line.decode(errors='replace'), flush=True)
                self.line.clear()
            else:
                self.line.append(value & 0xFF)
        elif reg == 1:  # present
            self.last_frame = bytes(uc.mem_read(value, 240 * 160 * 2))
            if self.frame in self.shots or (self.args.shot_every and self.frame % self.args.shot_every == 0):
                write_png(os.path.join(self.args.out, f'frame_{self.frame:05d}.png'), self.last_frame)
        elif reg == 3:  # end of frame
            self.frame += 1
            if self.frame >= self.args.frames:
                uc.emu_stop()
        elif reg == 4:  # exit
            self.exit_code = value
            uc.emu_stop()
        elif reg == 7:  # save command
            buf, length = self.regs[5], self.regs[6]
            if value == 1 and self.args.save:
                open(self.args.save, 'wb').write(bytes(uc.mem_read(buf, length)))
            elif value == 2:
                ok = 0
                if self.args.save and os.path.exists(self.args.save):
                    d = open(self.args.save, 'rb').read()[:length]
                    uc.mem_write(buf, d)
                    ok = 1
                self.regs[8] = ok
        elif reg == 12:  # audio chunk
            left = bytes(uc.mem_read(self.regs[9], value))
            right = bytes(uc.mem_read(self.regs[10], value))
            self.audio_rate = self.regs[11]
            for l, r in zip(left, right):
                self.audio += bytes(((l + 128) & 0xFF, (r + 128) & 0xFF))

    def run(self):
        t0 = time.time()
        try:
            self.uc.emu_start(self.entry, 0xFFFFFFFF)
        except UcError as e:
            pc = self.uc.reg_read(UC_ARM_REG_PC)
            lr = self.uc.reg_read(UC_ARM_REG_LR)
            print(f'CRASH at frame {self.frame}: {e}\n  pc={pc:#010x} {where(self.syms, pc)}\n'
                  f'  lr={lr:#010x} {where(self.syms, lr)}', flush=True)
            if self.last_frame:
                write_png(os.path.join(self.args.out, 'crash_last_frame.png'), self.last_frame)
            return 2
        dt = time.time() - t0
        print(f'ran {self.frame} frames in {dt:.1f}s ({self.frame / max(dt, 1e-6):.1f} fps emulated)')
        if self.last_frame:
            write_png(os.path.join(self.args.out, 'last_frame.png'), self.last_frame)
        if self.args.wav and self.audio:
            with wave.open(self.args.wav, 'wb') as w:
                w.setnchannels(2)
                w.setsampwidth(1)
                w.setframerate(self.audio_rate or 13379)
                w.writeframes(bytes(self.audio))
        return self.exit_code or 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('elf')
    ap.add_argument('--frames', type=int, default=600)
    ap.add_argument('--shot', type=int, action='append', default=[])
    ap.add_argument('--shot-every', type=int, default=0)
    ap.add_argument('--press', action='append', default=[])
    ap.add_argument('--out', default='build/shots')
    ap.add_argument('--save')
    ap.add_argument('--wav')
    return Harness(ap.parse_args()).run()


if __name__ == '__main__':
    sys.exit(main())
