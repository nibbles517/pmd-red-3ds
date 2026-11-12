// Drawing list (gpulist.h): building it, and drawing it in software.

#include <string.h>
#include <stdio.h>
#include "gpulist.h"
#include "bgcache.h"

GpuList gGpuList;
uint16_t gGpuStrip[GPU_STRIP_ROWS][GPU_STRIP_W];
uint16_t gGpuObj[GPU_OBJ_H][GPU_OBJ_W];

const char *const gGpuWhy[GPU_WHY_COUNT] = {
    "none", "blank", "mode", "window", "effect", "obj-change", "obj-affine", "obj-semi", "obj-mosaic", "full", "obj-window", "obj-full", "strip-full",
};

// Each layer's strip row for its line (the line: sStripLine)
static int sStripRow[4], sStripLine[4];
// The rectangles the last line described added to or made taller (its
// line: sRecLine), for GpuList_RepeatLine
#define REC_MAX 1024
static uint16_t sRec[REC_MAX];
static int sRecN, sRecLine, sRecOver;
static inline void Record(int line, int quad)
{
    if (line != sRecLine) {
        sRecLine = line;
        sRecN = 0;
        sRecOver = 0;
    }
    if (sRecN < REC_MAX)
        sRec[sRecN++] = (uint16_t)quad;
    else
        sRecOver = 1;
}
// Each layer's rectangles by where they start on the screen (x): the one
// whose last line it is (stamp: the frame << 10 | that line)
static uint16_t sDxQuad[4][512];
static uint32_t sDxStamp[4][512];

void GpuList_Begin(int w, int h)
{
    GpuList *l = &gGpuList;
    l->w = w;
    l->h = h;
    l->backdrop = 0;
    l->bdSecond = 0;
    l->border = 0;
    l->eva = 16;
    l->evb = 0;
    l->quadCount = 0;
    l->stripRows = 0;
    l->frame++;
    l->full = 0;
    l->fallback = 0;
    l->fadeOn = 0;
    l->palette = GPU_PAL_STEADY;
    l->objBanks = 0;
    for (int b = 0; b < 4; b++)
        sStripLine[b] = -1;
    sRecLine = -2;
}

// Adds a rectangle one line tall, or makes one of the line before's taller
static void Add(int line, int bg, int tex, uint16_t key, int flags, int dx, int w, int sx, int sy)
{
    GpuList *l = &gGpuList;
    // The rectangle of the line before at the same screen x (a piece can
    // only continue one that starts where it starts): one look, instead of
    // searching the line before's (a dungeon's lines, cut in many pieces by
    // the light's window and most not continuing one, took a fifth of the
    // time describing a frame, searching)
    if (dx >= 0 && dx < 512 && sDxStamp[bg][dx] == (l->frame << 10 | (uint32_t)(line - 1))) {
        GpuQuad *q = &l->quads[sDxQuad[bg][dx]];
        if (q->sx == sx && q->w == w && q->tex == tex && q->key == key && q->flags == flags && q->dy + q->h == line
            && q->sy + q->h == sy) {
            q->h++;
            sDxStamp[bg][dx] = l->frame << 10 | (uint32_t)line;
            Record(line, sDxQuad[bg][dx]);
            return;
        }
    }
    if (l->quadCount >= GPU_MAX_QUADS) {
        l->full = 1;
        return;
    }
    GpuQuad *q = &l->quads[l->quadCount];
    q->tex = (uint8_t)tex;
    q->flags = (uint8_t)flags;
    q->key = key;
    q->dx = (int16_t)dx;
    q->dy = (int16_t)line;
    q->w = (int16_t)w;
    q->h = 1;
    q->sx = (int16_t)sx;
    q->sy = (int16_t)sy;
    if (dx >= 0 && dx < 512) {
        sDxQuad[bg][dx] = (uint16_t)l->quadCount;
        sDxStamp[bg][dx] = l->frame << 10 | (uint32_t)line;
    }
    Record(line, l->quadCount);
    l->quadCount++;
}

int GpuList_RepeatLine(int line)
{
    GpuList *l = &gGpuList;
    if (sRecLine != line - 1 || sRecOver)
        return 0;
    // Each must be a background's piece ending on the line before, whose
    // next row is in the same row of tiles (decoded with the one before)
    for (int i = 0; i < sRecN; i++) {
        const GpuQuad *q = &l->quads[sRec[i]];
        if (q->tex > GTEX_BG3 || q->dy + q->h != line || ((q->sy + q->h) & 7) == 0)
            return 0;
    }
    for (int i = 0; i < sRecN; i++) {
        GpuQuad *q = &l->quads[sRec[i]];
        q->h++;
        if (q->dx >= 0 && q->dx < 512 && sDxQuad[q->tex - GTEX_BG0][q->dx] == sRec[i])
            sDxStamp[q->tex - GTEX_BG0][q->dx] = l->frame << 10 | (uint32_t)line;
    }
    sRecLine = line;
    return 1;
}

