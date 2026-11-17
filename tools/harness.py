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
  --save-state F    write a snapshot (CPU + memory) at the end of the run
  --load-state F    start from a snapshot instead of booting; frame numbers
                    for --press/--shot continue from the snapshot's frame
  keys: A B SELECT START RIGHT LEFT UP DOWN R L, combine with '+'
needs: libunicorn (LIBUNICORN_PATH) and its Python bindings on PYTHONPATH
"""
import argparse, os, struct, sys, time, wave, zlib

from unicorn import Uc, UcError, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_MEM_WRITE, UC_HOOK_MEM_READ, \
    UC_HOOK_MEM_UNMAPPED, UC_PROT_ALL, UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_PC, UC_ARM_REG_LR, UC_ARM_REG_SP, UC_CPU_ARM_1176

IO_BASE = 0xF0000000
KEYS = {'A': 0, 'B': 1, 'SELECT': 2, 'START': 3, 'RIGHT': 4, 'LEFT': 5, 'UP': 6, 'DOWN': 7, 'R': 8, 'L': 9, 'Y': 10, 'X': 11, 'ZL': 12, 'ZR': 13, 'CUP': 14, 'CDOWN': 15}


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
    # Like the 3DS, only the code segment is executable: from the first
    # non-executable segment's page to the end (data, bss, heap, stack) is RW.
    ro_start = None
    for i in range(e_phnum):
        p_type, _, p_vaddr, _, _, p_memsz, p_flags = struct.unpack_from('<7I', data, e_phoff + i * e_phentsize)
        if p_type == 1 and p_memsz and not (p_flags & 1):
            start = p_vaddr & ~0xFFF
            ro_start = start if ro_start is None else min(ro_start, start)
    if ro_start is not None:
        from unicorn import UC_PROT_READ, UC_PROT_WRITE
        uc.mem_protect(ro_start, hi - ro_start, UC_PROT_READ | UC_PROT_WRITE)
    return e_entry


def reload_code(uc, path):
    """Writes the ELF's read-only segments (code, constant data) again: after
    --load-state, to try a new build's code or script data from an old
    snapshot (only while nothing moved: edits that keep every size)."""
    data = open(path, 'rb').read()
    e_phoff, = struct.unpack_from('<I', data, 28)
    e_phentsize, e_phnum = struct.unpack_from('<HH', data, 42)
    for i in range(e_phnum):
        p_type, p_offset, p_vaddr, _, p_filesz, p_memsz, p_flags = struct.unpack_from('<7I', data, e_phoff + i * e_phentsize)
        if p_type == 1 and p_filesz and not (p_flags & 2):
            uc.mem_write(p_vaddr, data[p_offset:p_offset + p_filesz])


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


def write_png(path, frame_bytes, width=256, height=192):
    rows = []
    for y in range(height):
        row = bytearray(b'\0')
        for x in range(width):
            c, = struct.unpack_from('<H', frame_bytes, (y * width + x) * 2)
            r, g, b = c & 31, (c >> 5) & 31, (c >> 10) & 31
            row += bytes(((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2)))
        rows.append(bytes(row))
    raw = zlib.compress(b''.join(rows), 9)

    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) + \
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
        self.regs = [0] * 32
        self.line = bytearray()
        self.frame = 0
        self.last_frame = None
        self.exit_code = None
        self.presses = []
        self.songs = {int(a): int(b) for a, b in (x.split(':') for x in args.song)}
        for p in args.press:
            f, keys, dur = p.split(':')
            mask = 0
            for k in keys.split('+'):
                mask |= 1 << KEYS[k.upper()]
            self.presses.append((int(f), int(f) + int(dur), mask))
        self.shots = set(args.shot)
        self.captures = set(args.capture)
        self.audio = bytearray()
        self.audio_rate = 0
        os.makedirs(args.out, exist_ok=True)
        self.watch_base = {}
        for w in args.watch:
            spec, _, ln = w.partition(':')
            name, _, off = spec.partition('+')
            addrs = [v for v, _, n in self.syms if n == name]
            a = addrs[0] + (int(off, 0) if off else 0)
            self.watch_base[w] = a
            self.uc.hook_add(UC_HOOK_MEM_WRITE, self.on_watch, user_data=w, begin=a, end=a + int(ln or '4', 0) - 1)
        for name in args.trace:
            addrs = [v for v, _, n in self.syms if n == name]
            if not addrs:
                print(f'--trace: no symbol {name}')
                continue
            a = addrs[0] & ~1
            self.uc.hook_add(UC_HOOK_CODE, self.on_trace, user_data=name, begin=a, end=a)

    REG_NAMES = None

    def _cpu_regs(self):
        import unicorn.arm_const as c
        names = [n for n in dir(c) if n.startswith('UC_ARM_REG_') and n not in ('UC_ARM_REG_INVALID', 'UC_ARM_REG_ENDING')]
        ids = []
        for n in sorted(names):
            v = getattr(c, n)
            if isinstance(v, int) and 0 < v < getattr(c, 'UC_ARM_REG_ENDING', 1000) and v not in ids:
                ids.append(v)
        return ids

    def ini_config(self):
        # --config FILE.ini: the text settings file (config.ini); else the
        # older binary one (config.bin), and config.ini is not read or written
        return bool(self.args.config) and self.args.config.lower().endswith('.ini')

    def sd_path(self, name):
        # the SD card: --sd DIR (default: the data folder)
        root = self.args.sd or self.args.data or (os.path.dirname(self.args.save) if self.args.save else '.')
        return os.path.join(root, name.lstrip('/'))

    def save_path(self):
        slot = getattr(self, 'save_slot', 0)
        if not self.args.save or slot == 0:
            return self.args.save
        stem, ext = os.path.splitext(self.args.save)
        return f'{stem}-{slot + 1}{ext}'

    def save_state(self, path):
        import pickle
        regs = {}
        for r in self._cpu_regs():
            try:
                regs[r] = self.uc.reg_read(r)
            except Exception:
                pass
        mem = [(b, e, bytes(self.uc.mem_read(b, e - b + 1))) for b, e, _ in self.uc.mem_regions() if b != IO_BASE]
        with open(path, 'wb') as f:
            pickle.dump({'regs': regs, 'mem': mem, 'frame': self.frame, 'io': self.regs}, f, protocol=4)

    def load_state(self, path):
        import pickle
        st = pickle.load(open(path, 'rb'))
        for b, e, data in st['mem']:
            self.uc.mem_write(b, data)
        # Restore CPSR/mode-related registers before the general ones.
        for r, v in st['regs'].items():
            try:
                self.uc.reg_write(r, v)
            except Exception:
                pass
        self.frame = st['frame']
        self.regs = list(st['io']) + [0] * (32 - len(st['io']))
        return self.uc.reg_read(UC_ARM_REG_PC)

    def on_watch(self, uc, access, addr, size, value, user):
        import unicorn.arm_const as c
        pc = uc.reg_read(UC_ARM_REG_PC); lr = uc.reg_read(UC_ARM_REG_LR)
        n = self.watch_count = getattr(self, 'watch_count', 0) + 1
        if n < 4000:
            print(f'[watch] f{self.frame} {user}+{addr - self.watch_base[user]:#x} <- {value:#x} ({size}) pc={where(self.syms, pc)} lr={where(self.syms, lr & ~1)}', flush=True)

    def on_trace(self, uc, addr, size, name):
        import unicorn.arm_const as c
        r = [uc.reg_read(getattr(c, f'UC_ARM_REG_R{i}')) for i in range(4)]
        lr = uc.reg_read(UC_ARM_REG_LR)
        print(f'[trace] f{self.frame} {name}(' + ', '.join(f'{v:#x}' for v in r) + f') <- {where(self.syms, lr & ~1)}', flush=True)

    def wants_shot(self):
        return self.frame in self.shots or bool(self.args.shot_every and self.frame % self.args.shot_every == 0)

    def keys(self):
        m = 0
        for start, end, mask in self.presses:
            if start <= self.frame < end:
                m |= mask
        return m

    def on_read(self, uc, access, addr, size, value, user):
        reg = (addr - IO_BASE) // 4
        if reg == 2:
            val = self.keys()
        elif reg == 13:  # capture this frame?
            val = 1 if self.frame in self.captures else 0
        elif reg == 15:  # dump the world maps?
            val = 1 if self.args.dump_maps else 0
        elif reg == 16:  # was the last frame late? (Frame Skip: Auto)
            val = 1 if self.args.late_every and self.frame % self.args.late_every == 0 else 0
        elif reg == 22:  # start a GBA song this frame? (--song FRAME:N)
            val = self.songs.get(self.frame, 0)
        elif reg == 21:  # the drawing list's mode (port/core/gpulist.h)
            val = self.args.gpu_list
        elif reg == 14:  # render the second screen this frame?
            val = 1 if self.wants_shot() or self.args.sub_always else 0
        else:
            val = self.regs[reg]
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
        elif reg == 18:  # the size of the next second-screen picture
            self.sub_size = (value >> 16, value & 0xFFFF)
        elif reg == 17:  # the size of the next picture presented
            self.frame_size = (value >> 16, value & 0xFFFF)
        elif reg == 1:  # present
            w, h = getattr(self, 'frame_size', (256, 192))
            self.last_size = (w, h)
            self.last_frame = bytes(uc.mem_read(value, w * h * 2))
            if getattr(self, 'pending_capture_png', None):
                open(self.pending_capture_png[:-4] + '.frame', 'wb').write(self.last_frame)
                self.pending_capture_png = None
            if self.wants_shot():
                write_png(os.path.join(self.args.out, f'frame_{self.frame:05d}.png'), self.last_frame, *self.last_size)
        elif reg == 3:  # end of frame
            self.frame += 1
            if self.frame >= self.args.frames:
                uc.emu_stop()
        elif reg == 4:  # exit
            self.exit_code = value
            uc.emu_stop()
        elif reg == 7:  # save command
            buf, length = self.regs[5], self.regs[6]
            save = self.save_path()
            if value == 1 and save:
                open(save, 'wb').write(bytes(uc.mem_read(buf, length)))
            elif value == 5:  # Change Current Save Data: FILE, FILE-2, FILE-3
                self.save_slot = length
            elif value == 6:  # data file open (Music Options)
                name = bytes(uc.mem_read(buf, length)).decode('latin-1')
                folder = self.args.data or (os.path.dirname(self.args.save) if self.args.save else '.')
                path = os.path.join(folder, name)
                if name.startswith('/'):  # a path on the SD card: --sd DIR
                    path = self.sd_path(name)
                if name == 'config.ini':  # the settings: --config FILE.ini (else none)
                    path = self.args.config if self.ini_config() else ''
                self.regs[8] = 0
                if os.path.isfile(path):
                    self.files = getattr(self, 'files', {})
                    h = len(self.files) + 1
                    self.files[h] = open(path, 'rb').read()
                    self.regs[8] = h
            elif value == 7:
                d = getattr(self, 'files', {}).get(self.regs[20])
                self.regs[8] = len(d) if d is not None else 0
            elif value == 8:
                d = getattr(self, 'files', {}).get(self.regs[20])
                off = self.regs[19]
                ok = d is not None and off + length <= len(d)
                if ok:
                    uc.mem_write(buf, d[off:off + length])
                self.regs[8] = 1 if ok else 0
            elif value == 9:
                getattr(self, 'files', {}).pop(self.regs[20], None)
            elif value == 10:  # data file write (Music Matching)
                name = bytes(uc.mem_read(self.regs[20], self.regs[19])).decode('latin-1')
                folder = self.args.data or (os.path.dirname(self.args.save) if self.args.save else '.')
                path = os.path.join(folder, name)
                if name == 'config.ini':
                    path = self.args.config if self.ini_config() else None
                if path:
                    open(path, 'wb').write(bytes(uc.mem_read(buf, length)))
                self.regs[8] = 1
            elif value == 11:  # a folder of the SD card (--sd DIR): 128-byte entries, 124-byte name + isDir byte
                name = bytes(uc.mem_read(self.regs[20], self.regs[19])).decode('latin-1')
                path, n = self.sd_path(name), 0
                if os.path.isdir(path):
                    for e in sorted(os.listdir(path)):
                        if n >= length:
                            break
                        raw = e.encode('utf-8')
                        if len(raw) >= 124:
                            continue
                        isdir = os.path.isdir(os.path.join(path, e))
                        uc.mem_write(buf + n * 128, raw.ljust(124, b'\0') + bytes([1 if isdir else 0, 0, 0, 0]))
                        n += 1
                    self.regs[8] = n
                else:
                    self.regs[8] = 0xFFFFFFFF
            elif value == 2:
                ok = 0
                if save and os.path.exists(save):
                    d = open(save, 'rb').read()[:length]
                    uc.mem_write(buf, d)
                    ok = 1
                self.regs[8] = ok
            elif value == 3 and self.args.config and not self.ini_config():
                open(self.args.config, 'wb').write(bytes(uc.mem_read(buf, length)))
            elif value == 4:
                ok = 0
                if self.args.config and not self.ini_config() and os.path.exists(self.args.config):
                    d = open(self.args.config, 'rb').read()[:length]
                    if len(d) > 0:
                        uc.mem_write(buf, d.ljust(length, b'\0'))
                        ok = 1
                self.regs[8] = ok
        elif reg == 13:  # capture finished: dump regs[160][0x400], vram, pltt, oam
            size = 160 * 0x400 + 0x18000 + 0x400 + 0x400
            path = os.path.join(self.args.out, f'capture_{self.frame:05d}.bin')
            open(path, 'wb').write(bytes(uc.mem_read(value, size)))
            # the PNG of the same frame is the reference output
            self.pending_capture_png = path[:-4] + '.png'
        elif reg == 15:  # a world map rendered (512x512)
            self.maps_dumped = getattr(self, 'maps_dumped', 0) + 1
            write_png(os.path.join(self.args.out, f'worldmap{self.maps_dumped}.png'), bytes(uc.mem_read(value, 512 * 512 * 2)), 512, 512)
        elif reg == 14 and value and self.wants_shot():  # second screen rendered (256x192)
            sw, sh = getattr(self, 'sub_size', (256, 192))
            write_png(os.path.join(self.args.out, f'sub_{self.frame:05d}.png'), bytes(uc.mem_read(value, sw * sh * 2)), sw, sh)
        elif reg == 12:  # audio chunk: `value` interleaved 16-bit stereo samples
            if self.frame >= self.args.wav_from:
                self.audio += bytes(uc.mem_read(self.regs[9], value * 4))
            self.audio_rate = self.regs[11]

    def run_profiled(self, start):
        """Statistical profiler: run in slices of N instructions, sample the PC
        after each, and report where time goes per function."""
        import collections
        from unicorn.arm_const import UC_ARM_REG_CPSR
        samples = collections.Counter()
        callers = collections.Counter()
        pcs = collections.Counter()
        per_frame = collections.Counter()
        self.stopped_by_hook = False
        pc = start
        while self.exit_code is None and self.frame < self.args.frames:
            frame_before = self.frame
            self.uc.emu_start(pc, 0xFFFFFFFF, count=self.args.profile)
            pc = self.uc.reg_read(UC_ARM_REG_PC)
            if self.uc.reg_read(UC_ARM_REG_CPSR) & 0x20:
                pc |= 1  # resume in Thumb state
            per_frame[self.frame] += 1
            pf = self.args.profile_frames
            if not pf or int(pf.split(':')[0]) <= self.frame < int(pf.split(':')[1]):
                name = where(self.syms, pc & ~1).split('+')[0]
                samples[name] += 1
                pcs[pc & ~1] += 1
                lr = self.uc.reg_read(UC_ARM_REG_LR)
                callers[(name, where(self.syms, lr & ~1).split('+')[0])] += 1
        total = sum(samples.values())
        print(f'\nprofile: {total} samples, {self.args.profile} instructions each')
        for name, n in samples.most_common(30):
            print(f'  {100 * n / total:5.1f}%  {name}')
        if self.args.profile_pcs:
            with open(self.args.profile_pcs, 'w') as f:
                for addr, n in pcs.most_common():
                    f.write(f'{addr:#x} {n}\n')
        print(f'instructions per frame: {total * self.args.profile / max(1, self.frame - self.first_frame):.0f}')
        heavy = sorted((f, n) for f, n in per_frame.items() if n * self.args.profile > 3_000_000)
        if heavy:
            print('frames over 3M instructions:', ', '.join(f'{f}: {n * self.args.profile / 1e6:.1f}M' for f, n in heavy))
        print('top (function <- caller):')
        for (f, c), n in callers.most_common(15):
            print(f'  {100 * n / total:5.1f}%  {f} <- {c}')

    def run(self):
        t0 = time.time()
        start = self.entry
        first = self.frame
        self.first_frame = first
        if self.args.load_state:
            start = self.load_state(self.args.load_state)
            if self.args.reload_code:
                reload_code(self.uc, self.args.elf)
            first = self.frame
            self.args.frames += self.frame  # --frames counts frames to run
        self.first_frame = first
        try:
            if self.args.profile:
                self.run_profiled(start)
            else:
                self.uc.emu_start(start, 0xFFFFFFFF)
        except UcError as e:
            pc = self.uc.reg_read(UC_ARM_REG_PC)
            lr = self.uc.reg_read(UC_ARM_REG_LR)
            print(f'CRASH at frame {self.frame}: {e}\n  pc={pc:#010x} {where(self.syms, pc)}\n'
                  f'  lr={lr:#010x} {where(self.syms, lr)}', flush=True)
            import unicorn.arm_const as c
            regs = [self.uc.reg_read(getattr(c, f'UC_ARM_REG_R{i}')) for i in range(13)]
            sp = self.uc.reg_read(UC_ARM_REG_SP)
            print('  ' + ' '.join(f'r{i}={v:#x}' for i, v in enumerate(regs)) + f' sp={sp:#x}')
            # Return addresses still on the stack, for a rough backtrace.
            text_lo, text_hi = 0x100000, max(v for v, _, _ in self.syms)
            try:
                stack = self.uc.mem_read(sp, 0x400)
                seen = 0
                for i in range(0, len(stack), 4):
                    v, = struct.unpack_from('<I', stack, i)
                    if text_lo <= v < text_hi and v & 1 == 0:
                        print(f'  stack+{i:#05x}: {v:#010x} {where(self.syms, v)}')
                        seen += 1
                        if seen >= 12:
                            break
            except UcError:
                pass
            if self.last_frame:
                write_png(os.path.join(self.args.out, 'crash_last_frame.png'), self.last_frame, *self.last_size)
            return 2
        dt = time.time() - t0
        ran = self.frame - first
        print(f'ran {ran} frames (now at frame {self.frame}) in {dt:.1f}s ({ran / max(dt, 1e-6):.1f} fps emulated)')
        if self.args.save_state:
            self.save_state(self.args.save_state)
            print(f'state saved to {self.args.save_state} at frame {self.frame}')
        if self.last_frame:
            write_png(os.path.join(self.args.out, 'last_frame.png'), self.last_frame, *self.last_size)
        if self.args.wav and self.audio:
            with wave.open(self.args.wav, 'wb') as w:
                w.setnchannels(2)
                w.setsampwidth(2)
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
    ap.add_argument('--data', help='folder of the data files (pmd-blue.nds); default: the --save folder')
    ap.add_argument('--sd', help='folder standing for the SD card (Music Options finding the DS ROM); default: the data folder')
    ap.add_argument('--config', help='the port settings file: FILE.ini (config.ini), or the older config.bin')
    ap.add_argument('--wav')
    ap.add_argument('--song', action='append', default=[], help='FRAME:N: stop the GBA music and start song N (loudness checks)')
    ap.add_argument('--wav-from', type=int, default=0, help='keep only the audio from this frame on')
    ap.add_argument('--save-state')
    ap.add_argument('--load-state')
    ap.add_argument('--reload-code', action='store_true', help='after --load-state, the ELF\'s code and constant data again (edits that move nothing)')
    ap.add_argument('--capture', type=int, action='append', default=[], help='capture this frame for renderer tests')
    ap.add_argument('--dump-maps', action='store_true', help='write both world maps as PNGs at startup')
    ap.add_argument('--trace', action='append', default=[], help='log calls to this function (frame, r0-r3, lr)')
    ap.add_argument('--watch', action='append', default=[], help='log writes to SYMBOL+OFFSET[:LEN]')
    ap.add_argument('--late-every', type=int, default=0, help='report every Nth frame as late (Frame Skip: Auto)')
    ap.add_argument('--gpu-list', type=int, default=0, help='drawing list: 1 draw frames from it, 2 also check them against the renderer')
    ap.add_argument('--sub-always', action='store_true', help='render the second screen every frame (as on the 3DS)')
    ap.add_argument('--profile-pcs', help='write the sampled PCs here (address count)')
    ap.add_argument('--profile-frames', help='A:B: count only the samples in frames A..B-1')
    ap.add_argument('--profile', type=int, default=0, help='sample the PC every N instructions')
    return Harness(ap.parse_args()).run()


if __name__ == '__main__':
    sys.exit(main())
