// Background cache: each text background's map kept decoded, as pixels.
//
// Drawing a text background line by line decodes every tile row it crosses
// (map entry, tile data, palette) every frame, although from one frame to the
// next almost nothing in a map changes: mostly only the scroll. So each
// background's whole map (up to 512x512 pixels) is kept decoded, and a line
// is copied out of it at the line's scroll.
//
// What changed is found by comparison, not by watching writes: at the start
// of each drawn frame (BgCache_Prepare) VRAM's tile data and the background
// palettes are compared with copies kept from the last time, and each cached
// map entry with the map; the tiles whose entry or tile data changed are
// marked, and decoded again the first time a line reaches them (so a tile
// that is not on the screen costs nothing).
//
// The colour numbers are kept too: when only the palettes change (the
// game's screen fades change them every frame, colour cycles some banks),
// nothing is decoded again; the tiles using the palettes that changed have
// their colours made again from the numbers (when a line reaches them).
//
// A cache is made for the settings a background has when the frame starts
// (its BGxCNT: tile and map bases, colours, size; or the wide view's map).
// A line where they differ (changed during the frame), or with mosaic, is
// drawn the usual way.

#ifndef BGCACHE_H
#define BGCACHE_H

#include <stdint.h>

#define BGC_STRIDE 512 // pixels per cached row (the widest map)

typedef struct {
    int valid;                // made for the settings below
    const uint16_t *wideMap;  // the wide view's map (64x64 entries in RAM), or NULL: VRAM
    unsigned charBase, screenBase;
    int color256, size;       // BGxCNT's (size: 0-3)
    int edge;                 // the wide view's EDGE_TILE drawn transparent (ppu.c)
    int tilesW, tilesH;       // the map's size in tiles (32 or 64)
    // Bit tx of word pair [ty] / [y] (tile column tx; tx 32-63 in the second word):
    uint32_t todo[64][2];     // tile (tx, ty) to be decoded before use
    uint32_t recolor[64][2];  // ... its colours to be made again from idx (the palettes changed)
    uint32_t any[512][2];     // pixel row y of tile column tx has an opaque pixel
    uint32_t full[512][2];    // ... is opaque in all 8 pixels
    uint16_t entry[64 * 64];  // the map entry each tile was decoded from (ty * 64 + tx)
    int byIndex;              // most palettes changed (a fade): the lines look the colours up,
                              // until (after it) the colours are all made again, a part a frame
    int catchUp, catchStart;  // ... tile rows done so far, from the first one on the screen
    int indexOk;              // idx stands for every pixel (not so: a 256-colour wide map's black)
    uint16_t table[256];      // the colour of each idx (the palettes as they are now)
    uint32_t table32[256];    // (the same, a word each: quicker to look up)
    // Drawing on the GPU (gpulist.h): tiles whose colour numbers changed
    // since the backend last made its texture's (set by Decode; by Recolor
    // too for a cache without colour numbers; the backend adds those whose
    // palette bank's base colours changed)
    uint32_t gpuDirty[64][2];
    int gpuAny;
    uint16_t px[BGC_STRIDE * 512]; // the pixels (TRANSPARENT: bit 15)
    uint8_t idx[BGC_STRIDE * 512]; // ... as colour numbers: 4bpp bank * 16 + n (n 0: clear;
                              // 0x10: the wide map's black), 256 colours the number itself
} BgCache;

extern BgCache gBgCache[4];

// At the start of a drawn frame, before any line: the
// caches for the frame's settings (regs: its I/O registers; wide: the wide
// view), and what changed since the last drawn frame marked.
void BgCache_Prepare(const uint8_t *regs, int wide);

// Decodes tile (tx, ty) of cache c and clears its todo bit.
void BgCache_Decode(BgCache *c, int tx, int ty);
// Makes tile (tx, ty)'s colours again from its colour numbers; clears its recolor bit.
void BgCache_Recolor(BgCache *c, int tx, int ty);

#endif
