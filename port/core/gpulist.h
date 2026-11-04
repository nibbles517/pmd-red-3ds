// Drawing list: a frame of the main screen as rectangles for the GPU.
//
// Stage 1 of drawing on the GPU (docs/GPU.md). For a frame that only uses
// what the list can express, the renderer (ppu.c: Ppu_ListLine,
// Ppu_ListObjects) describes the frame instead of drawing it:
//   - the backgrounds as rectangles of their decoded maps (bgcache.h: each
//     layer's px, 512 pixels a row), one per run of lines that read the
//     same columns of consecutive rows (so a whole background is usually
//     one to four rectangles);
//   - what can't be read from a map that way (the wide view's HUD row, BG1's
//     side tiles, the full-screen tiled effects, lines decoded the slow way)
//     as line strips: those lines' pixels, copied into the strip texture;
//   - each sprite as one rectangle of the sprite texture, into which its
//     pixels are coloured (flips are the rectangle's).
// Each rectangle has a key: what is in front of what (smaller: in front).
// Drawn back to front over the backdrop colour, with transparent texels
// (bit 15) left out, the rectangles make the picture the renderer would.
//
// Windows: a layer's pieces of a line are cut to where the windows show it.
// Alpha blending: each pixel keeps a flag (the GPU's stencil): whether what
// is there is a second target. A rectangle that is a first target (GQ_BLEND,
// where the windows allow the effect) blends with what is under it where the
// flag is set (eva/evb), else covers it; each rectangle sets the flag to
// whether it is a second target (GQ_SECOND). As the GBA blends a pixel's top
// layer with the one under it, this is exact while no layer is both a first
// and a second target (else the renderer draws the frame).
//
// GpuList_Draw draws a list in software (the test harness checks it against
// the renderer, frame by frame); the 3DS will hand it to the GPU.

#ifndef GPULIST_H
#define GPULIST_H

#include <stdint.h>
#include "port.h"

enum {
    GTEX_BG0, GTEX_BG1, GTEX_BG2, GTEX_BG3, // gBgCache[n].px (BGC_STRIDE pixels a row, 512 rows)
    GTEX_STRIP,                             // GPU_STRIP_W pixels a row, GPU_STRIP_ROWS rows
    GTEX_OBJ,                               // GPU_OBJ_W x GPU_OBJ_H
};
#define GPU_STRIP_W 512
#define GPU_STRIP_ROWS 512
#define GPU_OBJ_W 512
#define GPU_OBJ_H 256
#define GPU_MAX_QUADS 1024

enum { GQ_HFLIP = 1, GQ_VFLIP = 2, GQ_BLEND = 4, GQ_SECOND = 8 };

typedef struct {
    uint8_t tex, flags; // GQ_*
    uint16_t key;      // in front: smaller (see GpuKeyBg / GpuKeyObj)
    int16_t dx, dy;    // where on the picture (sprites can start off it)
    int16_t w, h;
    int16_t sx, sy;    // the texture's texel at the rectangle's top left (before flips)
} GpuQuad;

// Within a priority, sprites (lower OAM index in front) before BG0..BG3
static inline uint16_t GpuKeyObj(int prio, int oam) { return (uint16_t)(prio * 256 + oam); }
static inline uint16_t GpuKeyBg(int prio, int bg) { return (uint16_t)(prio * 256 + 128 + bg); }

typedef struct {
    int w, h;          // the picture: w x h pixels
    uint16_t backdrop; // BGR555
    uint8_t bdSecond;  // the backdrop is a second target
    uint8_t eva, evb;  // alpha blending's amounts (0-16)
    int quadCount;
    GpuQuad quads[GPU_MAX_QUADS];
    int stripRows;
    int objX, objY, objRowH; // the sprite texture's next free place (rows of rectangles)
    int full;          // something didn't fit: the list can't be used
    int fallback;      // why the renderer couldn't describe the frame (GPU_WHY_*), 0: it could
} GpuList;

enum {
    GPU_WHY_NONE, GPU_WHY_BLANK, GPU_WHY_MODE, GPU_WHY_WINDOW, GPU_WHY_EFFECT, GPU_WHY_OBJ_CHANGE,
    GPU_WHY_OBJ_AFFINE, GPU_WHY_OBJ_SEMI, GPU_WHY_OBJ_MOSAIC, GPU_WHY_FULL, GPU_WHY_OBJ_WINDOW, GPU_WHY_COUNT
};
extern const char *const gGpuWhy[GPU_WHY_COUNT];

extern GpuList gGpuList;
extern uint16_t gGpuStrip[GPU_STRIP_ROWS][GPU_STRIP_W];
extern uint16_t gGpuObj[GPU_OBJ_H][GPU_OBJ_W];

void GpuList_Begin(int w, int h);

// A background's line (ppu.c): screen x xa..xb-1 read from cache layer bg's
// row y, from column x0 at screen x xa (wrapping at the map's width wPx);
// key: its place in front; flags: GQ_BLEND, GQ_SECOND.
void GpuList_BgLine(int line, int bg, uint16_t key, int flags, int y, int x0, int wPx, int xa, int xb);
// ... or its pixels (px: the line's, screen x 0 on), xa..xb-1 of them
void GpuList_StripLine(int line, int bg, uint16_t key, int flags, const uint16_t *px, int xa, int xb);
// A sprite: its w x h pixels go to the returned place (GPU_OBJ_W a row;
// NULL: no room, gGpuList.full set). flags: GQ_*
uint16_t *GpuList_Obj(int x, int y, int w, int h, int flags, uint16_t key);

// Draws the list in software: dst holds the picture (stride pixels a row)
void GpuList_Draw(const GpuList *l, uint16_t *dst, int stride);

#endif
