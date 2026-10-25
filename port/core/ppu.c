// Scanline renderer for GBA video state held in shadow memory.
//
// Renders one 240-pixel line at a time from the shadow registers, VRAM, palette
// and OAM, so mid-frame register changes (pmd-red's VCount callbacks and
// HBlank DMA) take effect on exactly the lines they do on hardware.
//
// Output is GBA BGR555 (bit 15 clear). Covers modes 0-2 (text and affine
// backgrounds), mode 3, regular and affine sprites, windows 0/1 and the object
// window, alpha blending, brighten/darken, and mosaic.

#include <stdint.h>
#include <string.h>
#include "port.h"

#define W 240
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
static ObjLine sObj;

// Affine backgrounds keep internal reference points that advance each line.
static int32_t sAffX[2], sAffY[2];
static uint32_t sAffLatchX[2], sAffLatchY[2];

static inline uint16_t R16(unsigned off) { return (uint16_t)(gPortRegs[off] | (gPortRegs[off + 1] << 8)); }
static inline uint32_t R32(unsigned off) { return R16(off) | ((uint32_t)R16(off + 2) << 16); }
static inline uint16_t Pal(unsigned index) { return (uint16_t)((gPortPltt[index * 2] | (gPortPltt[index * 2 + 1] << 8)) & 0x7FFF); }

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

