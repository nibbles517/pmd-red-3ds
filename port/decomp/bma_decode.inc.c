// C version of ground_map.c's sub_80A5204, which pmd-red only has as Thumb
// assembly. Written for the 3DS port; verified byte-for-byte against the
// original Thumb code by tools/test_bma_decode.py (fuzzed inputs, run under a
// CPU emulator).
//
// Decodes a ground map's data layer and collision layers into a buffer of
// 256-byte rows. Rows 0-3 and the first 5 columns of every row are a 0xFF
// border. Returns the source pointer just past the consumed data.
//
// Run-length commands (one byte, cmd):
//   data layer:      cmd > 0xBF  literal: next (cmd - 0xBF) bytes
//                    cmd > 0x7F  repeat: next byte, (cmd - 0x7F) times
//                    otherwise   zeros (cmd + 1) times
//   collision layer: cmd > 0x7F  (cmd - 0x7F) set bits
//                    otherwise   skip (cmd + 1) cells
// After the first row, every row is coded relative to the row above: data
// bytes are XORed with it, and collision runs mean "flip" / "same as above".

static u8 *sub_80A5204(void *a, const u8 *src, BmaHeader *hdr, s32 totalRows)
{
    u8 *out = a;
    u8 *p = out;
    const u8 *prev;
    s32 rows, x, y, n, i;
    s32 w = hdr->mapWidthTiles;
    s32 h = hdr->mapHeightTiles;
    u8 cmd, v, bit;
    s32 layer;

    for (rows = 0; rows < 4; rows++) {
        for (i = 0; i < 256; i++)
            *p++ = 0xFF;
    }

    if (hdr->hasDataLayer > 0) {
        for (y = 0; y < h; y++) {
            rows++;
            for (i = 0; i < 5; i++)
                *p++ = 0xFF;

            if (y == 0) {
                for (x = 0; x < w; x += n) {
                    cmd = *src++;
                    if (cmd > 0xBF) {
                        n = cmd - 0xBF;
                        for (i = 0; i < n; i++)
                            *p++ = *src++;
                    }
                    else if (cmd > 0x7F) {
                        v = *src++;
                        n = cmd - 0x7F;
                        for (i = 0; i < n; i++)
                            *p++ = v;
                    }
                    else {
                        n = cmd + 1;
                        for (i = 0; i < n; i++)
                            *p++ = 0;
                    }
                }
            }
            else {
                prev = p - 256;
                for (x = 0; x < w; x += n) {
                    cmd = *src++;
                    if (cmd > 0xBF) {
                        n = cmd - 0xBF;
                        for (i = 0; i < n; i++)
                            *p++ = *src++ ^ *prev++;
                    }
                    else if (cmd > 0x7F) {
                        v = *src++;
                        n = cmd - 0x7F;
                        for (i = 0; i < n; i++)
                            *p++ = v ^ *prev++;
                    }
                    else {
                        n = cmd + 1;
                        for (i = 0; i < n; i++)
                            *p++ = *prev++;
                    }
                }
            }

            // Padding is counted from the nominal width, not from how many
            // bytes the commands actually wrote (matches the original).
            for (x = 5 + w; x <= 0xFF; x++)
                *p++ = 0xFF;
        }
    }
    else {
        for (y = 0; y < h; y++) {
            rows++;
            for (i = 0; i < 5; i++)
                *p++ = 0xFF;
            for (i = 0; i < w; i++)
                *p++ = 0;
            for (x = 5 + w; x <= 0xFF; x++)
                *p++ = 0xFF;
        }
    }

    for (; rows < totalRows; rows++) {
        for (i = 0; i < 256; i++)
            *p++ = 0xFF;
    }

    // Collision layers OR bit 7 (first layer) and bit 6 (second layer) into
    // the cells written above, starting again from row 4.
    for (layer = 0; layer < 2; layer++) {
        if (hdr->hasCollision <= layer)
            break;

        bit = (layer == 0) ? 0x80 : 0x40;
        p = out + 0x400;
        for (y = 0; y < h; y++) {
            p += 5;
            if (y == 0) {
                for (x = 0; x < w; x += n) {
                    cmd = *src++;
                    if (cmd > 0x7F) {
                        n = cmd - 0x7F;
                        for (i = 0; i < n; i++)
                            *p++ |= bit;
                    }
                    else {
                        n = cmd + 1;
                        p += n;
                    }
                }
            }
            else {
                prev = p - 256;
                for (x = 0; x < w; x += n) {
                    cmd = *src++;
                    if (cmd > 0x7F) {
                        n = cmd - 0x7F;
                        for (i = 0; i < n; i++, p++, prev++) {
                            if (!(*prev & bit))
                                *p |= bit;
                        }
                    }
                    else {
                        n = cmd + 1;
                        for (i = 0; i < n; i++, p++, prev++) {
                            if (*prev & bit)
                                *p |= bit;
                        }
                    }
                }
            }
            p += 0xFB - w;
        }
    }

    return (u8 *)src;
}
