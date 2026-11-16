#!/usr/bin/env python3
"""Title menu backgrounds (port_view.h: gPortMenuPics) from PNGs.

    menu_bgs.py IN_DIR OUT.bin

IN_DIR holds ds/, fit/ and 3ds/, each with the same pictures (1.png, 2.png,
... in that order), at 256x192, 320x240 and 400x240. OUT.bin: a little-endian
u32 count, then every DS picture, every Fit one, every 3DS one, each BGR555
a row at a time, as the game's palettes make a colour at full brightness
(each 8-bit channel * 31 / 256).

The pictures may be made from the game's art: like the ROM's data, they go
into the personal build only (build/, never the repository).
"""
import os, re, struct, sys
from PIL import Image

SIZES = [('ds', 256, 192), ('fit', 320, 240), ('3ds', 400, 240)]


def pictures(folder):
    names = [n for n in os.listdir(folder) if n.lower().endswith('.png')]
    return sorted(names, key=lambda n: [int(t) if t.isdigit() else t for t in re.split(r'(\d+)', n)])


def main():
    src, out = sys.argv[1], sys.argv[2]
    lists = [pictures(os.path.join(src, d)) for d, _, _ in SIZES]
    count = len(lists[0])
    if count == 0 or any(len(l) != count for l in lists):
        sys.exit('menu_bgs.py: ds/, fit/ and 3ds/ need the same number of pictures')
    data = bytearray(struct.pack('<I', count))
    for (d, w, h), names in zip(SIZES, lists):
        for n in names:
            im = Image.open(os.path.join(src, d, n)).convert('RGB')
            if im.size != (w, h):
                sys.exit('menu_bgs.py: %s/%s is %dx%d, not %dx%d' % (d, n, im.size[0], im.size[1], w, h))
            px = im.tobytes()
            row = bytearray(w * h * 2)
            for i in range(w * h):
                r, g, b = px[3 * i], px[3 * i + 1], px[3 * i + 2]
                c = (r * 31 // 256) | ((g * 31 // 256) << 5) | ((b * 31 // 256) << 10)
                row[2 * i] = c & 0xFF
                row[2 * i + 1] = c >> 8
            data += row
    with open(out, 'wb') as f:
        f.write(data)
    print('menu_bgs.py: %d pictures of each size, %d bytes' % (count, len(data)))


if __name__ == '__main__':
    main()
