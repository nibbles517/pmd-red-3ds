// Scanline renderer for GBA video state held in shadow memory.
//
// Renders one 240-pixel line at a time from the shadow registers, VRAM, palette
// and OAM, so mid-frame register changes (pmd-red's VCount callbacks and
// HBlank DMA) take effect on exactly the lines they do on hardware.
//
// Output is GBA BGR555 (bit 15 clear). Covers modes 0-2 (text and affine
// backgrounds), mode 3, regular and affine sprites, windows 0/1 and the object
// window, alpha blending, brighten/darken, and mosaic.
//
// Wide view (Ppu_SetView): the line is SCREEN_W (256 or 320) pixels, GBA x 0 drawn at
// screen x WIDE_OX, and lines past 159 are drawn too (up to SCREEN_H). The game
// only knows a 240x160 screen, so window edges at the screen's edges are
// stretched to the new edges, and backgrounds the game draws for the larger
// view (gPortWideBg) are read from wider maps kept outside VRAM.
//
// Speed matters here: this runs 160 times a frame on a 268 MHz CPU. Common
// cases take fast paths (whole tile rows decoded at once, layers painted back
// to front when no windows or alpha blending are active); everything else goes
// through the general per-pixel path. Text backgrounds are copied out of
// their maps kept decoded (bgcache.h) rather than decoded line by line.
// port/test/ppu_ref.c is the original
// straightforward version, and tools/test_ppu checks this file against it,
// byte for byte, on frames captured from the game.

#include <stdint.h>
#include <string.h>
#include "port.h"
#include "bgcache.h"

// Dual Core: ppu_b.c builds this file a second time, with its own state,
// for the second CPU core to draw part of the lines with (Ppu* -> PpuB*).
#ifdef PPU_SECOND
#define Ppu_BeginFrame PpuB_BeginFrame
#define Ppu_RenderLine PpuB_RenderLine
#define Ppu_SkipLine PpuB_SkipLine
#define Ppu_SetView PpuB_SetView
#define Ppu_SetLineRegs PpuB_SetLineRegs
#endif

#define W SCREEN_W_MAX // buffer size; the line drawn is sW pixels
static int sW = GBA_W, sOx = 0, sWide = 0;
// The wide view's size (Ppu_SetView: SCREEN_W x SCREEN_H as they were then)
static int sViewW = 256, sViewH = 192, sViewOx = 8;
// Pixels per group (below): 8 (1 << 3), or 16 for lines over 256 pixels
static int sGs = 3;
// Sprite x (9 bits, GBA screen x) from which it counts as negative: 256, or
// 320 when the wide view reaches past GBA x 256 (Fit: up to 280)
static int sObjXWrap = 256;
#ifndef PPU_SECOND
struct PortWideBg gPortWideBg[4];
uint8_t gPortTileBg[4];
uint8_t gPortHudLines, gPortHudLeft;
uint32_t gPortHudBar[PORT_HUD_BAR_MAX]; // (PORT_HUD_BAR_MAX / 8 tiles of 8 rows)
#endif

// The I/O registers the renderer reads: the live ones, or (Dual Core) a
// copy of them made as the line went by (Ppu_SetLineRegs).
static const uint8_t *sRegs = gPortRegs;

void Ppu_SetLineRegs(const uint8_t *regs)
{
    sRegs = regs ? regs : gPortRegs;
}

#define gPortRegs sRegs
#define TILE_OY ((sViewH - GBA_H) / 2)
static int RenderTextBgVram(int bg, int line, uint16_t *out, uint16_t cnt);

void Ppu_SetView(int wide)
{
    sViewW = SCREEN_W;
    sViewH = SCREEN_H;
    sViewOx = WIDE_OX;
    sWide = wide;
    sW = wide ? sViewW : GBA_W;
    sOx = wide ? sViewOx : 0;
    sGs = sW > 256 ? 4 : 3;
    sObjXWrap = sW > 256 ? 320 : 256;
}
#define TRANSPARENT 0x8000u

enum { L_BG0, L_BG1, L_BG2, L_BG3, L_OBJ, L_BD };

// Per-line object buffer.
typedef struct {
    uint16_t color[W];   // BGR555 or TRANSPARENT
    uint8_t prio[W];     // 0-3, 4 = none
    uint8_t semi[W];     // semi-transparent (alpha) sprite pixel
    uint8_t win[W];      // inside the object window
} ObjLine;

static uint16_t sBg[4][W];
// Where each layer's line is read from: sBg, or (a text background out of
// its cache, unwrapped) straight from the cache, not copied (RenderCachedLine).
static const uint16_t *sBgSrc[4];
static const uint16_t *sLastSrc; // ... as the renderer left it
static int sWantPtr;             // the caller takes the line from sLastSrc (and does not change it)
static int sBgAny[4];               // layer has an opaque pixel on this line
// Which 8-pixel groups of the line (bit g: x 8g..8g+7) may hold an opaque
// pixel, per layer: compositing skips the others. Set by the renderers in
// sLastMask (all groups when a renderer does not track them).
static uint32_t sBgMask[4];
static uint32_t sLastMask;
// ... and which groups are opaque in all 8 pixels (0 when not tracked).
static uint32_t sBgFull[4];
static uint32_t sLastFull;
#define ALL_GROUPS 0xFFFFFFFFu

// Takes the next run of consecutive groups out of *m: x from *x0 to *x1
// (exclusive, within the line). Returns 0 when there is none left.
static inline int NextRun(uint32_t *m, int *x0, int *x1)
{
    if (*m == 0)
        return 0;
    int g = __builtin_ctz(*m);
    uint32_t rest = ~(*m >> g);
    int end = rest == 0 ? 32 : g + __builtin_ctz(rest);
    *x0 = g << sGs;
    *x1 = (end << sGs) > sW ? sW : end << sGs;
    *m = end >= 32 ? 0 : *m & (~0u << end);
    return 1;
}

// The groups covering x .. x+n-1 (n >= 1).
static inline uint32_t Groups(int x, int n)
{
    return (2u << ((x + n - 1) >> sGs)) - (1u << (x >> sGs));
}
static ObjLine sObj;
static int sObjAny, sObjSemiAny, sObjWinAny;
static int sObjMinX = W, sObjMaxX = -1; // span sprites touched (this line)
static int sObjDirtyMin = 0, sObjDirtyMax = W - 1; // span to clear (last line)
static unsigned sObjPrioMask;       // priorities present among sprite pixels

// Affine backgrounds keep internal reference points that advance each line.
static int32_t sAffX[2], sAffY[2];
static uint32_t sAffLatchX[2], sAffLatchY[2];

static inline uint16_t R16(unsigned off) { return (uint16_t)(gPortRegs[off] | (gPortRegs[off + 1] << 8)); }
static inline uint32_t R32(unsigned off) { return R16(off) | ((uint32_t)R16(off + 2) << 16); }
static inline uint16_t Pal(unsigned index) { return (uint16_t)((gPortPltt[index * 2] | (gPortPltt[index * 2 + 1] << 8)) & 0x7FFF); }
#define PAL16 ((const uint16_t *)gPortPltt)

static int32_t Sext28(uint32_t v) { return (int32_t)(v << 4) >> 4; }

// The OAM entries that can show this frame (OAM is only written in VBlank).
static uint8_t sObjList[128];
static int sObjCount;

void Ppu_BeginFrame(void)
{
    sObjCount = 0;
    for (int n = 0; n < 128; n++) {
        const uint8_t *o = &gPortOam[n * 8];
        uint16_t a0 = (uint16_t)(o[0] | (o[1] << 8));
        if (!((a0 >> 8) & 1) && (a0 & 0x200))
            continue; // disabled
        if (((a0 >> 14) & 3) == 3 || ((a0 >> 10) & 3) == 3)
            continue; // no such shape / mode
        sObjList[sObjCount++] = (uint8_t)n;
    }
    for (int i = 0; i < 2; i++) {
        sAffLatchX[i] = R32(0x28 + i * 0x10);
        sAffLatchY[i] = R32(0x2C + i * 0x10);
        sAffX[i] = Sext28(sAffLatchX[i]);
        sAffY[i] = Sext28(sAffLatchY[i]);
    }
}