static void RenderTextBg(int bg, int line, uint16_t *out)
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
    int mosaicH = 1, y = line;

    if (cnt & 0x40) {
        uint16_t m = R16(0x4C);
        mosaicH = (m & 0xF) + 1;
        int mv = ((m >> 4) & 0xF) + 1;
        y -= y % mv;
    }
    y = (y + vofs) & (heightPx - 1);

    int tileRow = y >> 3, inY = y & 7;
    for (int sx = 0; sx < W; sx++) {
        int px = sx - (sx % mosaicH);
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
    int32_t fx = sAffX[i], fy = sAffY[i];
    int mosaicH = (cnt & 0x40) ? (R16(0x4C) & 0xF) + 1 : 1;

    for (int sx = 0; sx < W; sx++, fx += pa, fy += pc) {
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
    for (int x = 0; x < W; x++) {
        unsigned a = (line * W + x) * 2;
        out[x] = (uint16_t)((gPortVram[a] | (gPortVram[a + 1] << 8)) & 0x7FFF);
    }
}

static const uint8_t sObjW[3][4] = { { 8, 16, 32, 64 }, { 16, 32, 32, 64 }, { 8, 8, 16, 32 } };
static const uint8_t sObjH[3][4] = { { 8, 16, 32, 64 }, { 8, 8, 16, 32 }, { 16, 32, 32, 64 } };

static void RenderObjects(int line, uint16_t dispcnt)
{
    int oneD = (dispcnt >> 6) & 1;
    int bitmapMode = (dispcnt & 7) >= 3;
    uint16_t mosaic = R16(0x4C);
    int mosH = ((mosaic >> 8) & 0xF) + 1, mosV = ((mosaic >> 12) & 0xF) + 1;

    for (int x = 0; x < W; x++) {
        sObj.color[x] = TRANSPARENT;
        sObj.prio[x] = 4;
        sObj.semi[x] = 0;
        sObj.win[x] = 0;
    }
    if (!(dispcnt & 0x1000))
        return;

    for (int n = 0; n < 128; n++) {
        const uint8_t *o = &gPortOam[n * 8];
        uint16_t a0 = (uint16_t)(o[0] | (o[1] << 8));
        uint16_t a1 = (uint16_t)(o[2] | (o[3] << 8));
        uint16_t a2 = (uint16_t)(o[4] | (o[5] << 8));
        int affine = (a0 >> 8) & 1;
        if (!affine && (a0 & 0x200))
            continue; // disabled
        int shape = (a0 >> 14) & 3;
        if (shape == 3)
            continue;
        int size = (a1 >> 14) & 3;
        int w = sObjW[shape][size], h = sObjH[shape][size];
        int boundW = w, boundH = h;
        if (affine && (a0 & 0x200)) { boundW *= 2; boundH *= 2; }

        int y = a0 & 0xFF;
        if (y + boundH > 256) y -= 256;
        int x = a1 & 0x1FF;
        if (x >= 256) x -= 512;

        int ly = line;
        int objMosaic = (a0 >> 12) & 1;
        if (objMosaic) ly -= ly % mosV;
        int row = ly - y;
        if (line < y || line >= y + boundH || row < 0)
            continue;

        int mode = (a0 >> 10) & 3;
        if (mode == 3)
            continue;
        int color256 = (a0 >> 13) & 1;
        unsigned tileNum = a2 & 0x3FF;
        if (bitmapMode && tileNum < 512)
            continue;
        int prio = (a2 >> 10) & 3;
        unsigned palBank = (a2 >> 12) & 0xF;
        unsigned rowStride = oneD ? (unsigned)(w >> 3) * (color256 ? 2 : 1) : 32;

        int16_t pa = 0x100, pb = 0, pc = 0, pd = 0x100;
        int hflip = 0, vflip = 0;
        if (affine) {
            unsigned p = ((a1 >> 9) & 0x1F) * 32;
            pa = (int16_t)(gPortOam[p + 6] | (gPortOam[p + 7] << 8));
            pb = (int16_t)(gPortOam[p + 14] | (gPortOam[p + 15] << 8));
            pc = (int16_t)(gPortOam[p + 22] | (gPortOam[p + 23] << 8));
            pd = (int16_t)(gPortOam[p + 30] | (gPortOam[p + 31] << 8));
        }
        else {
            hflip = (a1 >> 12) & 1;
            vflip = (a1 >> 13) & 1;
        }

        for (int bx = 0; bx < boundW; bx++) {
            int sx = x + bx;
            if (sx < 0 || sx >= W)
                continue;
            int col = bx;
            if (objMosaic) col -= (sx % mosH);
            int tx, ty;
            if (affine) {
                int cx = col - boundW / 2, cy = row - boundH / 2;
                tx = ((pa * cx + pb * cy) >> 8) + w / 2;
                ty = ((pc * cx + pd * cy) >> 8) + h / 2;
                if (tx < 0 || ty < 0 || tx >= w || ty >= h)
                    continue;
            }
            else {
                tx = hflip ? w - 1 - col : col;
                ty = vflip ? h - 1 - row : row;
                if (tx < 0 || tx >= w) continue;
            }

            unsigned tile = tileNum + (ty >> 3) * rowStride + (tx >> 3) * (color256 ? 2 : 1);
            unsigned idx;
            if (color256) {
                unsigned a = 0x10000 + ((tile & 0x3FF) * 32) + (ty & 7) * 8 + (tx & 7);
                idx = a < 0x18000 ? gPortVram[a] : 0;
            }
            else {
                unsigned a = 0x10000 + ((tile & 0x3FF) * 32) + (ty & 7) * 4 + ((tx & 7) >> 1);
                idx = a < 0x18000 ? gPortVram[a] : 0;
                idx = (tx & 1) ? (idx >> 4) : (idx & 0xF);
            }
            if (!idx)
                continue;

            if (mode == 2) { // object window: shape only
                sObj.win[sx] = 1;
                continue;
            }
            if (prio < sObj.prio[sx]) {
                sObj.color[sx] = Pal(256 + (color256 ? idx : palBank * 16 + idx));
                sObj.prio[sx] = (uint8_t)prio;
                sObj.semi[sx] = (mode == 1);
            }
        }
    }
}

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

static int InWindowRange(int v, int lo, int hi, int limit)
{
    if (hi > limit || lo > hi) {
        // Hardware treats out-of-range or inverted bounds as a wrapped window.
        if (hi > limit && lo <= hi) hi = limit;
        else return v >= lo || v < hi;
    }
    return v >= lo && v < hi;
}

void Ppu_RenderLine(int line, uint16_t *dst)
{
    uint16_t dispcnt = R16(0x00);
    int mode = dispcnt & 7;

    if (dispcnt & 0x80) { // forced blank: white
        for (int x = 0; x < W; x++) dst[x] = 0x7FFF;
        for (int i = 0; i < 2; i++) { AffineReloadIfWritten(i); AffineAdvance(i); }
        return;
    }

    for (int i = 0; i < 2; i++)
        AffineReloadIfWritten(i);

    int bgOn[4] = { 0, 0, 0, 0 };
    switch (mode) {
    case 0:
        for (int b = 0; b < 4; b++)
            if (dispcnt & (0x100 << b)) { bgOn[b] = 1; RenderTextBg(b, line, sBg[b]); }
        break;
    case 1:
        for (int b = 0; b < 2; b++)
            if (dispcnt & (0x100 << b)) { bgOn[b] = 1; RenderTextBg(b, line, sBg[b]); }
        if (dispcnt & 0x400) { bgOn[2] = 1; RenderAffineBg(2, sBg[2]); }
        break;
    case 2:
        if (dispcnt & 0x400) { bgOn[2] = 1; RenderAffineBg(2, sBg[2]); }
        if (dispcnt & 0x800) { bgOn[3] = 1; RenderAffineBg(3, sBg[3]); }
        break;
    case 3:
        if (dispcnt & 0x400) { bgOn[2] = 1; RenderMode3(line, sBg[2]); }
        break;
    default:
        break; // modes 4/5 are not used by pmd-red
    }

    RenderObjects(line, dispcnt);

    int bgPrio[4];
    for (int b = 0; b < 4; b++)
        bgPrio[b] = R16(0x08 + b * 2) & 3;

    uint16_t bldcnt = R16(0x50);
    uint16_t bldalpha = R16(0x52);
    int eva = bldalpha & 0x1F, evb = (bldalpha >> 8) & 0x1F;
    int evy = R16(0x54) & 0x1F;
    if (eva > 16) eva = 16;
    if (evb > 16) evb = 16;
    if (evy > 16) evy = 16;
    int effect = (bldcnt >> 6) & 3;

    int win0 = (dispcnt >> 13) & 1, win1 = (dispcnt >> 14) & 1, winObj = (dispcnt >> 15) & 1;
    int anyWin = win0 | win1 | winObj;
    uint16_t win0h = R16(0x40), win1h = R16(0x42), win0v = R16(0x44), win1v = R16(0x46);
    uint16_t winin = R16(0x48), winout = R16(0x4A);
    int in0V = win0 && InWindowRange(line, win0v >> 8, win0v & 0xFF, 160);
    int in1V = win1 && InWindowRange(line, win1v >> 8, win1v & 0xFF, 160);

    uint16_t backdrop = Pal(0);

    for (int x = 0; x < W; x++) {
        unsigned winMask = 0x3F; // BG0-3, OBJ, effects
        if (anyWin) {
            if (in0V && InWindowRange(x, win0h >> 8, win0h & 0xFF, 240)) winMask = winin & 0x3F;
            else if (in1V && InWindowRange(x, win1h >> 8, win1h & 0xFF, 240)) winMask = (winin >> 8) & 0x3F;
            else if (winObj && sObj.win[x]) winMask = (winout >> 8) & 0x3F;
            else winMask = winout & 0x3F;
        }

        // Find the top two visible layers.
        int topL = L_BD, secL = L_BD;
        uint16_t topC = backdrop, secC = backdrop;
        int topP = 5, secP = 5;
        for (int p = 0; p < 4; p++) {
            if ((winMask & 0x10) && sObj.prio[x] == p && sObj.color[x] != TRANSPARENT) {
                if (topL == L_BD) { topL = L_OBJ; topC = sObj.color[x]; topP = p; }
                else if (secL == L_BD) { secL = L_OBJ; secC = sObj.color[x]; secP = p; }
            }
            for (int b = 0; b < 4; b++) {
                if (!bgOn[b] || bgPrio[b] != p || !(winMask & (1u << b)) || sBg[b][x] == TRANSPARENT)
                    continue;
                if (topL == L_BD) { topL = b; topC = sBg[b][x]; topP = p; }
                else if (secL == L_BD) { secL = b; secC = sBg[b][x]; secP = p; }
            }
            if (secL != L_BD)
                break;
        }
        (void)topP; (void)secP;

        uint16_t c = topC;
        int effectsOn = (winMask & 0x20) != 0;
        int topIsTarget1 = (bldcnt >> topL) & 1;
        int secIsTarget2 = (bldcnt >> (8 + secL)) & 1;

        if (topL == L_OBJ && sObj.semi[x] && secIsTarget2) {
            c = Blend(topC, secC, eva, evb); // semi-transparent sprites always alpha blend
        }
        else if (effectsOn && topIsTarget1) {
            if (effect == 1 && secIsTarget2) c = Blend(topC, secC, eva, evb);
            else if (effect == 2) c = Brighten(topC, evy);
            else if (effect == 3) c = Darken(topC, evy);
        }
        dst[x] = c;
    }

    for (int i = 0; i < 2; i++)
        AffineAdvance(i);
}
