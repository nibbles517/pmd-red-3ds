#!/usr/bin/env python3
"""Differential test for port/decomp/bma_decode.inc.c.

Runs pmd-red's original Thumb sub_80A5204 (taken from a matching pmd_red.elf
built from your ROM) under the Unicorn CPU emulator, and the port's C version
compiled for the host, on the same fuzzed inputs. Every output byte and the
returned source pointer must match.

usage: test_bma_decode.py UPSTREAM_DIR [iterations]
needs: libunicorn (LIBUNICORN_PATH) and its Python bindings on PYTHONPATH
"""
import ctypes, os, random, struct, subprocess, sys, tempfile

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, \
    UC_ARM_REG_SP, UC_ARM_REG_LR

HERE = os.path.dirname(os.path.abspath(__file__))
C_SRC = os.path.join(HERE, '..', 'port', 'decomp', 'bma_decode.inc.c')

CODE, SRC, HDR, OUT, STACK, RET = 0x08000000, 0x02000000, 0x02100000, 0x03000000, 0x04000000, 0x05000000
OUT_SIZE = 0x40000
SRC_SIZE = 0x40000


def original_code(upstream):
    elf = os.path.join(upstream, 'pmd_red.elf')
    nm = subprocess.check_output(['arm-none-eabi-nm', elf], text=True)
    addr = next(int(l.split()[0], 16) for l in nm.splitlines() if l.endswith(' sub_80A5204'))
    rom = open(os.path.join(upstream, 'pmd_red.gba'), 'rb').read()
    start = addr - 0x08000000
    return addr, rom[start:start + 0x500]  # function is ~0x490 bytes


def host_lib():
    tmp = tempfile.mkdtemp()
    wrapper = os.path.join(tmp, 'w.c')
    with open(wrapper, 'w') as f:
        f.write('#include <stdint.h>\ntypedef uint8_t u8; typedef uint16_t u16; typedef int16_t s16; typedef int32_t s32;\n'
                'typedef struct { u8 mapWidthTiles, mapHeightTiles, tilingWidth, tilingHeight, mapWidthChunks, mapHeightChunks;'
                ' u16 numLayers; s16 hasDataLayer; s16 hasCollision; } BmaHeader;\n'
                f'#include "{os.path.abspath(C_SRC)}"\n'
                'u8 *bma_decode(void *a, const u8 *b, BmaHeader *c, s32 d) { return sub_80A5204(a, b, c, d); }\n')
    so = os.path.join(tmp, 'w.so')
    subprocess.check_call(['gcc', '-O2', '-shared', '-fPIC', '-Wall', '-o', so, wrapper])
    lib = ctypes.CDLL(so)
    lib.bma_decode.restype = ctypes.c_void_p
    lib.bma_decode.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int32]
    return lib


def gen_case(rng):
    w = rng.choice([0, 1, 2, rng.randint(1, 60), rng.randint(60, 200)])
    h = rng.choice([0, 1, 2, rng.randint(1, 40), rng.randint(40, 120)])
    data = rng.choice([0, 1])
    coll = rng.choice([0, 1, 2])
    total = rng.randint(4 + h, 4 + h + 20)
    # Mostly well-formed streams; sometimes commands overshoot the row width,
    # which the original tolerates, so the C version must too.
    src = bytes(rng.randint(0, 255) for _ in range(SRC_SIZE))
    hdr = struct.pack('<6BHhh', w, h, 3, 3, (w + 2) // 3, (h + 2) // 3, 1, data, coll)
    return hdr, src, total


def run_original(addr, code, hdr, src, total):
    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
    for base, size in ((CODE, 0x100000), (SRC, 0x200000), (OUT, 0x100000), (STACK - 0x10000, 0x20000), (RET, 0x1000)):
        uc.mem_map(base, size)
    uc.mem_write(addr, code)
    uc.mem_write(SRC, src)
    uc.mem_write(HDR, hdr)
    uc.reg_write(UC_ARM_REG_R0, OUT)
    uc.reg_write(UC_ARM_REG_R1, SRC)
    uc.reg_write(UC_ARM_REG_R2, HDR)
    uc.reg_write(UC_ARM_REG_R3, total)
    uc.reg_write(UC_ARM_REG_SP, STACK)
    uc.reg_write(UC_ARM_REG_LR, RET | 1)
    uc.emu_start(addr | 1, RET, count=50_000_000)
    return bytes(uc.mem_read(OUT, OUT_SIZE)), uc.reg_read(UC_ARM_REG_R0) - SRC


def run_c(lib, hdr, src, total):
    out = ctypes.create_string_buffer(OUT_SIZE)
    s = ctypes.create_string_buffer(src, SRC_SIZE)
    h = ctypes.create_string_buffer(hdr, len(hdr))
    end = lib.bma_decode(out, s, h, total)
    return out.raw, end - ctypes.addressof(s)


def main():
    upstream = sys.argv[1]
    iters = int(sys.argv[2]) if len(sys.argv) > 2 else 300
    addr, code = original_code(upstream)
    lib = host_lib()
    rng = random.Random(1234)
    for i in range(iters):
        hdr, src, total = gen_case(rng)
        a_out, a_end = run_original(addr, code, hdr, src, total)
        b_out, b_end = run_c(lib, hdr, src, total)
        if a_out != b_out or a_end != b_end:
            diff = next((j for j in range(OUT_SIZE) if a_out[j] != b_out[j]), None)
            w, h = hdr[0], hdr[1]
            print(f'MISMATCH case {i}: w={w} h={h} data={struct.unpack("<h", hdr[8:10])[0]} '
                  f'coll={struct.unpack("<h", hdr[10:12])[0]} total={total} '
                  f'end {a_end:#x} vs {b_end:#x} first byte diff at {diff}')
            return 1
    print(f'OK: {iters} fuzzed cases match the original Thumb code byte for byte')
    return 0


if __name__ == '__main__':
    sys.exit(main())
