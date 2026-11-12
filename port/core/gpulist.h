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
//   - each sprite as one rectangle of the sprite texture, which holds its
//     pixels' colour numbers (flips are the rectangle's). A sprite keeps its
//     place in the texture from frame to frame (gGpuObjSlots) while its tiles
//     don't change, so it isn't made again (nor sent to the GPU again).
// Colours: the backgrounds' textures (from their caches' colour numbers),
// the sprites' and the backdrop are coloured from the list's base palette;
// the strips (and the backgrounds whose cache has no colour numbers) are
// pixels, already coloured (GQ_PREFADED). Fades: when every palette entry is
// the same fade of a colour of its own (port_view.h: gPortPalFade), the base
// palette is those colours and the fade is the list's (the GPU does it: the
// textures don't change during a fade); otherwise the base palette is the
// palettes as they are (no fade). Drawn in software, every colour is the
// palettes' as they are (the renderer's).
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
#define GPU_OBJ_H 512
#define GPU_MAX_QUADS 4096

enum { GQ_HFLIP = 1, GQ_VFLIP = 2, GQ_BLEND = 4, GQ_SECOND = 8, GQ_PREFADED = 16 };

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
    uint16_t base[512];// the base palette (BGR555): what the textures are coloured from
    PortPalFade fade;  // ... and the fade on it (mul 256, add 0: none)
    int fadeOn;        // (the fade isn't none)
    int palette;       // GPU_PAL_* (the meter)
    uint16_t objBanks; // the OBJ palette banks the sprites use (bit n: bank n)
    uint32_t usedBanks;// the palette banks the frame shows (bits 0-15 BG, 16-31 OBJ: GpuList_Palette)
    int quadCount;
    GpuQuad quads[GPU_MAX_QUADS];
    int stripRows;
    uint32_t frame;    // (counts the listed frames: the sprite slots' ages)
    int full;          // something didn't fit: the list can't be used
    int fallback;      // why the renderer couldn't describe the frame (GPU_WHY_*), 0: it could
    uint16_t border;   // the framed view's border colour (BGR555) | 0x8000; 0: black (gPortBorderFill)
} GpuList;

enum {
    GPU_WHY_NONE, GPU_WHY_BLANK, GPU_WHY_MODE, GPU_WHY_WINDOW, GPU_WHY_EFFECT, GPU_WHY_OBJ_CHANGE,
    GPU_WHY_OBJ_AFFINE, GPU_WHY_OBJ_SEMI, GPU_WHY_OBJ_MOSAIC, GPU_WHY_FULL, GPU_WHY_OBJ_WINDOW, GPU_WHY_OBJ_FULL, GPU_WHY_STRIP_FULL, GPU_WHY_COUNT
};
extern const char *const gGpuWhy[GPU_WHY_COUNT];

enum { GPU_PAL_STEADY, GPU_PAL_FADE, GPU_PAL_OTHER }; // the palettes: as their base; a fade of it; neither

extern GpuList gGpuList;
extern uint16_t gGpuStrip[GPU_STRIP_ROWS][GPU_STRIP_W];
extern uint16_t gGpuObj[GPU_OBJ_H][GPU_OBJ_W]; // OBJ colour numbers (0-255), 0x8000: transparent

void GpuList_Begin(int w, int h);
// The base palette and the fade (once the frame is listed: from the palette
// banks it uses; the palettes don't change during a frame)
void GpuList_Palette(void);
void Port_GpuListCounts(uint32_t *listed, uint32_t why[GPU_WHY_COUNT]); // (frame.c)

// A background's line (ppu.c): screen x xa..xb-1 read from cache layer bg's
// row y, from column x0 at screen x xa (wrapping at the map's width wPx);
// key: its place in front; flags: GQ_BLEND, GQ_SECOND.
void GpuList_BgLine(int line, int bg, uint16_t key, int flags, int y, int x0, int wPx, int xa, int xb);
// Line `line` is the line before's again, a row further down each
// background (frame.c: its registers and windows the same): the line
// before's rectangles made a line taller. 0: not possible (described anew).
int GpuList_RepeatLine(int line);
// ... or its pixels (px: the line's, screen x 0 on), xa..xb-1 of them
void GpuList_StripLine(int line, int bg, uint16_t key, int flags, const uint16_t *px, int xa, int xb);
// A sprite at (x, y), w x h, whose look is `look` (its tiles, size, colours,
// palette bank: GpuObjLook): its rectangle, on its slot of the sprite texture.
// Returns where its pixels go (GPU_OBJ_W a row) if they must be made (a new
// slot, or its tiles changed since: changed, the frame they last did), else
// NULL with *made set; NULL with gGpuList.full set: no room. bank: its
// palette bank (0xFFFF: 256 colours).
uint16_t *GpuList_Obj(int x, int y, int w, int h, int flags, uint16_t key, uint32_t look, uint32_t changed,
                      uint16_t bank, int *made);
// The OBJ tiles (sprite VRAM, 1024 of 32 bytes): the frame each last changed
// (GpuList_ObjTiles compares them with a copy: once a frame, before the sprites)
extern uint32_t gGpuObjTileChanged[1024];
int GpuList_ObjOverlap(const GpuQuad *a, const GpuQuad *b);
void GpuList_ObjTiles(const uint8_t *objVram);
static inline uint32_t GpuObjLook(unsigned tile, int shape, int size, int color256, unsigned bank, int oneD)
{
    return 0x80000000u | (tile & 0x3FF) | ((unsigned)shape << 10) | ((unsigned)size << 12) | ((unsigned)color256 << 14)
         | ((bank & 15) << 15) | ((unsigned)oneD << 19);
}

// The sprite texture's slots: four size classes (64, 32, 16, 8 pixels), a
// quarter of the texture each
#define GPU_OBJ_SLOTS (16 + 64 + 256 + 1024)
typedef struct {
    uint32_t look;     // (0: free)
    uint32_t used;     // the frame it was last shown
    uint32_t built;    // the frame its pixels were made
    uint16_t x, y;     // its place in the sprite texture
    uint16_t w, h;
    uint16_t bank;     // its palette bank (0xFFFF: 256 colours)
    uint8_t gpuDirty;  // made since the GPU's texture last had it
} GpuObjSlot;
extern GpuObjSlot gGpuObjSlots[GPU_OBJ_SLOTS];

// Draws the list in software: dst holds the picture (stride pixels a row)
void GpuList_Draw(const GpuList *l, uint16_t *dst, int stride);

#endif
