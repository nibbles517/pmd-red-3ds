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
// Wide view (Ppu_SetView): the line is SCREEN_W (256) pixels, GBA x 0 drawn at
// screen x WIDE_OX, and lines past 159 are drawn too (up to SCREEN_H). The game
// only knows a 240x160 screen, so window edges at the screen's edges are
// stretched to the new edges, and backgrounds the game draws for the larger
// view (gPortWideBg) are read from wider maps kept outside VRAM.
//
// Speed matters here: this runs 160 times a frame on a 268 MHz CPU. Common
// cases take fast paths (whole tile rows decoded at once, layers painted back
// to front when no windows or alpha blending are active); everything else goes
// through the general per-pixel path. port/test/ppu_ref.c is the original
// straightforward version, and tools/test_ppu checks this file against it,
// byte for byte, on frames captured from the game.

#include <stdint.h>
#include <string.h>
#include "port.h"

#define W SCREEN_W // buffer size; the line drawn is sW pixels
static int sW = GBA_W, sOx = 0, sWide = 0;
struct PortWideBg gPortWideBg[4];
uint8_t gPortTileBg[4];
#define TILE_OY ((SCREEN_H - GBA_H) / 2)
static int RenderTextBgVram(int bg, int line, uint16_t *out, uint16_t cnt);