// A write to BGxX/BGxY mid-frame reloads the internal reference point.
static void AffineReloadIfWritten(int i)
{
    uint32_t x = R32(0x28 + i * 0x10), y = R32(0x2C + i * 0x10);
    if (x != sAffLatchX[i]) { sAffLatchX[i] = x; sAffX[i] = Sext28(x); }
    if (y != sAffLatchY[i]) { sAffLatchY[i] = y; sAffY[i] = Sext28(y); }
}

static void AffineAdvance(int i)
{
    sAffX[i] += (int16_t)R16(0x22 + i * 0x10); // PB
    sAffY[i] += (int16_t)R16(0x26 + i * 0x10); // PD
}

// --- Text backgrounds -------------------------------------------------------

// General per-pixel path, used only with mosaic.
static void RenderTextBgMosaic(int bg, int line, uint16_t *out)
{
    uint16_t cnt = R16(0x08 + bg * 2);
    unsigned charBase = ((cnt >> 2) & 3) * 0x4000;
    unsigned screenBase = ((cnt >> 8) & 0x1F) * 0x800;
    int color256 = (cnt >> 7) & 1;
    int size = (cnt >> 14) & 3;
    int widthPx = (size & 1) ? 512 : 256;
    int heightPx = (size & 2) ? 512 : 256;
    int hofs = R16(0x10 + bg * 4) & 0x1FF;
    int vofs = R16(0x12 + bg * 4) & 0x1FF;
    uint16_t m = R16(0x4C);
    int mosaicH = (m & 0xF) + 1;
    int mv = ((m >> 4) & 0xF) + 1;
    int y = line - line % mv;
    y = (y + vofs) & (heightPx - 1);

    int tileRow = y >> 3, inY = y & 7;
    for (int sx = 0; sx < sW; sx++) {
        int gx = sx - sOx;
        int px = gx - (((gx % mosaicH) + mosaicH) % mosaicH);
        int x = (px + hofs) & (widthPx - 1);
        unsigned block = 0;
        if (x >= 256) block += 1;
        if (y >= 256) block += (widthPx == 512) ? 2 : 1;
        unsigned mapOff = screenBase + block * 0x800 + ((tileRow & 31) * 32 + ((x >> 3) & 31)) * 2;
        if (mapOff + 1 >= 0x10000) { out[sx] = TRANSPARENT; continue; }
        uint16_t e = (uint16_t)(gPortVram[mapOff] | (gPortVram[mapOff + 1] << 8));
        int tx = x & 7, ty = inY;
        if (e & 0x400) tx = 7 - tx;
        if (e & 0x800) ty = 7 - ty;
        unsigned tile = e & 0x3FF;
        unsigned idx;
        if (color256) {
            unsigned a = charBase + tile * 64 + ty * 8 + tx;
            idx = (a < 0x10000) ? gPortVram[a] : 0;
            out[sx] = idx ? Pal(idx) : TRANSPARENT;
        }
        else {
            unsigned a = charBase + tile * 32 + ty * 4 + (tx >> 1);
            idx = (a < 0x10000) ? gPortVram[a] : 0;
            idx = (tx & 1) ? (idx >> 4) : (idx & 0xF);
            out[sx] = idx ? Pal((e >> 12) * 16 + idx) : TRANSPARENT;
        }
    }
}

// 16-colour palettes as decoding uses them: entry 0 TRANSPARENT, the others
// without bit 15. Built for a bank the first time a line uses it (palettes
// can change between lines).
static uint16_t sPalT[16][16];
static uint16_t sPalTBuilt; // bit per bank, for this line

static inline const uint16_t *PalT(unsigned bank)
{
    if (!(sPalTBuilt & (1u << bank))) {
        const uint16_t *pal = &PAL16[bank * 16];
        sPalT[bank][0] = TRANSPARENT;
        for (int i = 1; i < 16; i++)
            sPalT[bank][i] = pal[i] & 0x7FFF;
        sPalTBuilt |= (uint16_t)(1u << bank);
    }
    return sPalT[bank];
}

// Decodes one 8-pixel row of the tile named by map entry `e` into px[0..7].
// Returns 0 if all pixels are transparent, 2 if all are opaque, else 1.
#define HAS_ZERO_NIBBLE(v) ((((v) - 0x11111111u) & ~(v) & 0x88888888u) != 0)
#define HAS_ZERO_BYTE(v) ((((v) - 0x01010101u) & ~(v) & 0x80808080u) != 0)
static inline __attribute__((always_inline)) int DecodeTileRow(uint16_t e, unsigned charBase, int inY, int color256, uint16_t *px)
{
    int ty = (e & 0x800) ? 7 - inY : inY;
    unsigned tile = e & 0x3FF;
    int any = 0;
    if (color256) {
        unsigned a = charBase + tile * 64 + ty * 8;
        uint32_t lo = 0, hi = 0;
        if (a < 0x10000) {
            lo = *(const uint32_t *)&gPortVram[a];
            hi = *(const uint32_t *)&gPortVram[a + 4];
        }
        if ((lo | hi) == 0) {
            for (int i = 0; i < 8; i++) px[i] = TRANSPARENT;
            return 0;
        }
        for (int i = 0; i < 8; i++) {
            unsigned idx = (i < 4) ? (lo >> (8 * i)) & 0xFF : (hi >> (8 * (i - 4))) & 0xFF;
            px[i] = idx ? (PAL16[idx] & 0x7FFF) : TRANSPARENT;
        }
        any = (HAS_ZERO_BYTE(lo) || HAS_ZERO_BYTE(hi)) ? 1 : 2;
    }
    else {
        unsigned a = charBase + tile * 32 + ty * 4;
        uint32_t row = (a < 0x10000) ? *(const uint32_t *)&gPortVram[a] : 0; // 4-byte aligned by construction
        if (row == 0) {
            for (int i = 0; i < 8; i++) px[i] = TRANSPARENT;
            return 0;
        }
        const uint16_t *pal = PalT(e >> 12);
        any = HAS_ZERO_NIBBLE(row) ? 1 : 2;
        if (e & 0x400) { // horizontal flip
            px[7] = pal[row & 0xF];         px[6] = pal[(row >> 4) & 0xF];
            px[5] = pal[(row >> 8) & 0xF];  px[4] = pal[(row >> 12) & 0xF];
            px[3] = pal[(row >> 16) & 0xF]; px[2] = pal[(row >> 20) & 0xF];
            px[1] = pal[(row >> 24) & 0xF]; px[0] = pal[row >> 28];
            return any;
        }
        px[0] = pal[row & 0xF];         px[1] = pal[(row >> 4) & 0xF];
        px[2] = pal[(row >> 8) & 0xF];  px[3] = pal[(row >> 12) & 0xF];
        px[4] = pal[(row >> 16) & 0xF]; px[5] = pal[(row >> 20) & 0xF];
        px[6] = pal[(row >> 24) & 0xF]; px[7] = pal[row >> 28];
        return any;
    }
    if (e & 0x400) { // horizontal flip
        for (int i = 0; i < 4; i++) {
            uint16_t t = px[i];
            px[i] = px[7 - i];
            px[7 - i] = t;
        }
    }
    return any;
}

