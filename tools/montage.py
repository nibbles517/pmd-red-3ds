#!/usr/bin/env python3
"""Tiles 240x160 harness screenshots into one PNG grid.

usage: montage.py OUT.png COLUMNS IN1.png IN2.png ...
"""
import struct, sys, zlib


def read_png(path):
    d = open(path, 'rb').read()
    i, idat = 8, b''
    while i < len(d):
        n, = struct.unpack('>I', d[i:i + 4])
        if d[i + 4:i + 8] == b'IDAT':
            idat += d[i + 8:i + 8 + n]
        i += 12 + n
    raw = zlib.decompress(idat)
    return [raw[y * 721 + 1:(y + 1) * 721] for y in range(160)]


def main():
    out, cols, paths = sys.argv[1], int(sys.argv[2]), sys.argv[3:]
    imgs = [read_png(p) for p in paths]
    rows_n = (len(imgs) + cols - 1) // cols
    gap = b'\x40\x40\x40' * 4
    blank = [b'\0\0\0' * 240] * 160
    W = 240 * cols + 4 * (cols - 1)
    lines = []
    for gy in range(rows_n):
        tiles = [imgs[gy * cols + gx] if gy * cols + gx < len(imgs) else blank for gx in range(cols)]
        for y in range(160):
            lines.append(b'\0' + gap.join(t[y] for t in tiles))
        if gy < rows_n - 1:
            lines += [b'\0' + b'\x40' * (W * 3)] * 4
    H = len(lines)

    def chunk(t, data):
        return struct.pack('>I', len(data)) + t + data + struct.pack('>I', zlib.crc32(t + data) & 0xFFFFFFFF)
    open(out, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0)) +
                          chunk(b'IDAT', zlib.compress(b''.join(lines))) + chunk(b'IEND', b''))


if __name__ == '__main__':
    main()
