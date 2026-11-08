// Background cache (see bgcache.h).

#include <string.h>
#include "port.h"
#include "bgcache.h"

#define TRANSPARENT 0x8000u
#define EDGE_TILE 0xF279 // as ppu.c
#define PAL16 ((const uint16_t *)gPortPltt)
#define HAS_ZERO_NIBBLE(v) ((((v) - 0x11111111u) & ~(v) & 0x88888888u) != 0)
#define HAS_ZERO_BYTE(v) ((((v) - 0x01010101u) & ~(v) & 0x80808080u) != 0)

BgCache gBgCache[4];

static uint32_t sCharCopy[0x10000 / 4];  // VRAM's tile data at the last Prepare
static uint8_t sCharChanged[0x10000 / 32]; // per 32 bytes: changed since then
static uint16_t sPalCopy[256];           // the background palettes, likewise
static int sCharFirst, sCharLast;        // the first and last changed 32 bytes (none: first > last)

// A byte of 16-colour tile data (two pixels) as their two colour numbers
// (0, or the bank's), for each bank
static uint16_t sNibPair[16][256];
static int sNibPairReady;
static void MakeNibPair(void)
{
    for (int bank = 0; bank < 16; bank++)
        for (int b = 0; b < 256; b++) {
            unsigned n0 = b & 15, n1 = b >> 4;
            sNibPair[bank][b] = (uint16_t)((n0 ? bank << 4 | n0 : 0) | ((n1 ? bank << 4 | n1 : 0) << 8));
        }
    sNibPairReady = 1;
}

// The map entry of tile (tx, ty), or -1 for one outside VRAM (drawn clear).
static inline int MapEntry(const BgCache *c, int tx, int ty)
{
    if (c->wideMap)
        return c->wideMap[ty * 64 + tx];
    unsigned block = ((ty >= 32) ? ((c->tilesW == 64) ? 2 : 1) : 0) + ((tx >= 32) ? 1 : 0);
    unsigned off = c->screenBase + block * 0x800 + (ty & 31) * 64 + (tx & 31) * 2;
    if (off + 1 >= 0x10000)
        return -1;
    return *(const uint16_t *)&gPortVram[off];
}