// A line of a background out of its cache (bgcache.h): y the map's row, x the
// map's column at the line's first pixel. Tiles not decoded yet are decoded
// first; a group is marked opaque-somewhere / opaque-all from the row bits of
// the tile columns it covers.
static inline uint64_t CacheRowBits(const uint32_t w[2], int tilesW, int tx0)
{
    // The tile columns from tx0 on, wrapping (a 32-column map: twice over)
    uint64_t v = w[0] | ((uint64_t)(tilesW == 64 ? w[1] : w[0]) << 32);
    return tx0 ? (v >> tx0) | (v << (64 - tx0)) : v;
}

// The even bits of v, packed (bit 2i to bit i).
static inline uint32_t EvenBits(uint64_t v)
{
    v &= 0x5555555555555555ull;
    v = (v | (v >> 1)) & 0x3333333333333333ull;
    v = (v | (v >> 2)) & 0x0F0F0F0F0F0F0F0Full;
    v = (v | (v >> 4)) & 0x00FF00FF00FF00FFull;
    v = (v | (v >> 8)) & 0x0000FFFF0000FFFFull;
    return (uint32_t)(v | (v >> 16));
}

static int RenderCachedLine(BgCache *c, int y, int x, uint16_t *out)
{
    const int wPx = c->tilesW * 8;
    const int tx0 = x >> 3, skip = x & 7, ty = y >> 3;
    const int k = (skip + sW + 7) >> 3; // tile columns the line covers (at most 42)
    const uint64_t cols = (1ull << k) - 1;

    uint64_t todo = CacheRowBits(c->todo[ty], c->tilesW, tx0) & cols;
    while (todo) {
        uint32_t lo32 = (uint32_t)todo; // (the lowest bit: 32-bit ctz, no library call)
        int i = lo32 ? __builtin_ctz(lo32) : 32 + __builtin_ctz((uint32_t)(todo >> 32));
        todo &= todo - 1;
        BgCache_Decode(c, (tx0 + i) & (c->tilesW - 1), ty);
    }
    __sync_synchronize(); // (tiles the other core decoded: theirs before their todo bits)
    uint64_t any = CacheRowBits(c->any[y], c->tilesW, tx0) & cols;
    uint64_t full = CacheRowBits(c->full[y], c->tilesW, tx0) & cols;

    if (c->byIndex && (CacheRowBits(c->recolor[ty], c->tilesW, tx0) & cols)) {
        // the palettes are fading (or just stopped, and these colours are
        // not made yet): the colours looked up from the numbers
        const uint8_t *row = &c->idx[y * BGC_STRIDE];
        const uint16_t *table = c->table;
        for (int sx = 0, cx = x; sx < sW;) {
            int n = wPx - cx;
            if (n > sW - sx) n = sW - sx;
            const uint8_t *s8 = &row[cx];
            uint16_t *d = &out[sx];
            int i = 0;
            for (; i + 4 <= n; i += 4) {
                d[i] = table[s8[i]];
                d[i + 1] = table[s8[i + 1]];
                d[i + 2] = table[s8[i + 2]];
                d[i + 3] = table[s8[i + 3]];
            }
            for (; i < n; i++)
                d[i] = table[s8[i]];
            sx += n;
            cx = 0;
        }
    }
    else {
        uint64_t stale = CacheRowBits(c->recolor[ty], c->tilesW, tx0) & cols;
        while (stale) { // (colours not made since the palettes last changed)
            uint32_t lo32 = (uint32_t)stale;
            int i = lo32 ? __builtin_ctz(lo32) : 32 + __builtin_ctz((uint32_t)(stale >> 32));
            stale &= stale - 1;
            BgCache_Recolor(c, (tx0 + i) & (c->tilesW - 1), ty);
        }
        const uint16_t *row = &c->px[y * BGC_STRIDE];
        if (sWantPtr && x + sW <= wPx) { // (one piece: read where it is)
            sLastSrc = &row[x];
            goto copied;
        }
        for (int sx = 0, cx = x; sx < sW;) {
            int n = wPx - cx;
            if (n > sW - sx) n = sW - sx;
            memcpy(&out[sx], &row[cx], n * sizeof(uint16_t));
            sx += n;
            cx = 0;
        }
    }
copied:;

    uint32_t mask = 0, opaque = 0;
    const int gs = sGs, groups = (sW + (1 << gs) - 1) >> gs;
    if (any) {
        if (gs == 3) { // 8-pixel groups: one or two tile columns each
            uint64_t m = skip ? any | (any >> 1) : any;
            uint64_t f = skip ? full & (full >> 1) : full;
            uint32_t keep = groups >= 32 ? ALL_GROUPS : (1u << groups) - 1;
            mask = (uint32_t)m & keep;
            opaque = (uint32_t)f & keep;
        }
        else if (gs == 4) { // 16-pixel groups: two or three tile columns each
            uint64_t m = skip ? any | (any >> 1) | (any >> 2) : any | (any >> 1);
            uint64_t f = skip ? full & (full >> 1) & (full >> 2) : full & (full >> 1);
            uint32_t keep = groups >= 32 ? ALL_GROUPS : (1u << groups) - 1;
            mask = EvenBits(m) & keep;
            opaque = EvenBits(f) & keep;
        }
        else {
            for (int g = 0; g < groups; g++) {
                int p0 = g << gs, p1 = p0 + (1 << gs);
                if (p1 > sW) p1 = sW;
                int lo = (p0 + skip) >> 3, hi = (p1 - 1 + skip) >> 3;
                uint64_t cm = ((2ull << (hi - lo)) - 1) << lo;
                if (any & cm) mask |= 1u << g;
                if ((full & cm) == cm) opaque |= 1u << g;
            }
        }
    }
    sLastMask = mask;
    sLastFull = opaque;
    return any != 0;
}

// A background the game draws for the wide view: a 64x64-entry map in RAM,
// scrolled by gPortWideBg (not by the BG registers). PORT_WIDE_BLACK entries
// are opaque black (outside the map).
static int RenderWideBg(int bg, int line, uint16_t *out)
{
    const struct PortWideBg *wb = &gPortWideBg[bg];
    uint16_t cnt = R16(0x08 + bg * 2);
    unsigned charBase = ((cnt >> 2) & 3) * 0x4000;
    int color256 = (cnt >> 7) & 1;
    int y = (line + wb->scrollY) & 511;
    int inY = y & 7;
    const uint16_t *row = &wb->map[(y >> 3) * 64];
    int any = 0;
    uint32_t mask = 0, notFull = 0;
    int x = (wb->scrollX - sOx) & 511;
    BgCache *c = &gBgCache[bg];
    if (c->valid && c->wideMap == wb->map && c->charBase == charBase && c->color256 == color256)
        return RenderCachedLine(c, y, x, out);

    for (int sx = 0; sx < sW;) {
        uint16_t e = row[x >> 3];
        int skip = x & 7;
        int n = 8 - skip;
        uint16_t px[8];
        int got;
        if (n > sW - sx) n = sW - sx;
        if (e == PORT_WIDE_BLACK) {
            for (int i = 0; i < n; i++) out[sx + i] = 0;
            any = 1;
            mask |= Groups(sx, n);
        }
        else if (n == 8) {
            if ((got = DecodeTileRow(e, charBase, inY, color256, &out[sx])) != 0) {
                any = 1;
                mask |= Groups(sx, 8);
            }
            if (got != 2)
                notFull |= Groups(sx, 8);
        }
        else if (notFull |= Groups(sx, n), DecodeTileRow(e, charBase, inY, color256, px)) {
            for (int i = 0; i < n; i++) {
                out[sx + i] = px[skip + i];
                any |= !(px[skip + i] & TRANSPARENT);
            }
            mask |= Groups(sx, n);
        }
        else {
            for (int i = 0; i < n; i++) out[sx + i] = TRANSPARENT;
        }
        sx += n;
        x = (x + n) & 511;
    }
    sLastMask = mask;
    sLastFull = mask & ~notFull;
    return any;
}