void GpuList_BgLine(int line, int bg, uint16_t key, int flags, int y, int x0, int wPx, int xa, int xb)
{
    int x = x0;
    for (int sx = xa; sx < xb;) {
        int n = wPx - x;
        if (n > xb - sx) n = xb - sx;
        Add(line, bg, GTEX_BG0 + bg, key, flags, sx, n, x, y);
        sx += n;
        x = 0;
    }
}

void GpuList_StripLine(int line, int bg, uint16_t key, int flags, const uint16_t *px, int xa, int xb)
{
    GpuList *l = &gGpuList;
    if (xb <= xa)
        return;
    // One strip row per layer and line (each of its pieces at its own x)
    int row = sStripLine[bg] == line ? sStripRow[bg] : -1;
    if (row < 0) {
        if (l->stripRows >= GPU_STRIP_ROWS) {
            l->full = 2;
            return;
        }
        row = l->stripRows++;
        sStripLine[bg] = line;
        sStripRow[bg] = row;
    }
    memcpy(&gGpuStrip[row][xa], &px[xa], (xb - xa) * sizeof(uint16_t));
    Add(line, bg, GTEX_STRIP, key, flags, xa, xb - xa, xa, row);
}

// The sprite slots: class c (64, 32, 16, 8 pixels) takes a quarter of the
// texture: (0,0), (256,0), (0,256), (256,256); its slots in rows of 256 / size.
GpuObjSlot gGpuObjSlots[GPU_OBJ_SLOTS];
static const int sClassFirst[4] = { 0, 16, 80, 336 }, sClassCount[4] = { 16, 64, 256, 1024 };
static const int sClassSize[4] = { 64, 32, 16, 8 }, sClassX[4] = { 0, 256, 0, 256 }, sClassY[4] = { 0, 0, 256, 256 };
// A hash of looks to slots (index + 1; stale ones are skipped by comparing
// the look); made again when too full of stale ones
#define OBJ_HASH 4096
static uint16_t sObjHash[OBJ_HASH];
static int sObjHashAdds;

static unsigned ObjHashOf(uint32_t look)
{
    return (look * 2654435761u) >> 20; // (12 bits)
}

static void ObjHashAdd(uint32_t look, int slot)
{
    if (++sObjHashAdds > OBJ_HASH / 2) { // remade from the slots
        memset(sObjHash, 0, sizeof(sObjHash));
        sObjHashAdds = 1;
        for (int i = 0; i < GPU_OBJ_SLOTS; i++)
            if (gGpuObjSlots[i].look && i != slot) {
                unsigned h = ObjHashOf(gGpuObjSlots[i].look);
                while (sObjHash[h]) h = (h + 1) & (OBJ_HASH - 1);
                sObjHash[h] = (uint16_t)(i + 1);
                sObjHashAdds++;
            }
    }
    unsigned h = ObjHashOf(look);
    while (sObjHash[h]) h = (h + 1) & (OBJ_HASH - 1);
    sObjHash[h] = (uint16_t)(slot + 1);
}

static int ObjFind(uint32_t look)
{
    for (unsigned h = ObjHashOf(look); sObjHash[h]; h = (h + 1) & (OBJ_HASH - 1)) {
        int i = sObjHash[h] - 1;
        if (gGpuObjSlots[i].look == look)
            return i;
    }
    return -1;
}

uint32_t gGpuObjTileChanged[1024];
static uint32_t sObjVram[1024][8]; // (the copy)

void GpuList_ObjTiles(const uint8_t *objVram)
{
    const uint32_t *v = (const uint32_t *)objVram;
    for (int t = 0; t < 1024; t++, v += 8) {
        uint32_t *c = sObjVram[t];
        if (c[0] != v[0] || c[1] != v[1] || c[2] != v[2] || c[3] != v[3]
            || c[4] != v[4] || c[5] != v[5] || c[6] != v[6] || c[7] != v[7]) {
            memcpy(c, v, 32);
            gGpuObjTileChanged[t] = gGpuList.frame;
        }
    }
}