void BgCache_Decode(BgCache *c, int tx, int ty)
{
    int e = MapEntry(c, tx, ty);
    uint16_t *dst = &c->px[ty * 8 * BGC_STRIDE + tx * 8];
    uint8_t *dsti = &c->idx[ty * 8 * BGC_STRIDE + tx * 8];
    const uint16_t *table = c->table;
    unsigned rows = 0; // 2 bits per row: 0 clear, 1 some, 2 opaque
    unsigned dPx = 0, dIdx = 0; // (whether the tile's pixels, colour numbers changed: the GPU's texture)

    if (e < 0 || (c->wideMap && e == PORT_WIDE_BLACK)) {
        uint16_t v = e < 0 ? TRANSPARENT : 0; // (the wide map's black: opaque)
        uint8_t n = e < 0 ? 0 : 0x10;
        for (int r = 0; r < 8; r++)
            for (int i = 0; i < 8; i++) {
                dPx |= dst[r * BGC_STRIDE + i] ^ v;
                dIdx |= dsti[r * BGC_STRIDE + i] ^ n;
                dst[r * BGC_STRIDE + i] = v;
                dsti[r * BGC_STRIDE + i] = n;
            }
        rows = e < 0 ? 0 : 0xAAAA;
    }
    else {
        uint16_t me = (uint16_t)e;
        if (c->edge && me == EDGE_TILE)
            me = 0;
        unsigned tile = me & 0x3FF;
        int hflip = (me & 0x400) != 0;
        // A row at a time: its 8 colour numbers as two words (pixels 0-3,
        // 4-7, in the screen's order), then their colours
        unsigned bank = (me >> 12) << 4;
        int vflip = (me & 0x800) != 0, cmpPx = !c->indexOk; // (the pixels only matter to the GPU without indexOk)
        const int color256 = c->color256;
        const unsigned base = c->charBase + (color256 ? tile * 64 : tile * 32);
        const uint16_t *pair = sNibPair[bank >> 4]; // (16 colours: each byte, two pixels)
        const uint32_t *t32 = c->table32;
        for (int r = 0; r < 8; r++) {
            uint16_t *px = &dst[r * BGC_STRIDE];
            uint8_t *pi = &dsti[r * BGC_STRIDE];
            int ty8 = vflip ? 7 - r : r;
            unsigned st = 0;
            uint32_t lo, hi;
            if (color256) {
                unsigned a = base + ty8 * 8;
                lo = 0, hi = 0;
                if (a < 0x10000) {
                    lo = *(const uint32_t *)&gPortVram[a];
                    hi = *(const uint32_t *)&gPortVram[a + 4];
                }
                if (lo | hi)
                    st = (HAS_ZERO_BYTE(lo) || HAS_ZERO_BYTE(hi)) ? 1 : 2;
            }
            else {
                unsigned a = base + ty8 * 4;
                uint32_t row = (a < 0x10000) ? *(const uint32_t *)&gPortVram[a] : 0;
                if (row)
                    st = HAS_ZERO_NIBBLE(row) ? 1 : 2;
                lo = pair[row & 0xFF] | ((uint32_t)pair[(row >> 8) & 0xFF] << 16);
                hi = pair[(row >> 16) & 0xFF] | ((uint32_t)pair[row >> 24] << 16);
            }
            if (hflip) {
                uint32_t t = __builtin_bswap32(lo);
                lo = __builtin_bswap32(hi);
                hi = t;
            }
            uint32_t o0, o1;
            memcpy(&o0, pi, 4);
            memcpy(&o1, pi + 4, 4);
            dIdx |= (o0 ^ lo) | (o1 ^ hi);
            memcpy(pi, &lo, 4);
            memcpy(pi + 4, &hi, 4);
            uint32_t p0 = t32[lo & 0xFF] | (t32[(lo >> 8) & 0xFF] << 16);
            uint32_t p1 = t32[(lo >> 16) & 0xFF] | (t32[lo >> 24] << 16);
            uint32_t p2 = t32[hi & 0xFF] | (t32[(hi >> 8) & 0xFF] << 16);
            uint32_t p3 = t32[(hi >> 16) & 0xFF] | (t32[hi >> 24] << 16);
            if (cmpPx) {
                uint32_t q[4];
                memcpy(q, px, 16);
                dPx |= (q[0] ^ p0) | (q[1] ^ p1) | (q[2] ^ p2) | (q[3] ^ p3);
            }
            memcpy(px, &p0, 4);
            memcpy(px + 2, &p1, 4);
            memcpy(px + 4, &p2, 4);
            memcpy(px + 6, &p3, 4);
            rows |= st << (r * 2);
        }
    }
    // The row bits
    int w = tx >> 5;
    uint32_t bit = 1u << (tx & 31);
    for (int r = 0; r < 8; r++) {
        unsigned st = (rows >> (r * 2)) & 3;
        int y = ty * 8 + r;
        if (st) c->any[y][w] |= bit;
        else c->any[y][w] &= ~bit;
        if (st == 2) c->full[y][w] |= bit;
        else c->full[y][w] &= ~bit;
    }
    c->entry[ty * 64 + tx] = (uint16_t)e;
    c->recolor[ty][w] &= ~bit;
    c->todo[ty][w] &= ~bit;
    // The GPU's texture: made from the colour numbers (indexOk), else the
    // pixels; a tile decoded again as it was (an animation rewriting the
    // tiles or map with what's there) isn't sent again
    if (c->indexOk ? dIdx : dPx) {
        c->gpuDirty[ty][w] |= bit;
        c->gpuAny = 1;
    }
}

void BgCache_Recolor(BgCache *c, int tx, int ty)
{
    uint16_t *dst = &c->px[ty * 8 * BGC_STRIDE + tx * 8];
    const uint8_t *src = &c->idx[ty * 8 * BGC_STRIDE + tx * 8];
    const uint32_t *t32 = c->table32;
    for (int r = 0; r < 8; r++, dst += BGC_STRIDE, src += BGC_STRIDE) {
        uint32_t lo, hi;
        memcpy(&lo, src, 4);
        memcpy(&hi, src + 4, 4);
        uint32_t p0 = t32[lo & 0xFF] | (t32[(lo >> 8) & 0xFF] << 16);
        uint32_t p1 = t32[(lo >> 16) & 0xFF] | (t32[lo >> 24] << 16);
        uint32_t p2 = t32[hi & 0xFF] | (t32[(hi >> 8) & 0xFF] << 16);
        uint32_t p3 = t32[(hi >> 16) & 0xFF] | (t32[hi >> 24] << 16);
        memcpy(dst, &p0, 4);
        memcpy(dst + 2, &p1, 4);
        memcpy(dst + 4, &p2, 4);
        memcpy(dst + 6, &p3, 4);
    }
    c->recolor[ty][tx >> 5] &= ~(1u << (tx & 31));
    if (!c->indexOk) { // (the GPU's textures: from the colour numbers, but for these)
        c->gpuDirty[ty][tx >> 5] |= 1u << (tx & 31);
        c->gpuAny = 1;
    }
}

// The colour of each colour number, from the palettes as they are now
static void BuildTable(BgCache *c)
{
    for (int n = 0; n < 256; n++)
        c->table[n] = PAL16[n] & 0x7FFF;
    if (c->color256) {
        c->table[0] = TRANSPARENT;
    }
    else {
        for (int n = 0; n < 256; n += 16)
            c->table[n] = TRANSPARENT;
        c->table[0x10] = 0; // (the wide map's black)
    }
    for (int n = 0; n < 256; n++)
        c->table32[n] = c->table[n];
}