void Ppu_SetView(int wide)
{
    sWide = wide;
    sW = wide ? SCREEN_W : GBA_W;
    sOx = wide ? WIDE_OX : 0;
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
static int sBgAny[4];               // layer has an opaque pixel on this line
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

void Ppu_BeginFrame(void)
{
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

// Decodes one 8-pixel row of the tile named by map entry `e` into px[0..7].
// Returns nonzero if any pixel is opaque.
static inline int DecodeTileRow(uint16_t e, unsigned charBase, int inY, int color256, uint16_t *px)
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
        any = 1;
    }
    else {
        unsigned a = charBase + tile * 32 + ty * 4;
        uint32_t row = (a < 0x10000) ? *(const uint32_t *)&gPortVram[a] : 0; // 4-byte aligned by construction
        if (row == 0) {
            for (int i = 0; i < 8; i++) px[i] = TRANSPARENT;
            return 0;
        }
        const uint16_t *pal = &PAL16[(e >> 12) * 16];
        for (int i = 0; i < 8; i++, row >>= 4) {
            unsigned idx = row & 0xF;
            px[i] = idx ? (pal[idx] & 0x7FFF) : TRANSPARENT;
        }
        any = 1;
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

// A background the game draws for the wide view: a 64x32-entry map in RAM,
// scrolled by gPortWideBg (not by the BG registers). PORT_WIDE_BLACK entries
// are opaque black (outside the map).
static int RenderWideBg(int bg, int line, uint16_t *out)
{
    const struct PortWideBg *wb = &gPortWideBg[bg];
    uint16_t cnt = R16(0x08 + bg * 2);
    unsigned charBase = ((cnt >> 2) & 3) * 0x4000;
    int color256 = (cnt >> 7) & 1;
    int y = (line + wb->scrollY) & 255;
    int inY = y & 7;
    const uint16_t *row = &wb->map[(y >> 3) * 64];
    int any = 0;
    int x = (wb->scrollX - sOx) & 511;

    for (int sx = 0; sx < sW;) {
        uint16_t e = row[x >> 3];
        int skip = x & 7;
        int n = 8 - skip;
        uint16_t px[8];
        if (n > sW - sx) n = sW - sx;
        if (e == PORT_WIDE_BLACK) {
            for (int i = 0; i < n; i++) out[sx + i] = 0;
            any = 1;
        }
        else if (DecodeTileRow(e, charBase, inY, color256, px)) {
            for (int i = 0; i < n; i++) {
                out[sx + i] = px[skip + i];
                any |= !(px[skip + i] & TRANSPARENT);
            }
        }
        else {
            for (int i = 0; i < n; i++) out[sx + i] = TRANSPARENT;
        }
        sx += n;
        x = (x + n) & 511;
    }
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
    sWide = 1; sW = SCREEN_W; sOx = WIDE_OX;
    for (int sx = 0; sx < sW; sx++)
        out[sx] = gbaLine[((sx - sOx) % GBA_W + GBA_W) % GBA_W];
    return any;
}

// Returns nonzero if the line has any opaque pixel.
static int RenderTextBg(int bg, int line, uint16_t *out)
{
    uint16_t cnt = R16(0x08 + bg * 2);
    if (sWide && gPortWideBg[bg].map)
        return RenderWideBg(bg, line, out);
    if (sWide && gPortTileBg[bg] && R16(0x10 + bg * 4) == 0 && R16(0x12 + bg * 4) == 0)
        return RenderTiledBg(bg, line, out);
    if (sWide && bg < 2) {
        // BG0/BG1 hold the text windows, made for the GBA screen; what the
        // maps hold outside it is left over (old windows), so not drawn.
        if (line >= GBA_H) {
            for (int x = 0; x < sW; x++) out[x] = TRANSPARENT;
            return 0;
        }
        sW = sOx + GBA_W;
        int any = RenderTextBgVram(bg, line, out, cnt);
        sW = SCREEN_W;
        for (int x = 0; x < sOx; x++) out[x] = TRANSPARENT;
        for (int x = sOx + GBA_W; x < sW; x++) out[x] = TRANSPARENT;
        return any;
    }
    return RenderTextBgVram(bg, line, out, cnt);
}

static int RenderTextBgVram(int bg, int line, uint16_t *out, uint16_t cnt)
{
    if (cnt & 0x40) {
        RenderTextBgMosaic(bg, line, out);
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
    int inY = y & 7;
    unsigned rowBlock = (y >= 256) ? ((widthPx == 512) ? 2 : 1) : 0;
    unsigned rowOff = screenBase + rowBlock * 0x800 + ((y >> 3) & 31) * 64;
    int any = 0;

    int sx = 0;
    int x = (hofs - sOx) & (widthPx - 1);
    while (sx < sW) {
        unsigned mapOff = rowOff + ((x >= 256) ? 0x800 : 0) + ((x >> 3) & 31) * 2;
        int skip = x & 7;
        int n = 8 - skip;
        if (n > sW - sx) n = sW - sx;
        if (mapOff + 1 >= 0x10000) {
            for (int i = 0; i < n; i++) out[sx + i] = TRANSPARENT;
        }
        else {
            uint16_t e = *(const uint16_t *)&gPortVram[mapOff]; // map entries are 2-byte aligned
            if (sWide && e == EDGE_TILE)
                e = 0; // see EDGE_TILE
            if (n == 8) {
                any |= DecodeTileRow(e, charBase, inY, color256, &out[sx]);
            }
            else {
                uint16_t px[8];
                if (DecodeTileRow(e, charBase, inY, color256, px)) {
                    for (int i = 0; i < n; i++) {
                        out[sx + i] = px[skip + i];
                        any |= !(px[skip + i] & TRANSPARENT);
                    }
                }
                else {
                    for (int i = 0; i < n; i++) out[sx + i] = TRANSPARENT;
                }
            }
        }
        sx += n;
        x = (x + n) & (widthPx - 1);
    }
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

    for (int n = 0; n < 128; n++) {
        const uint8_t *o = &gPortOam[n * 8];
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
        if (y + boundH > 256) y -= 256;
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
        if (x >= 256) x -= 512;
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

static inline uint16_t Blend(uint16_t a, uint16_t b, int eva, int evb)
{
    int r = ((a & 31) * eva + (b & 31) * evb) >> 4;
    int g = (((a >> 5) & 31) * eva + ((b >> 5) & 31) * evb) >> 4;
    int bl = (((a >> 10) & 31) * eva + ((b >> 10) & 31) * evb) >> 4;
    if (r > 31) r = 31;
    if (g > 31) g = 31;
    if (bl > 31) bl = 31;
    return (uint16_t)(r | (g << 5) | (bl << 10));
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

// Fills [lo, hi) of a window span, with the hardware's wrap-around rules
// (same as InWindowRange, but for a whole line at once).
static void FillSpan(uint8_t *wm, int lo, int hi, uint8_t value)
{
    if (sWide)
        WideWindowX(&lo, &hi);
    if (hi > sW || lo > hi) {
        if (hi > sW && lo <= hi) hi = sW;
        else { // wrapped: x >= lo or x < hi
            for (int x = 0; x < sW; x++)
                if (x >= lo || x < hi) wm[x] = value;
            return;
        }
    }
    for (int x = lo; x < hi; x++) wm[x] = value;
}

// Lines a window can reach: 160, or the wide view's height. A window that
// reaches the GBA screen's bottom reaches the wide screen's bottom.
static int sVLimit = GBA_H;
static inline int WinBottom(int hi) { return (sWide && hi >= GBA_H) ? SCREEN_H : hi; }

// Composites one line. Layers are painted back to front through the window
// mask, tracking each pixel's top two layers; then the colour effect (alpha,
// brighten or darken) is applied in one pass. Equivalent to the reference's
// per-pixel search for the top two visible layers, without the per-pixel work.
static void Compose(int line, uint16_t *dst, const int bgOn[4], const int bgPrio[4], uint16_t dispcnt)
{
    static uint8_t wm[W], topL[W], secL[W];
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
    int win0 = (dispcnt >> 13) & 1, win1 = (dispcnt >> 14) & 1, winObj = (dispcnt >> 15) & 1;
    if (!(win0 | win1 | winObj)) {
        memset(wm, 0x3F, sizeof(wm));
    }
    else {
        uint16_t winin = R16(0x48), winout = R16(0x4A);
        memset(wm, winout & 0x3F, sizeof(wm));
        if (winObj && sObjWinAny) {
            uint8_t v = (winout >> 8) & 0x3F;
            for (int x = sObjMinX; x <= sObjMaxX; x++)
                if (sObj.win[x]) wm[x] = v;
        }
        if (win1) {
            uint16_t h = R16(0x42), v = R16(0x46);
            if (InWindowRange(line, v >> 8, WinBottom(v & 0xFF), sVLimit))
                FillSpan(wm, h >> 8, h & 0xFF, (winin >> 8) & 0x3F);
        }
        if (win0) { // window 0 has priority over window 1
            uint16_t h = R16(0x40), v = R16(0x44);
            if (InWindowRange(line, v >> 8, WinBottom(v & 0xFF), sVLimit))
                FillSpan(wm, h >> 8, h & 0xFF, winin & 0x3F);
        }
    }

    uint16_t backdrop = Pal(0);
    for (int x = 0; x < sW; x++) dst[x] = backdrop;
    memset(topL, L_BD, sizeof(topL));
    int needSec = (effect == 1) || sObjSemiAny;
    if (needSec) {
        memset(secL, L_BD, sizeof(secL));
        for (int x = 0; x < sW; x++) secC[x] = backdrop;
    }

    // Back to front: within a priority, BG3..BG0, then sprites (a sprite beats
    // a background of the same priority, a lower BG index beats a higher one).
    for (int p = 3; p >= 0; p--) {
        for (int b = 3; b >= 0; b--) {
            if (!bgOn[b] || bgPrio[b] != p || !sBgAny[b])
                continue;
            const uint16_t *src = sBg[b];
            uint8_t bit = (uint8_t)(1u << b);
            if (needSec) {
                for (int x = 0; x < sW; x++) {
                    if (!(src[x] & TRANSPARENT) && (wm[x] & bit)) {
                        secC[x] = dst[x]; secL[x] = topL[x];
                        dst[x] = src[x]; topL[x] = (uint8_t)b;
                    }
                }
            }
            else {
                for (int x = 0; x < sW; x++) {
                    if (!(src[x] & TRANSPARENT) && (wm[x] & bit)) {
                        dst[x] = src[x]; topL[x] = (uint8_t)b;
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

    // Colour effects.
    if (effect == 0 && !sObjSemiAny)
        return;
    for (int x = 0; x < sW; x++) {
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

void Ppu_RenderLine(int line, uint16_t *dst)
{
    uint16_t dispcnt = R16(0x00);
    int mode = dispcnt & 7;

    sVLimit = sWide ? SCREEN_H : GBA_H;

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
            if (dispcnt & (0x100 << b)) { bgOn[b] = 1; sBgAny[b] = RenderTextBg(b, line, sBg[b]); }
        break;
    case 1:
        for (int b = 0; b < 2; b++)
            if (dispcnt & (0x100 << b)) { bgOn[b] = 1; sBgAny[b] = RenderTextBg(b, line, sBg[b]); }
        if (dispcnt & 0x400) { bgOn[2] = 1; sBgAny[2] = 1; RenderAffineBg(2, sBg[2]); }
        break;
    case 2:
        if (dispcnt & 0x400) { bgOn[2] = 1; sBgAny[2] = 1; RenderAffineBg(2, sBg[2]); }
        if (dispcnt & 0x800) { bgOn[3] = 1; sBgAny[3] = 1; RenderAffineBg(3, sBg[3]); }
        break;
    case 3:
        if (dispcnt & 0x400) { bgOn[2] = 1; sBgAny[2] = 1; RenderMode3(line, sBg[2]); }
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