// Whether sprite rectangles a and b (quads of the sprite texture) have a
// shown pixel in the same place: the GBA shows one sprite's pixel there (the
// front one's), the list draws both (a blended one over the other would blend
// with it, not with what is under the sprites).
int GpuList_ObjOverlap(const GpuQuad *a, const GpuQuad *b)
{
    int x0 = a->dx > b->dx ? a->dx : b->dx, x1 = a->dx + a->w < b->dx + b->w ? a->dx + a->w : b->dx + b->w;
    int y0 = a->dy > b->dy ? a->dy : b->dy, y1 = a->dy + a->h < b->dy + b->h ? a->dy + a->h : b->dy + b->h;
    for (int y = y0; y < y1; y++) {
        int ja = y - a->dy, jb = y - b->dy;
        const uint16_t *ra = gGpuObj[a->sy + ((a->flags & GQ_VFLIP) ? a->h - 1 - ja : ja)];
        const uint16_t *rb = gGpuObj[b->sy + ((b->flags & GQ_VFLIP) ? b->h - 1 - jb : jb)];
        for (int x = x0; x < x1; x++) {
            int ia = x - a->dx, ib = x - b->dx;
            if (!(ra[a->sx + ((a->flags & GQ_HFLIP) ? a->w - 1 - ia : ia)] & 0x8000)
                && !(rb[b->sx + ((b->flags & GQ_HFLIP) ? b->w - 1 - ib : ib)] & 0x8000))
                return 1;
        }
    }
    return 0;
}

uint16_t *GpuList_Obj(int x, int y, int w, int h, int flags, uint16_t key, uint32_t look, uint32_t changed,
                      uint16_t bank, int *made)
{
    GpuList *l = &gGpuList;
    *made = 0;
    if (l->quadCount >= GPU_MAX_QUADS) {
        l->full = 1;
        return NULL;
    }
    int slot = ObjFind(look);
    int build = slot < 0 || changed > gGpuObjSlots[slot].built;
    if (slot < 0) { // a slot of its class: a free one, else the one shown longest ago (not this frame)
        int big = w > h ? w : h, c = big > 32 ? 0 : big > 16 ? 1 : big > 8 ? 2 : 3;
        uint32_t oldest = 0xFFFFFFFF;
        for (int i = sClassFirst[c]; i < sClassFirst[c] + sClassCount[c]; i++) {
            GpuObjSlot *s = &gGpuObjSlots[i];
            if (!s->look) { slot = i; break; }
            if (s->used != l->frame && s->used < oldest) { oldest = s->used; slot = i; }
        }
        if (slot < 0) {
            l->full = 1;
            return NULL;
        }
        GpuObjSlot *s = &gGpuObjSlots[slot];
        int k = slot - sClassFirst[c], perRow = 256 / sClassSize[c];
        s->look = look;
        s->x = (uint16_t)(sClassX[c] + (k % perRow) * sClassSize[c]);
        s->y = (uint16_t)(sClassY[c] + (k / perRow) * sClassSize[c]);
        ObjHashAdd(look, slot);
    }
    GpuObjSlot *s = &gGpuObjSlots[slot];
    s->used = l->frame;
    s->w = (uint16_t)w;
    s->h = (uint16_t)h;
    s->bank = bank;
    GpuQuad *q = &l->quads[l->quadCount++];
    q->tex = GTEX_OBJ;
    q->flags = (uint8_t)flags;
    q->key = key;
    q->dx = (int16_t)x;
    q->dy = (int16_t)y;
    q->w = (int16_t)w;
    q->h = (int16_t)h;
    q->sx = (int16_t)s->x;
    q->sy = (int16_t)s->y;
    if (!build) {
        *made = 1;
        return NULL;
    }
    s->built = l->frame;
    s->gpuDirty = 1;
    return &gGpuObj[s->y][s->x];
}

// The base palette and the fade: every entry the frame shows (the palette
// banks its background tiles and sprites use, and the backdrop) the same fade
// of its own colour, or black staying black under a darkening
static const PortPalFade sNoFade = { 256, { 0, 0, 0 }, { 0, 0, 0 } };

static int IsNoFade(const PortPalFade *f)
{
    return f->mul >= 256 && !f->add[0] && !f->add[1] && !f->add[2];
}