// The text system puts this tile (a black line on its left) down the GBA
// screen's left edge, in column 0 of BG0. The wide view shows what is left of
// that edge, where the line would stand out: it is left out there.
#define EDGE_TILE 0xF279

static int RenderTextBg(int bg, int line, uint16_t *out);

// Wide view, gPortTileBg: the background's GBA-sized picture repeated, with
// one copy in the middle of the screen (full-screen move effects).
static int RenderTiledBg(int bg, int line, uint16_t *out)
{
    static uint16_t gbaLine[GBA_W];
    int gy = ((line - TILE_OY) % GBA_H + GBA_H) % GBA_H;
    int any;

    sWide = 0; sW = GBA_W; sOx = 0;
    any = RenderTextBg(bg, gy, gbaLine);
    sWide = 1; sW = sViewW; sOx = sViewOx;
    sLastMask = ALL_GROUPS; sLastFull = 0;
    if (!any)
        return 0; // nothing to show: compositing skips the layer
    int gx = ((-sOx) % GBA_W + GBA_W) % GBA_W;
    for (int sx = 0; sx < sW; sx++) {
        out[sx] = gbaLine[gx];
        if (++gx == GBA_W)
            gx = 0;
    }
    sLastMask = ALL_GROUPS; sLastFull = 0;
    return any;
}

// Wide view, gPortHudLines set: BG0's top row (the dungeon's floor, level
// and HP; `line` is the row's own, 0-7), gPortHudLeft further left than the
// GBA screen's, its HP bar (8 pixels per tile from column 18, shown while
// the row holds it) longer: gPortHudBar's.
static int RenderHud(int line, uint16_t *out, uint16_t cnt)
{
    int ox = sOx - gPortHudLeft;
    int saveOx = sOx;
    sOx = ox;
    sW = ox + GBA_W;
    int any = RenderTextBgVram(0, line, out, cnt);
    sOx = saveOx;
    sW = sViewW;
    for (int x = 0; x < ox; x++) out[x] = TRANSPARENT;
    for (int x = ox + GBA_W; x < sW; x++) out[x] = TRANSPARENT;

    int hofs = R16(0x10) & 0x1FF, vofs = R16(0x12) & 0x1FF;
    unsigned screenBase = ((cnt >> 8) & 0x1F) * 0x800;
    uint16_t e = *(const uint16_t *)&gPortVram[screenBase + (((vofs >> 3) & 31) * 32 + ((18 + (hofs >> 3)) & 31)) * 2];
    if (e == 0xF2CC) { // the bar is shown
        int x0 = ox + 18 * 8;
        for (int i = 0; i < PORT_HUD_BAR_MAX && x0 + i < sW; i++) {
            uint32_t row = gPortHudBar[(i >> 3) * 8 + line];
            unsigned n = (row >> ((i & 7) * 4)) & 0xF;
            out[x0 + i] = n ? Pal(0xF0 + n) : TRANSPARENT;
            any |= n != 0;
        }
    }
    sLastMask = ALL_GROUPS; sLastFull = 0;
    return any;
}

// Returns nonzero if the line has any opaque pixel.
static int RenderTextBg(int bg, int line, uint16_t *out)
{
    uint16_t cnt = R16(0x08 + bg * 2);
    sLastMask = ALL_GROUPS; sLastFull = 0;
    if (sWide && gPortWideBg[bg].map)
        return RenderWideBg(bg, line, out); // (sets sLastMask)
    if (sWide && gPortTileBg[bg] && R16(0x10 + bg * 4) == 0 && R16(0x12 + bg * 4) == 0) {
        sWantPtr = 0; // (these change the line after: in sBg)
        return RenderTiledBg(bg, line, out);
    }
    if (sWide && bg < 2) {
        sWantPtr = 0;
        // BG0/BG1 hold the text windows: in the wide view they reach the
        // screen's bottom (the game moves them down, text_1.c), but stay
        // within the GBA screen's width; what the maps hold to the sides is
        // left over, so not drawn. Column 0 of BG1 is always the text
        // system's fill tile, which in dungeons is the darkness (blended over
        // the floor): to the sides BG1 is that tile, so the darkness reaches
        // the edges.
        if (bg == 0 && gPortHudLines && line < 8 + gPortHudLines) {
            // the top row, gPortHudLines lower
            if (line < gPortHudLines) {
                for (int x = 0; x < sW; x++) out[x] = TRANSPARENT;
                sLastMask = 0; sLastFull = 0;
                return 0;
            }
            return RenderHud(line - gPortHudLines, out, cnt);
        }
        sW = sOx + GBA_W;
        int any = RenderTextBgVram(bg, line, out, cnt);
        sW = sViewW;
        uint32_t centerMask = sLastMask, centerFull = sLastFull;
        if (bg == 0) {
            for (int x = 0; x < sOx; x++) out[x] = TRANSPARENT;
            for (int x = sOx + GBA_W; x < sW; x++) out[x] = TRANSPARENT;
            // the sides are not opaque, whatever the map held there
            sLastFull &= ~(Groups(0, sOx) | Groups(sOx + GBA_W, sW - sOx - GBA_W));
            return any;
        }
        int hofs = R16(0x14) & 0x1FF, vofs = R16(0x16) & 0x1FF;
        int gy = line + vofs;
        unsigned screenBase = ((cnt >> 8) & 0x1F) * 0x800;
        unsigned mapOff = screenBase + ((gy >> 3) & 31) * 64 + ((hofs >> 3) & 31) * 2;
        uint16_t e = *(const uint16_t *)&gPortVram[mapOff];
        uint16_t px[8];
        int fill = DecodeTileRow(e, ((cnt >> 2) & 3) * 0x4000, gy & 7, (cnt >> 7) & 1, px);
        uint16_t pat[8]; // (screen x & 7 -> the tile's pixel there)
        for (int i = 0; i < 8; i++) pat[i] = px[(i - sOx + hofs) & 7];
        for (int x = 0; x < sOx; x++) out[x] = pat[x & 7];
        for (int x = sOx + GBA_W; x < sW; x++) out[x] = pat[x & 7];
        // The groups: the middle's as drawn (those wholly to the sides: the
        // side tile's), those taking in the sides opaque only if it is.
        uint32_t sides = Groups(0, sOx) | Groups(sOx + GBA_W, sW - sOx - GBA_W);
        uint32_t middle = Groups(sOx, GBA_W);
        sLastMask = fill ? (centerMask | sides) : centerMask;
        sLastFull = (centerFull & ~sides) | (fill == 2 ? (sides & ~middle) | (centerFull & sides & middle) : 0);
        return any | fill;
    }
    return RenderTextBgVram(bg, line, out, cnt);
}

