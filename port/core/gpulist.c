// Drawing list (gpulist.h): building it, and drawing it in software.

#include <string.h>
#include "gpulist.h"
#include "bgcache.h"

GpuList gGpuList;
uint16_t gGpuStrip[GPU_STRIP_ROWS][GPU_STRIP_W];
uint16_t gGpuObj[GPU_OBJ_H][GPU_OBJ_W];

const char *const gGpuWhy[GPU_WHY_COUNT] = {
    "none", "blank", "mode", "window", "effect", "obj-change", "obj-affine", "obj-semi", "obj-mosaic", "full", "obj-window",
};

// The rectangles each layer added on the line before (to be made a line
// taller when this line continues them)
#define LAST_MAX 12
static int16_t sLast[4][LAST_MAX];
static int sLastCount[4], sLastLine[4];
static int16_t sNow[4][LAST_MAX]; // ... and this line's, so far
static int sNowCount[4], sNowLine[4];

void GpuList_Begin(int w, int h)
{
    GpuList *l = &gGpuList;
    l->w = w;
    l->h = h;
    l->backdrop = 0;
    l->bdSecond = 0;
    l->eva = 16;
    l->evb = 0;
    l->quadCount = 0;
    l->stripRows = 0;
    l->objX = l->objY = l->objRowH = 0;
    l->full = 0;
    l->fallback = 0;
    for (int b = 0; b < 4; b++) {
        sLastCount[b] = sNowCount[b] = 0;
        sLastLine[b] = -2;
        sNowLine[b] = -1;
    }
}

// Adds a rectangle one line tall, or makes one of the line before's taller
static void Add(int line, int bg, int tex, uint16_t key, int flags, int dx, int w, int sx, int sy)
{
    GpuList *l = &gGpuList;
    if (sLastLine[bg] == line - 1) {
        for (int i = 0; i < sLastCount[bg]; i++) {
            GpuQuad *q = &l->quads[sLast[bg][i]];
            if (q->tex == tex && q->key == key && q->flags == flags && q->dx == dx && q->w == w && q->sx == sx
                && q->dy + q->h == line && q->sy + q->h == sy) {
                q->h++;
                if (sNowCount[bg] < LAST_MAX)
                    sNow[bg][sNowCount[bg]++] = sLast[bg][i];
                return;
            }
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
    if (sNowCount[bg] < LAST_MAX)
        sNow[bg][sNowCount[bg]++] = (int16_t)l->quadCount;
    l->quadCount++;
}

// A layer's first piece on a line: the rectangles it added on its last line
// become the ones to continue (sNow: this line's, so far)
static void StartLine(int line, int bg)
{
    if (sNowLine[bg] != line) {
        // a new line for this layer: the last one's rectangles are the ones to continue
        if (sNowLine[bg] >= 0) {
            memcpy(sLast[bg], sNow[bg], sizeof(sNow[bg]));
            sLastCount[bg] = sNowCount[bg];
            sLastLine[bg] = sNowLine[bg];
        }
        sNowLine[bg] = line;
        sNowCount[bg] = 0;
    }
}

void GpuList_BgLine(int line, int bg, uint16_t key, int flags, int y, int x0, int wPx, int xa, int xb)
{
    StartLine(line, bg);
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
    StartLine(line, bg);
    if (xb <= xa)
        return;
    // One strip row per layer and line (each of its pieces at its own x)
    int row = -1;
    for (int i = 0; i < sNowCount[bg]; i++) {
        const GpuQuad *q = &l->quads[sNow[bg][i]];
        if (q->tex == GTEX_STRIP && q->dy + q->h - 1 == line) {
            row = q->sy + q->h - 1;
            break;
        }
    }
    if (row < 0) {
        if (l->stripRows >= GPU_STRIP_ROWS) {
            l->full = 1;
            return;
        }
        row = l->stripRows++;
    }
    memcpy(&gGpuStrip[row][xa], &px[xa], (xb - xa) * sizeof(uint16_t));
    Add(line, bg, GTEX_STRIP, key, flags, xa, xb - xa, xa, row);
}

uint16_t *GpuList_Obj(int x, int y, int w, int h, int flags, uint16_t key)
{
    GpuList *l = &gGpuList;
    if (l->objX + w > GPU_OBJ_W) { // the next row of rectangles
        l->objY += l->objRowH;
        l->objX = 0;
        l->objRowH = 0;
    }
    if (l->objY + h > GPU_OBJ_H || l->quadCount >= GPU_MAX_QUADS) {
        l->full = 1;
        return NULL;
    }
    GpuQuad *q = &l->quads[l->quadCount++];
    q->tex = GTEX_OBJ;
    q->flags = (uint8_t)flags;
    q->key = key;
    q->dx = (int16_t)x;
    q->dy = (int16_t)y;
    q->w = (int16_t)w;
    q->h = (int16_t)h;
    q->sx = (int16_t)l->objX;
    q->sy = (int16_t)l->objY;
    uint16_t *place = &gGpuObj[l->objY][l->objX];
    l->objX += w;
    if (h > l->objRowH)
        l->objRowH = h;
    return place;
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

static const uint16_t *Texel0(int tex, int *stride)
{
    if (tex <= GTEX_BG3) {
        *stride = BGC_STRIDE;
        return gBgCache[tex].px;
    }
    if (tex == GTEX_STRIP) {
        *stride = GPU_STRIP_W;
        return &gGpuStrip[0][0];
    }
    *stride = GPU_OBJ_W;
    return &gGpuObj[0][0];
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
        int ts;
        const uint16_t *tex = Texel0(q->tex, &ts);
        for (int j = 0; j < q->h; j++) {
            int y = q->dy + j;
            if (y < 0 || y >= l->h)
                continue;
            int ty = q->sy + ((q->flags & GQ_VFLIP) ? q->h - 1 - j : j);
            const uint16_t *row = tex + ty * ts;
            uint16_t *d = dst + y * stride;
            uint8_t *sec = second + y * l->w;
            uint8_t mark = (q->flags & GQ_SECOND) ? 1 : 0;
            for (int i = 0; i < q->w; i++) {
                int x = q->dx + i;
                if (x < 0 || x >= l->w)
                    continue;
                uint16_t t = row[q->sx + ((q->flags & GQ_HFLIP) ? q->w - 1 - i : i)];
                if (t & 0x8000)
                    continue;
                d[x] = (q->flags & GQ_BLEND) && sec[x] ? Blend(t, d[x], l->eva, l->evb) : t;
                sec[x] = mark;
            }
        }
    }
}