// The BG palette banks the frame's background rectangles show
static unsigned BgBanksUsed(const GpuList *l)
{
    unsigned banks = 1; // (the backdrop: entry 0)
    for (int i = 0; i < l->quadCount && banks != 0xFFFF; i++) {
        const GpuQuad *q = &l->quads[i];
        if (q->tex > GTEX_BG3)
            continue;
        const BgCache *c = &gBgCache[q->tex];
        if (!c->indexOk)
            continue; // (drawn as they are: GQ_PREFADED)
        if (c->color256)
            return 0xFFFF;
        for (int ty = q->sy >> 3; ty <= (q->sy + q->h - 1) >> 3; ty++)
            for (int tx = q->sx >> 3; tx <= (q->sx + q->w - 1) >> 3; tx++) {
                unsigned bank = c->entry[ty * 64 + tx] >> 12, w = tx >> 5;
                uint32_t bit = 1u << (tx & 31);
                if ((banks >> bank) & 1)
                    continue;
                for (int r = 0; r < 8; r++) // (a tile with no opaque pixel shows no colour)
                    if (c->any[ty * 8 + r][w] & bit) {
                        banks |= 1u << bank;
                        break;
                    }
            }
    }
    return banks;
}

static int SameFade(const PortPalFade *a, const PortPalFade *b)
{
    return a->mul == b->mul && a->add[0] == b->add[0] && a->add[1] == b->add[1] && a->add[2] == b->add[2];
}

// Whether the frame shows a pixel of an odd entry: the backdrop, a shown
// background tile's opaque pixels (of the banks with odd entries), a sprite's
static int OddShown(const GpuList *l, const uint8_t *odd, uint32_t oddBanks)
{
    if (odd[0])
        return 1;
    for (int i = 0; i < l->quadCount; i++) {
        const GpuQuad *q = &l->quads[i];
        if (q->tex <= GTEX_BG3) {
            const BgCache *c = &gBgCache[q->tex];
            if (!c->indexOk)
                continue; // (drawn as they are: GQ_PREFADED)
            for (int ty = q->sy >> 3; ty <= (q->sy + q->h - 1) >> 3; ty++)
                for (int tx = q->sx >> 3; tx <= (q->sx + q->w - 1) >> 3; tx++) {
                    if (!c->color256 && !((oddBanks >> (c->entry[ty * 64 + tx] >> 12)) & 1))
                        continue;
                    const uint16_t *px = &c->px[ty * 8 * BGC_STRIDE + tx * 8];
                    const uint8_t *idx = &c->idx[ty * 8 * BGC_STRIDE + tx * 8];
                    for (int r = 0; r < 8; r++, px += BGC_STRIDE, idx += BGC_STRIDE)
                        for (int x = 0; x < 8; x++)
                            if (!(px[x] & 0x8000) && odd[idx[x]] && (c->color256 || idx[x] != 0x10))
                                return 1;
                }
        }
        else if (q->tex == GTEX_OBJ && (oddBanks >> 16)) {
            for (int j = 0; j < q->h; j++)
                for (int x = 0; x < q->w; x++) {
                    uint16_t n = gGpuObj[q->sy + j][q->sx + x];
                    if (!(n & 0x8000) && odd[256 + n])
                        return 1;
                }
        }
    }
    return 0;
}

void GpuList_Palette(void)
{
    GpuList *l = &gGpuList;
    const uint16_t *pal = (const uint16_t *)gPortPltt;
    int changed = 0;
    for (int i = 0; i < 512; i++) {
        uint16_t v = pal[i] & 0x7FFF, base = v;
        if (gPortPalValue[i] == v) // (as the palette buffer made it)
            base = gPortPalBase[i] & 0x7FFF;
        l->base[i] = base;
        changed |= base != v;
    }
    l->fade = sNoFade;
    l->fadeOn = 0;
    l->palette = GPU_PAL_STEADY;
    uint32_t used = BgBanksUsed(l) | ((uint32_t)l->objBanks << 16);
    l->usedBanks = used;
    if (!changed)
        return;

    // The fade: the one most of the shown entries have
    PortPalFade cand[8];
    int candN = 0, candC[8] = { 0 };
    for (int i = 0; i < 512; i++) {
        if (!((used >> (i >> 4)) & 1) || ((i & 15) == 0 && i != 0))
            continue; // (a bank not shown, or a clear colour)
        uint16_t v = pal[i] & 0x7FFF;
        const PortPalFade *fd = gPortPalValue[i] == v ? &gPortPalFade[i] : &sNoFade;
        if (IsNoFade(fd))
            continue;
        int k;
        for (k = 0; k < candN; k++)
            if (SameFade(&cand[k], fd))
                break;
        if (k == candN && candN < 8)
            cand[candN++] = *fd;
        if (k < 8)
            candC[k]++;
    }
    if (candN > 0) {
        int best = 0;
        for (int k = 1; k < candN; k++)
            if (candC[k] > candC[best])
                best = k;
        PortPalFade f = cand[best];
        int addsColour = f.add[0] | f.add[1] | f.add[2];
        // The shown entries that don't follow it (black staying black under a
        // darkening does): are any of them on the screen?
        static uint8_t odd[512];
        uint32_t oddBanks = 0;
        for (int i = 0; i < 512; i++) {
            odd[i] = 0;
            if (!((used >> (i >> 4)) & 1) || ((i & 15) == 0 && i != 0))
                continue;
            uint16_t v = pal[i] & 0x7FFF;
            const PortPalFade *fd = gPortPalValue[i] == v ? &gPortPalFade[i] : &sNoFade;
            if (SameFade(fd, &f) || (IsNoFade(fd) && v == 0 && !addsColour))
                continue;
            odd[i] = 1;
            oddBanks |= 1u << (i >> 4);
        }
        if (!oddBanks || !OddShown(l, odd, oddBanks)) {
            l->fade = f;
            l->fadeOn = 1;
            l->palette = GPU_PAL_FADE;
            return;
        }
    }
    for (int i = 0; i < 512; i++) // (the palettes as they are)
        l->base[i] = pal[i] & 0x7FFF;
    l->palette = GPU_PAL_OTHER;
}