static int RenderTextBgVram(int bg, int line, uint16_t *out, uint16_t cnt)
{
    if (cnt & 0x40) {
        RenderTextBgMosaic(bg, line, out);
        sLastMask = ALL_GROUPS; sLastFull = 0;
        return 1;
    }
    unsigned charBase = ((cnt >> 2) & 3) * 0x4000;
    unsigned screenBase = ((cnt >> 8) & 0x1F) * 0x800;
    int color256 = (cnt >> 7) & 1;
    int size = (cnt >> 14) & 3;
    int widthPx = (size & 1) ? 512 : 256;
    int heightPx = (size & 2) ? 512 : 256;
    int hofs = R16(0x10 + bg * 4) & 0x1FF;
    int vofs = R16(0x12 + bg * 4) & 0x1FF;
    int y = (line + vofs) & (heightPx - 1);
    BgCache *c = &gBgCache[bg];
    if (c->valid && !c->wideMap && c->charBase == charBase && c->screenBase == screenBase
        && c->color256 == color256 && c->size == size && c->edge == sWide)
        return RenderCachedLine(c, y, (hofs - sOx) & (widthPx - 1), out);
    int inY = y & 7;
    unsigned rowBlock = (y >= 256) ? ((widthPx == 512) ? 2 : 1) : 0;
    unsigned rowOff = screenBase + rowBlock * 0x800 + ((y >> 3) & 31) * 64;
    int any = 0;
    uint32_t mask = 0, notFull = 0;

    int sx = 0;
    int x = (hofs - sOx) & (widthPx - 1);
    while (sx < sW) {
        unsigned mapOff = rowOff + ((x >= 256) ? 0x800 : 0) + ((x >> 3) & 31) * 2;
        int skip = x & 7;
        int n = 8 - skip;
        if (n > sW - sx) n = sW - sx;
        if (mapOff + 1 >= 0x10000) {
            for (int i = 0; i < n; i++) out[sx + i] = TRANSPARENT;
            notFull |= Groups(sx, n);
        }
        else {
            uint16_t e = *(const uint16_t *)&gPortVram[mapOff]; // map entries are 2-byte aligned
            if (sWide && e == EDGE_TILE)
                e = 0; // see EDGE_TILE
            if (n == 8) {
                int got = DecodeTileRow(e, charBase, inY, color256, &out[sx]);
                if (got) {
                    any = 1;
                    mask |= Groups(sx, 8);
                }
                if (got != 2)
                    notFull |= Groups(sx, 8);
            }
            else {
                uint16_t px[8];
                notFull |= Groups(sx, n);
                if (DecodeTileRow(e, charBase, inY, color256, px)) {
                    for (int i = 0; i < n; i++) {
                        out[sx + i] = px[skip + i];
                        any |= !(px[skip + i] & TRANSPARENT);
                    }
                    mask |= Groups(sx, n);
                }
                else {
                    for (int i = 0; i < n; i++) out[sx + i] = TRANSPARENT;
                }
            }
        }
        sx += n;
        x = (x + n) & (widthPx - 1);
    }
    sLastMask = mask;
    sLastFull = mask & ~notFull;
    return any;
}

// --- Affine backgrounds and mode 3 (rare: per-pixel) -----------------------

static void RenderAffineBg(int bg, uint16_t *out)
{
    int i = bg - 2;
    uint16_t cnt = R16(0x08 + bg * 2);
    unsigned charBase = ((cnt >> 2) & 3) * 0x4000;
    unsigned screenBase = ((cnt >> 8) & 0x1F) * 0x800;
    int sizePx = 128 << ((cnt >> 14) & 3);
    int wrap = (cnt >> 13) & 1;
    int16_t pa = (int16_t)R16(0x20 + i * 0x10);
    int16_t pc = (int16_t)R16(0x24 + i * 0x10);
    int32_t fx = sAffX[i] - sOx * pa, fy = sAffY[i] - sOx * pc;
    int mosaicH = (cnt & 0x40) ? (R16(0x4C) & 0xF) + 1 : 1;

    for (int sx = 0; sx < sW; sx++, fx += pa, fy += pc) {
        int32_t sampleX = fx, sampleY = fy;
        if (mosaicH > 1) {
            int back = sx % mosaicH;
            sampleX -= pa * back;
            sampleY -= pc * back;
        }
        int x = sampleX >> 8, y = sampleY >> 8;
        if (wrap) { x &= sizePx - 1; y &= sizePx - 1; }
        else if (x < 0 || y < 0 || x >= sizePx || y >= sizePx) { out[sx] = TRANSPARENT; continue; }
        unsigned mapOff = screenBase + (y >> 3) * (sizePx >> 3) + (x >> 3);
        unsigned tile = mapOff < 0x10000 ? gPortVram[mapOff] : 0;
        unsigned a = charBase + tile * 64 + (y & 7) * 8 + (x & 7);
        unsigned idx = a < 0x10000 ? gPortVram[a] : 0;
        out[sx] = idx ? Pal(idx) : TRANSPARENT;
    }
}

static void RenderMode3(int line, uint16_t *out)
{
    for (int x = 0; x < sW; x++) {
        int gx = x - sOx;
        if (gx < 0 || gx >= GBA_W || line >= GBA_H) { out[x] = TRANSPARENT; continue; }
        unsigned a = (line * GBA_W + gx) * 2;
        out[x] = (uint16_t)((gPortVram[a] | (gPortVram[a + 1] << 8)) & 0x7FFF);
    }
}

// --- Objects ---------------------------------------------------------------

static const uint8_t sObjW[3][4] = { { 8, 16, 32, 64 }, { 16, 32, 32, 64 }, { 8, 8, 16, 32 } };
static const uint8_t sObjH[3][4] = { { 8, 16, 32, 64 }, { 8, 8, 16, 32 }, { 16, 32, 32, 64 } };

static inline unsigned ObjTexel(unsigned tile, int tx, int ty, int color256)
{
    if (color256) {
        unsigned a = 0x10000 + ((tile & 0x3FF) * 32) + (ty & 7) * 8 + (tx & 7);
        return a < 0x18000 ? gPortVram[a] : 0;
    }
    unsigned a = 0x10000 + ((tile & 0x3FF) * 32) + (ty & 7) * 4 + ((tx & 7) >> 1);
    unsigned idx = a < 0x18000 ? gPortVram[a] : 0;
    return (tx & 1) ? (idx >> 4) : (idx & 0xF);
}

static inline void ObjPlot(int sx, unsigned idx, int mode, int prio, int color256, unsigned palBank)
{
    if (sx < sObjMinX) sObjMinX = sx;
    if (sx > sObjMaxX) sObjMaxX = sx;
    if (mode == 2) { // object window: shape only
        sObj.win[sx] = 1;
        sObjWinAny = 1;
        return;
    }
    if (prio < sObj.prio[sx]) {
        sObj.color[sx] = PAL16[256 + (color256 ? idx : palBank * 16 + idx)] & 0x7FFF;
        sObj.prio[sx] = (uint8_t)prio;
        sObj.semi[sx] = (mode == 1);
        sObjAny = 1;
        sObjPrioMask |= 1u << prio;
        if (mode == 1) sObjSemiAny = 1;
    }
}