// Tile data or palette of entry e changed since the last Prepare.
static inline int EntryChanged(const BgCache *c, unsigned e, unsigned palChanged)
{
    if (c->wideMap && e == PORT_WIDE_BLACK)
        return 0;
    if (c->edge && e == EDGE_TILE)
        e = 0;
    unsigned tile = e & 0x3FF;
    if (c->indexOk)
        palChanged = 0; // (only the tile data matters: the colours are looked up again)
    if (c->color256) {
        unsigned a = c->charBase + tile * 64;
        return palChanged != 0 || (a < 0x10000 && (sCharChanged[a >> 5] | sCharChanged[(a >> 5) + 1]));
    }
    unsigned a = c->charBase + tile * 32;
    return ((palChanged >> (e >> 12)) & 1) || (a < 0x10000 && sCharChanged[a >> 5]);
}

void BgCache_Prepare(const uint8_t *regs, int wide)
{
    if (!sNibPairReady)
        MakeNibPair();
    // What changed in VRAM's tile data and the palettes since the last time
    const uint32_t *v = (const uint32_t *)gPortVram;
    uint32_t *copy = sCharCopy;
    sCharFirst = 0x10000 / 32;
    sCharLast = -1;
    for (int b = 0; b < 0x10000 / 32; b++, v += 8, copy += 8) {
        if (((v[0] ^ copy[0]) | (v[1] ^ copy[1]) | (v[2] ^ copy[2]) | (v[3] ^ copy[3])
             | (v[4] ^ copy[4]) | (v[5] ^ copy[5]) | (v[6] ^ copy[6]) | (v[7] ^ copy[7])) != 0) {
            memcpy(copy, v, 32);
            sCharChanged[b] = 1;
            if (sCharFirst > b) sCharFirst = b;
            sCharLast = b;
        }
        else {
            sCharChanged[b] = 0;
        }
    }
    unsigned palChanged = 0;
    for (int bank = 0; bank < 16; bank++) {
        if (memcmp(&sPalCopy[bank * 16], &PAL16[bank * 16], 32) != 0) {
            memcpy(&sPalCopy[bank * 16], &PAL16[bank * 16], 32);
            palChanged |= 1u << bank;
        }
    }

    uint16_t dispcnt = (uint16_t)(regs[0] | (regs[1] << 8));
    int mode = dispcnt & 7;
    for (int bg = 0; bg < 4; bg++) {
        BgCache *c = &gBgCache[bg];
        uint16_t cnt = (uint16_t)(regs[0x08 + bg * 2] | (regs[0x09 + bg * 2] << 8));
        int text = (mode == 0 || (mode == 1 && bg < 2)) && (dispcnt & (0x100 << bg)) && !(cnt & 0x40);
        if (text) {
            // The settings the lines will look for (ppu.c: RenderWideBg, RenderTextBgVram)
            BgCache want;
            want.charBase = ((cnt >> 2) & 3) * 0x4000;
            want.color256 = (cnt >> 7) & 1;
            if (wide && gPortWideBg[bg].map) {
                want.wideMap = gPortWideBg[bg].map;
                want.screenBase = 0;
                want.size = 3;
                want.edge = 0;
            }
            else {
                int hofs = (regs[0x10 + bg * 4] | (regs[0x11 + bg * 4] << 8)) & 0x1FF;
                int vofs = (regs[0x12 + bg * 4] | (regs[0x13 + bg * 4] << 8)) & 0x1FF;
                want.wideMap = NULL;
                want.screenBase = ((cnt >> 8) & 0x1F) * 0x800;
                want.size = (cnt >> 14) & 3;
                // (gPortTileBg: drawn as in the GBA view, while unscrolled)
                want.edge = wide && !(gPortTileBg[bg] && hofs == 0 && vofs == 0);
            }
            if (!c->valid || c->wideMap != want.wideMap || c->charBase != want.charBase
                || c->screenBase != want.screenBase || c->color256 != want.color256
                || c->size != want.size || c->edge != want.edge) {
                c->valid = 1;
                c->wideMap = want.wideMap;
                c->charBase = want.charBase;
                c->screenBase = want.screenBase;
                c->color256 = want.color256;
                c->size = want.size;
                c->edge = want.edge;
                c->tilesW = (want.size & 1) ? 64 : 32;
                c->tilesH = (want.size & 2) ? 64 : 32;
                c->indexOk = !(want.color256 && want.wideMap);
                c->byIndex = 0;
                c->catchUp = 0;
                BuildTable(c);
                memset(c->todo, 0xFF, sizeof(c->todo));
                memset(c->recolor, 0, sizeof(c->recolor));
                memset(c->gpuDirty, 0xFF, sizeof(c->gpuDirty)); // (whatever Decode finds)
                c->gpuAny = 1;
                continue;
            }
        }
        if (!c->valid)
            continue;
        // Mark the tiles whose entry, tile data or palette changed. (A
        // background off this frame is kept up too: what changed now is
        // not seen again later.) Unless some of its tile data or palettes
        // changed, only the entries need comparing, two at a time.
        int lastBlock = (int)((c->charBase + (c->color256 ? 0x10000 : 0x8000)) >> 5) - 1;
        // The palettes changed: the colours of the tiles using them are made
        // again from their numbers (4bpp: the tiles of the banks that changed;
        // 256 colours: all), not decoded again.
        // Most of them (the game's fades: all, every frame): no colours made
        // this frame, the lines look them up from the numbers instead.
        int palette = palChanged != 0;
        int fade = palette && c->indexOk && (c->color256 || __builtin_popcount(palChanged) >= 8);
        if (fade) {
            c->byIndex = 1;
            c->catchUp = 0;
            // (after it, the rows on the screen are done first)
            int top = c->wideMap ? gPortWideBg[bg].scrollY
                                 : (regs[0x12 + bg * 4] | (regs[0x13 + bg * 4] << 8));
            c->catchStart = ((top & 511) >> 3) & (c->tilesH - 1);
        }
        int byBank = palette && c->indexOk && !fade; // (per tile: its bank)
        if (palette) {
            BuildTable(c);
            if (fade || (c->indexOk && c->color256))
                memset(c->recolor, 0xFF, sizeof(c->recolor));
        }
        int slow = (palette && !c->indexOk)
                   || (sCharFirst <= lastBlock && sCharLast >= (int)(c->charBase >> 5));
        for (int ty = 0; ty < c->tilesH; ty++) {
            uint32_t *todo = c->todo[ty], *recolor = c->recolor[ty];
            const uint16_t *entry = &c->entry[ty * 64];
            for (int tx = 0; tx < c->tilesW; tx += 2) {
                int e0, e1;
                if (c->wideMap) {
                    e0 = c->wideMap[ty * 64 + tx];
                    e1 = c->wideMap[ty * 64 + tx + 1];
                }
                else {
                    unsigned block = ((ty >= 32) ? ((c->tilesW == 64) ? 2 : 1) : 0) + ((tx >= 32) ? 1 : 0);
                    unsigned off = c->screenBase + block * 0x800 + (ty & 31) * 64 + (tx & 31) * 2;
                    if (off >= 0x10000)
                        continue; // (outside VRAM: stays clear)
                    uint32_t two = *(const uint32_t *)&gPortVram[off];
                    if (!slow && !byBank && two == *(const uint32_t *)&entry[tx])
                        continue;
                    e0 = two & 0xFFFF;
                    e1 = two >> 16;
                }
                if (!slow && !byBank && e0 == entry[tx] && e1 == entry[tx + 1])
                    continue;
                uint32_t bit = 1u << (tx & 31);
                uint32_t *w = &todo[tx >> 5];
                if (!(*w & bit) && (e0 != entry[tx] || (slow && EntryChanged(c, (unsigned)e0, palChanged))))
                    *w |= bit;
                if (!(*w & (bit << 1)) && (e1 != entry[tx + 1] || (slow && EntryChanged(c, (unsigned)e1, palChanged))))
                    *w |= bit << 1;
                if (byBank) {
                    if (!(*w & bit) && ((palChanged >> (entry[tx] >> 12)) & 1))
                        recolor[tx >> 5] |= bit;
                    if (!(*w & (bit << 1)) && ((palChanged >> (entry[tx + 1] >> 12)) & 1))
                        recolor[tx >> 5] |= bit << 1;
                }
            }
        }
        // After a fade: the colours made again a few tile rows a frame (not
        // all on the first frame after it); the lines look them up until then.
        if (c->byIndex && !fade) {
            int budget = 128; // tiles a frame (no more than a fade frame costs)
            while (budget > 0 && c->catchUp < c->tilesH) {
                int ty = (c->catchStart + c->catchUp++) & (c->tilesH - 1);
                for (int w = 0; w < 2; w++) {
                    uint32_t stale = w * 32 < c->tilesW ? c->recolor[ty][w] & ~c->todo[ty][w] : 0;
                    while (stale) {
                        int b = __builtin_ctz(stale);
                        stale &= stale - 1;
                        BgCache_Recolor(c, w * 32 + b, ty);
                        budget--;
                    }
                    c->recolor[ty][w] = 0; // (tiles still to decode get their colours then)
                }
            }
            if (c->catchUp >= c->tilesH)
                c->byIndex = 0;
        }
    }
}