// --- Drawing in software ---------------------------------------------------------

// As the GBA (ppu.c): each channel (a * eva + b * evb) / 16, at most 31
static uint16_t Blend(uint16_t a, uint16_t b, int eva, int evb)
{
    int r = ((a & 31) * eva + (b & 31) * evb) >> 4;
    int g = (((a >> 5) & 31) * eva + ((b >> 5) & 31) * evb) >> 4;
    int bl = (((a >> 10) & 31) * eva + ((b >> 10) & 31) * evb) >> 4;
    if (r > 31) r = 31;
    if (g > 31) g = 31;
    if (bl > 31) bl = 31;
    return (uint16_t)(r | (g << 5) | (bl << 10));
}

// A texel as drawn: the palettes' colours as they are (the renderer's)
static inline uint16_t Texel(int tex, int x, int y)
{
    const uint16_t *pal = (const uint16_t *)gPortPltt;
    if (tex <= GTEX_BG3) { // (from its colour number, as the GPU's)
        const BgCache *c = &gBgCache[tex];
        uint16_t t = c->px[y * BGC_STRIDE + x];
        if ((t & 0x8000) || !c->indexOk)
            return t;
        uint8_t n = c->idx[y * BGC_STRIDE + x];
        return (!c->color256 && n == 0x10) ? 0 : (pal[n] & 0x7FFF);
    }
    if (tex == GTEX_STRIP)
        return gGpuStrip[y][x];
    uint16_t n = gGpuObj[y][x];
    return (n & 0x8000) ? 0x8000 : (pal[256 + n] & 0x7FFF);
}

void GpuList_Draw(const GpuList *l, uint16_t *dst, int stride)
{
    // Back to front: the keys from the largest down (each key's in the order added)
    static uint16_t order[GPU_MAX_QUADS], start[1024];
    memset(start, 0, sizeof(start));
    for (int i = 0; i < l->quadCount; i++)
        start[1023 - (l->quads[i].key & 1023)]++;
    for (int k = 0, sum = 0; k < 1024; k++) {
        int c = start[k];
        start[k] = (uint16_t)sum;
        sum += c;
    }
    for (int i = 0; i < l->quadCount; i++)
        order[start[1023 - (l->quads[i].key & 1023)]++] = (uint16_t)i;

    static uint8_t second[SCREEN_W_MAX * SCREEN_H_MAX]; // (the GPU's stencil)
    for (int y = 0; y < l->h; y++)
        for (int x = 0; x < l->w; x++)
            dst[y * stride + x] = l->backdrop;
    memset(second, l->bdSecond, (size_t)l->w * l->h);

    for (int n = 0; n < l->quadCount; n++) {
        const GpuQuad *q = &l->quads[order[n]];
        for (int j = 0; j < q->h; j++) {
            int y = q->dy + j;
            if (y < 0 || y >= l->h)
                continue;
            int ty = q->sy + ((q->flags & GQ_VFLIP) ? q->h - 1 - j : j);
            uint16_t *d = dst + y * stride;
            uint8_t *sec = second + y * l->w;
            uint8_t mark = (q->flags & GQ_SECOND) ? 1 : 0;
            for (int i = 0; i < q->w; i++) {
                int x = q->dx + i;
                if (x < 0 || x >= l->w)
                    continue;
                uint16_t t = Texel(q->tex, q->sx + ((q->flags & GQ_HFLIP) ? q->w - 1 - i : i), ty);
                if (t & 0x8000)
                    continue;
                d[x] = (q->flags & GQ_BLEND) && sec[x] ? Blend(t, d[x], l->eva, l->evb) : t;
                sec[x] = mark;
            }
        }
    }
}