static void RenderObjects(int line, uint16_t dispcnt)
{
    int oneD = (dispcnt >> 6) & 1;
    int bitmapMode = (dispcnt & 7) >= 3;
    uint16_t mosaic = R16(0x4C);
    int mosH = ((mosaic >> 8) & 0xF) + 1, mosV = ((mosaic >> 12) & 0xF) + 1;

    if (sObjDirtyMax >= sObjDirtyMin) {
        int n = sObjDirtyMax - sObjDirtyMin + 1;
        memset(&sObj.color[sObjDirtyMin], 0x80, n * sizeof(uint16_t)); // 0x8080: TRANSPARENT bit set
        memset(&sObj.prio[sObjDirtyMin], 4, n);
        memset(&sObj.semi[sObjDirtyMin], 0, n);
        memset(&sObj.win[sObjDirtyMin], 0, n);
    }
    sObjAny = sObjSemiAny = sObjWinAny = 0;
    sObjMinX = sW;
    sObjMaxX = -1;
    sObjPrioMask = 0;
    if (!(dispcnt & 0x1000))
        return;

    for (int k = 0; k < sObjCount; k++) {
        const uint8_t *o = &gPortOam[sObjList[k] * 8];
        uint16_t a0 = (uint16_t)(o[0] | (o[1] << 8));
        int affine = (a0 >> 8) & 1;
        if (!affine && (a0 & 0x200))
            continue; // disabled
        int shape = (a0 >> 14) & 3;
        if (shape == 3)
            continue;
        uint16_t a1 = (uint16_t)(o[2] | (o[3] << 8));
        int size = (a1 >> 14) & 3;
        int w = sObjW[shape][size], h = sObjH[shape][size];
        int boundW = w, boundH = h;
        if (affine && (a0 & 0x200)) { boundW *= 2; boundH *= 2; }

        int y = a0 & 0xFF;
        int wholeY = gPortOamY[sObjList[k]];
        if (sWide && (wholeY & 0xFF) == y)
            y = wholeY; // the game's y, whole (port_view.h)
        else if (y + boundH > 256) y -= 256;
        if (line < y || line >= y + boundH)
            continue;
        int ly = line;
        int objMosaic = (a0 >> 12) & 1;
        if (objMosaic) ly -= ly % mosV;
        int row = ly - y;
        if (row < 0)
            continue;

        int mode = (a0 >> 10) & 3;
        if (mode == 3)
            continue;
        uint16_t a2 = (uint16_t)(o[4] | (o[5] << 8));
        int color256 = (a0 >> 13) & 1;
        unsigned tileNum = a2 & 0x3FF;
        if (bitmapMode && tileNum < 512)
            continue;
        int x = a1 & 0x1FF;
        if (x >= sObjXWrap) x -= 512;
        x += sOx;
        if (x >= sW || x + boundW <= 0)
            continue;
        int prio = (a2 >> 10) & 3;
        unsigned palBank = (a2 >> 12) & 0xF;
        unsigned rowStride = oneD ? (unsigned)(w >> 3) * (color256 ? 2 : 1) : 32;
        unsigned tileStep = color256 ? 2 : 1;

        if (affine) {
            unsigned p = ((a1 >> 9) & 0x1F) * 32;
            int16_t pa = (int16_t)(gPortOam[p + 6] | (gPortOam[p + 7] << 8));
            int16_t pb = (int16_t)(gPortOam[p + 14] | (gPortOam[p + 15] << 8));
            int16_t pc = (int16_t)(gPortOam[p + 22] | (gPortOam[p + 23] << 8));
            int16_t pd = (int16_t)(gPortOam[p + 30] | (gPortOam[p + 31] << 8));
            for (int bx = 0; bx < boundW; bx++) {
                int sx = x + bx;
                if (sx < 0 || sx >= sW)
                    continue;
                int col = bx;
                if (objMosaic) col -= ((sx - sOx) % mosH + mosH) % mosH;
                int cx = col - boundW / 2, cy = row - boundH / 2;
                int tx = ((pa * cx + pb * cy) >> 8) + w / 2;
                int ty = ((pc * cx + pd * cy) >> 8) + h / 2;
                if (tx < 0 || ty < 0 || tx >= w || ty >= h)
                    continue;
                unsigned tile = tileNum + (ty >> 3) * rowStride + (tx >> 3) * tileStep;
                unsigned idx = ObjTexel(tile, tx, ty, color256);
                if (idx)
                    ObjPlot(sx, idx, mode, prio, color256, palBank);
            }
            continue;
        }

        int hflip = (a1 >> 12) & 1;
        int vflip = (a1 >> 13) & 1;
        int ty = vflip ? h - 1 - row : row;
        unsigned rowTile = tileNum + (ty >> 3) * rowStride;
        int bx0 = x < 0 ? -x : 0;
        int bx1 = (x + boundW > sW) ? sW - x : boundW;
        if (objMosaic) {
            for (int bx = bx0; bx < bx1; bx++) {
                int sx = x + bx;
                int col = bx - (((sx - sOx) % mosH + mosH) % mosH);
                int tx = hflip ? w - 1 - col : col;
                if (tx < 0 || tx >= w) continue;
                unsigned idx = ObjTexel(rowTile + (tx >> 3) * tileStep, tx, ty, color256);
                if (idx)
                    ObjPlot(sx, idx, mode, prio, color256, palBank);
            }
            continue;
        }
        for (int bx = bx0; bx < bx1; bx++) {
            int tx = hflip ? w - 1 - bx : bx;
            unsigned idx = ObjTexel(rowTile + (tx >> 3) * tileStep, tx, ty, color256);
            if (idx)
                ObjPlot(x + bx, idx, mode, prio, color256, palBank);
        }
    }
}

// --- Compositing -----------------------------------------------------------

// The three channels at once: red in bits 0-9, blue 10-20 and green 21-31 of
// a word leave each channel's sum (at most 31 * 16 * 2) room to not carry
// into the next; channels past 31 are clamped as the hardware does.
static inline uint32_t Spread(uint16_t c) { return (c & 0x7C1Fu) | ((uint32_t)(c & 0x3E0u) << 16); }

static inline uint16_t Blend(uint16_t a, uint16_t b, int eva, int evb)
{
    uint32_t x = (Spread(a) * (uint32_t)eva + Spread(b) * (uint32_t)evb) >> 4;
    uint32_t over = x & ((1u << 5) | (1u << 15) | (1u << 26)); // channel >= 32
    x |= over - (over >> 5);                                    // then 31
    return (uint16_t)((x & 0x7C1Fu) | ((x >> 16) & 0x3E0u));
}

// ... with eva + evb <= 16, where no channel can pass 31: no clamping
static inline uint16_t BlendNoClamp(uint32_t aTimesEva, uint16_t b, int evb)
{
    uint32_t x = (aTimesEva + Spread(b) * (uint32_t)evb) >> 4;
    return (uint16_t)((x & 0x7C1Fu) | ((x >> 16) & 0x3E0u));
}

// n copies of v from p
static inline void Fill16(uint16_t *p, uint16_t v, int n)
{
    if (n > 0 && ((uintptr_t)p & 2)) { *p++ = v; n--; }
    uint32_t v2 = v | ((uint32_t)v << 16);
    uint32_t *q = (uint32_t *)p;
    for (; n >= 2; n -= 2) *q++ = v2;
    if (n) *(uint16_t *)q = v;
}

static inline uint16_t Brighten(uint16_t c, int evy)
{
    int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    r += ((31 - r) * evy) >> 4;
    g += ((31 - g) * evy) >> 4;
    b += ((31 - b) * evy) >> 4;
    return (uint16_t)(r | (g << 5) | (b << 10));
}

static inline uint16_t Darken(uint16_t c, int evy)
{
    int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    r -= (r * evy) >> 4;
    g -= (g * evy) >> 4;
    b -= (b * evy) >> 4;
    return (uint16_t)(r | (g << 5) | (b << 10));
}

// Wide view: window edges in GBA coordinates to screen coordinates. Edges at
// the GBA screen's edges move to the wide screen's edges.
static void WideWindowX(int *lo, int *hi)
{
    if (*lo == 1 && *hi == 0) { *lo = 0; *hi = sW; return; } // "x >= 1 or x < 0": whole line
    *lo = (*lo == 0) ? 0 : (*lo >= GBA_W) ? sW : *lo + sOx;
    *hi = (*hi == 0) ? 0 : (*hi >= GBA_W) ? sW : *hi + sOx;
}

static int InWindowRange(int v, int lo, int hi, int limit)
{
    if (hi > limit || lo > hi) {
        // Hardware treats out-of-range or inverted bounds as a wrapped window.
        if (hi > limit && lo <= hi) hi = limit;
        else return v >= lo || v < hi;
    }
    return v >= lo && v < hi;
}

// Fills [lo, hi) of a window span (screen x), with the hardware's
// wrap-around rules (same as InWindowRange, but for a whole line at once).
static void FillSpanScreen(uint8_t *wm, int lo, int hi, uint8_t value)
{
    if (hi > sW || lo > hi) {
        if (hi > sW && lo <= hi) hi = sW;
        else { // wrapped: x >= lo or x < hi
            if (hi > 0) memset(wm, value, hi > sW ? sW : hi);
            if (lo < sW) memset(wm + (lo < 0 ? 0 : lo), value, sW - (lo < 0 ? 0 : lo));
            return;
        }
    }
    for (int x = lo; x < hi; x++) wm[x] = value;
}

// ... in GBA x (the window registers)
static void FillSpan(uint8_t *wm, int lo, int hi, uint8_t value)
{
    if (sWide)
        WideWindowX(&lo, &hi);
    FillSpanScreen(wm, lo, hi, value);
}

// Lines a window can reach: 160, or the wide view's height. A window that
// reaches the GBA screen's bottom reaches the wide screen's bottom.
static int sVLimit = GBA_H;
static inline int WinBottom(int hi) { return (sWide && hi >= GBA_H) ? sViewH : hi; }

// Composites one line. Layers are painted back to front through the window
// mask, tracking each pixel's top two layers; then the colour effect (alpha,
// brighten or darken) is applied in one pass. Equivalent to the reference's
// per-pixel search for the top two visible layers, without the per-pixel work.
static void Compose(int line, uint16_t *dst, const int bgOn[4], const int bgPrio[4], uint16_t dispcnt)
{
    static uint8_t wm[W] __attribute__((aligned(4))), topL[W], secL[W];
    static uint16_t secC[W];

    uint16_t bldcnt = R16(0x50);
    uint16_t bldalpha = R16(0x52);
    int eva = bldalpha & 0x1F, evb = (bldalpha >> 8) & 0x1F;
    int evy = R16(0x54) & 0x1F;
    if (eva > 16) eva = 16;
    if (evb > 16) evb = 16;
    if (evy > 16) evy = 16;
    int effect = (bldcnt >> 6) & 3;

    // Window mask per pixel: bits 0-3 BG0-3, bit 4 OBJ, bit 5 colour effects.
    // wmAll: the bits set everywhere on the line (the AND of every value
    // the mask can hold), for which the per-pixel test can be skipped.
    int win0 = (dispcnt >> 13) & 1, win1 = (dispcnt >> 14) & 1, winObj = (dispcnt >> 15) & 1;
    unsigned wmAll = 0x3F;
    uint8_t winout6 = 0x3F;
    if (!(win0 | win1 | winObj)) {
        memset(wm, 0x3F, sizeof(wm));
    }
    else {
        uint16_t winin = R16(0x48), winout = R16(0x4A);
        wmAll = winout & 0x3F;
        winout6 = winout & 0x3F;
        if (winObj && sObjWinAny) wmAll &= (winout >> 8) & 0x3F;
        if (win1) wmAll &= (winin >> 8) & 0x3F;
        if (win0) wmAll &= winin & 0x3F;
        memset(wm, winout & 0x3F, sizeof(wm));
        if (winObj && sObjWinAny) {
            uint8_t v = (winout >> 8) & 0x3F;
            for (int x = sObjMinX; x <= sObjMaxX; x++)
                if (sObj.win[x]) wm[x] = v;
        }
        if (win1) {
            uint16_t h = R16(0x42), v = R16(0x46);
            if (InWindowRange(line, v >> 8, WinBottom(v & 0xFF), sVLimit)) {
                const struct PortWinX *e = &gPortWin1X[line];
                if (sWide && e->valid && e->reg == h)
                    FillSpanScreen(wm, e->lo, e->hi, (winin >> 8) & 0x3F);
                else
                    FillSpan(wm, h >> 8, h & 0xFF, (winin >> 8) & 0x3F);
            }
        }
        if (win0) { // window 0 has priority over window 1
            uint16_t h = R16(0x40), v = R16(0x44);
            if (InWindowRange(line, v >> 8, WinBottom(v & 0xFF), sVLimit))
                FillSpan(wm, h >> 8, h & 0xFF, winin & 0x3F);
        }
    }

    // Per group, the window bits set in all its pixels / in some (groups
    // wholly inside or outside a window skip the per-pixel test below).
    // Mostly a group is all winout's: only the others are looked at closely.
    uint8_t gAll[32], gSome[32];
    const int groups = (sW + (1 << sGs) - 1) >> sGs;
    if (wmAll != 0x3F) {
        const int gs = sGs;
        const uint8_t out6 = winout6;
        const uint32_t rep4 = out6 * 0x01010101u;
        for (int g = 0; g < groups; g++) {
            int x0 = g << gs, x1 = x0 + (1 << gs);
            if (x1 > sW) x1 = sW;
            uint32_t a = 0xFFFFFFFFu, o = 0;
            int x = x0;
            for (; x + 4 <= x1; x += 4) { // (wm is 4-byte aligned, groups too)
                uint32_t w = *(const uint32_t *)&wm[x];
                a &= w;
                o |= w;
            }
            for (; x < x1; x++) { a &= wm[x] * 0x01010101u; o |= wm[x]; }
            if (a == rep4 && o == rep4) {
                gAll[g] = gSome[g] = out6;
                continue;
            }
            gAll[g] = (uint8_t)(a & (a >> 8) & (a >> 16) & (a >> 24));
            gSome[g] = (uint8_t)((o | (o >> 8) | (o >> 16) | (o >> 24)) & 0x3F);
        }
    }

    uint16_t backdrop = Pal(0);
    Fill16(dst, backdrop, sW);
    memset(topL, L_BD, sizeof(topL));
    int needSec = (effect == 1) || sObjSemiAny;
    if (needSec) {
        memset(secL, L_BD, sizeof(secL));
        Fill16(secC, backdrop, sW);
    }

    // Back to front: within a priority, BG3..BG0, then sprites (a sprite beats
    // a background of the same priority, a lower BG index beats a higher one).
    for (int p = 3; p >= 0; p--) {
        for (int b = 3; b >= 0; b--) {
            if (!bgOn[b] || bgPrio[b] != p || !sBgAny[b])
                continue;
            const uint16_t *src = sBgSrc[b];
            uint8_t bit = (uint8_t)(1u << b);
            uint32_t inAll = ALL_GROUPS, inSome = ALL_GROUPS; // groups shown in all pixels / some
            if (!(wmAll & bit)) {
                inAll = inSome = 0;
                for (int g = 0; g < groups; g++) {
                    inAll |= (uint32_t)((gAll[g] >> b) & 1) << g;
                    inSome |= (uint32_t)((gSome[g] >> b) & 1) << g;
                }
            }
            uint32_t full = sBgFull[b] & inAll; // opaque and shown: copied whole
            uint32_t m = full;
            int x, x1;
            while (NextRun(&m, &x, &x1)) {
                int n = x1 - x;
                if (needSec) {
                    memcpy(&secC[x], &dst[x], n * sizeof(uint16_t));
                    memcpy(&secL[x], &topL[x], n);
                }
                memcpy(&dst[x], &src[x], n * sizeof(uint16_t));
                memset(&topL[x], b, n);
            }
            uint32_t shown = sBgMask[b] & inAll & ~full; // shown: only transparency matters
            uint32_t part = sBgMask[b] & inSome & ~inAll; // shown in some pixels
            for (int pass = 0; pass < 2; pass++) {
              m = pass ? part : shown;
              while (NextRun(&m, &x, &x1)) {
                if (!pass) {
                    if (needSec) {
                        for (; x < x1; x++) {
                            if (!(src[x] & TRANSPARENT)) {
                                secC[x] = dst[x]; secL[x] = topL[x];
                                dst[x] = src[x]; topL[x] = (uint8_t)b;
                            }
                        }
                    }
                    else {
                        for (; x < x1; x++) {
                            if (!(src[x] & TRANSPARENT)) {
                                dst[x] = src[x]; topL[x] = (uint8_t)b;
                            }
                        }
                    }
                }
                else if (needSec) {
                    for (; x < x1; x++) {
                        if (!(src[x] & TRANSPARENT) && (wm[x] & bit)) {
                            secC[x] = dst[x]; secL[x] = topL[x];
                            dst[x] = src[x]; topL[x] = (uint8_t)b;
                        }
                    }
                }
                else {
                    for (; x < x1; x++) {
                        if (!(src[x] & TRANSPARENT) && (wm[x] & bit)) {
                            dst[x] = src[x]; topL[x] = (uint8_t)b;
                        }
                    }
                }
              }
            }
        }
        if (sObjAny && (sObjPrioMask & (1u << p))) {
            for (int x = sObjMinX; x <= sObjMaxX; x++) {
                if (sObj.prio[x] == p && !(sObj.color[x] & TRANSPARENT) && (wm[x] & 0x10)) {
                    if (needSec) { secC[x] = dst[x]; secL[x] = topL[x]; }
                    dst[x] = sObj.color[x]; topL[x] = L_OBJ;
                }
            }
        }
    }

    // Colour effects, on the groups where the top layer can be a target.
    if (effect == 0 && !sObjSemiAny)
        return;
    uint32_t targets = ALL_GROUPS;
    if (!(bldcnt & 0x20)) { // the backdrop is not a first target
        targets = 0;
        for (int b = 0; b < 4; b++)
            if ((bldcnt >> b) & 1)
                targets |= sBgMask[b];
        if (sObjAny && ((bldcnt & 0x10) || sObjSemiAny))
            targets |= Groups(sObjMinX, sObjMaxX - sObjMinX + 1);
    }
    if (effect == 1 && !sObjSemiAny) { // alpha blending only (the usual case)
        unsigned first = bldcnt & 0x3F, second = (bldcnt >> 8) & 0x3F;
        uint32_t effAll = ALL_GROUPS, effSome = ALL_GROUPS;
        if (!(wmAll & 0x20)) {
            effAll = effSome = 0;
            for (int g = 0; g < groups; g++) {
                effAll |= (uint32_t)((gAll[g] >> 5) & 1) << g;
                effSome |= (uint32_t)((gSome[g] >> 5) & 1) << g;
            }
        }
        uint32_t m = targets & effAll; // (effects on in every pixel)
        int x0, x1;
        if (eva + evb <= 16) { // (the top colour is mostly the same one: its product kept)
            uint16_t lastTop = dst[0] ^ 0xFFFF;
            uint32_t lastProd = 0;
            while (NextRun(&m, &x0, &x1)) {
                for (int x = x0; x < x1; x++) {
                    if ((first >> topL[x]) & (second >> secL[x]) & 1) {
                        uint16_t t = dst[x];
                        if (t != lastTop) { lastTop = t; lastProd = Spread(t) * (uint32_t)eva; }
                        dst[x] = BlendNoClamp(lastProd, secC[x], evb);
                    }
                }
            }
        }
        while (NextRun(&m, &x0, &x1)) {
            for (int x = x0; x < x1; x++) {
                if ((first >> topL[x]) & (second >> secL[x]) & 1)
                    dst[x] = Blend(dst[x], secC[x], eva, evb);
            }
        }
        m = targets & effSome & ~effAll; // (in some)
        while (NextRun(&m, &x0, &x1)) {
            for (int x = x0; x < x1; x++) {
                if (((first >> topL[x]) & (second >> secL[x]) & 1) && (wm[x] & 0x20))
                    dst[x] = Blend(dst[x], secC[x], eva, evb);
            }
        }
        return;
    }
    uint32_t m = targets;
    int x0, x1;
    while (NextRun(&m, &x0, &x1)) {
      for (int x = x0; x < x1; x++) {
        int t = topL[x];
        if (needSec && t == L_OBJ && sObj.semi[x] && ((bldcnt >> (8 + secL[x])) & 1)) {
            dst[x] = Blend(dst[x], secC[x], eva, evb); // semi-transparent sprites always alpha blend
            continue;
        }
        if (!(wm[x] & 0x20) || !((bldcnt >> t) & 1))
            continue;
        if (effect == 1) {
            if ((bldcnt >> (8 + secL[x])) & 1)
                dst[x] = Blend(dst[x], secC[x], eva, evb);
        }
        else if (effect == 2) dst[x] = Brighten(dst[x], evy);
        else if (effect == 3) dst[x] = Darken(dst[x], evy);
      }
    }
}

// A line left to the other renderer (Dual Core): only what carries over to
// the next line (the affine backgrounds' reference points) is kept up.
void Ppu_SkipLine(int line)
{
    (void)line;
    for (int i = 0; i < 2; i++) {
        AffineReloadIfWritten(i);
        AffineAdvance(i);
    }
}

void Ppu_RenderLine(int line, uint16_t *dst)
{
    uint16_t dispcnt = R16(0x00);
    sPalTBuilt = 0;
    int mode = dispcnt & 7;

    sVLimit = sWide ? sViewH : GBA_H;

    if (dispcnt & 0x80) { // forced blank: white
        for (int x = 0; x < sW; x++) dst[x] = 0x7FFF;
        for (int i = 0; i < 2; i++) { AffineReloadIfWritten(i); AffineAdvance(i); }
        return;
    }

    for (int i = 0; i < 2; i++)
        AffineReloadIfWritten(i);

    int bgOn[4] = { 0, 0, 0, 0 };
    switch (mode) {
    case 0:
        for (int b = 0; b < 4; b++)
            if (dispcnt & (0x100 << b)) {
                bgOn[b] = 1;
                sLastSrc = sBg[b];
                sWantPtr = 1;
                sBgAny[b] = RenderTextBg(b, line, sBg[b]);
                sWantPtr = 0;
                sBgSrc[b] = sLastSrc;
                sBgMask[b] = sBgAny[b] ? sLastMask : 0;
                sBgFull[b] = sBgAny[b] ? sLastFull : 0;
            }
        break;
    case 1:
        for (int b = 0; b < 2; b++)
            if (dispcnt & (0x100 << b)) {
                bgOn[b] = 1;
                sLastSrc = sBg[b];
                sWantPtr = 1;
                sBgAny[b] = RenderTextBg(b, line, sBg[b]);
                sWantPtr = 0;
                sBgSrc[b] = sLastSrc;
                sBgMask[b] = sBgAny[b] ? sLastMask : 0;
                sBgFull[b] = sBgAny[b] ? sLastFull : 0;
            }
        if (dispcnt & 0x400) { bgOn[2] = 1; sBgAny[2] = 1; sBgMask[2] = ALL_GROUPS; sBgFull[2] = 0; RenderAffineBg(2, sBg[2]); sBgSrc[2] = sBg[2]; }
        break;
    case 2:
        if (dispcnt & 0x400) { bgOn[2] = 1; sBgAny[2] = 1; sBgMask[2] = ALL_GROUPS; sBgFull[2] = 0; RenderAffineBg(2, sBg[2]); sBgSrc[2] = sBg[2]; }
        if (dispcnt & 0x800) { bgOn[3] = 1; sBgAny[3] = 1; sBgMask[3] = ALL_GROUPS; sBgFull[3] = 0; RenderAffineBg(3, sBg[3]); sBgSrc[3] = sBg[3]; }
        break;
    case 3:
        if (dispcnt & 0x400) { bgOn[2] = 1; sBgAny[2] = 1; sBgMask[2] = ALL_GROUPS; sBgFull[2] = 0; RenderMode3(line, sBg[2]); sBgSrc[2] = sBg[2]; }
        break;
    default:
        break; // modes 4/5 are not used by pmd-red
    }

    RenderObjects(line, dispcnt);
    sObjDirtyMin = sObjMinX;
    sObjDirtyMax = sObjMaxX;

    int bgPrio[4];
    for (int b = 0; b < 4; b++)
        bgPrio[b] = R16(0x08 + b * 2) & 3;
    Compose(line, dst, bgOn, bgPrio, dispcnt);

    for (int i = 0; i < 2; i++)
        AffineAdvance(i);
}
