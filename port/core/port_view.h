// The picture's size and the wide view (shared by the port core and the
// patched game code, so plain C with stdint only).
//
// Main Screen Settings > Size: DS, a 256x192 picture; Fit, 320x240 (the
// bottom screen's size). Most of the game draws its 240x160 picture in the
// middle of it, with a black border. Dungeon floors and ground maps use the
// wide view: the game draws more of the world around the same spot, the GBA
// screen's (0,0) at screen (WIDE_OX, 0), the camera keeping the player near
// the screen's middle (SCREEN_H / 2 lines down).
#ifndef PORT_VIEW_H
#define PORT_VIEW_H

#include <stdint.h>

#define SCREEN_W_MAX 320
#define SCREEN_H_MAX 240
// The current size (DS or Fit; 256x192 in GBA size), kept up to date by
// Port_UpdateScreenSize() (port/core/options.c) whenever the settings change.
extern int gPortScreenW, gPortScreenH;
void Port_UpdateScreenSize(void);
#define SCREEN_W gPortScreenW
#define SCREEN_H gPortScreenH
#define WIDE_OX ((SCREEN_W - 240) / 2) // screen x of GBA x 0 in the wide view
#define NARROW_OX WIDE_OX               // and in the narrow (GBA-sized) view
#define NARROW_OY ((SCREEN_H - 160) / 2)

// The sprites' y, whole (OAM keeps 8 bits, too few for 240 lines): written
// with OAM by the game (sprite.c); used for an entry while its low 8 bits
// still match the entry's y.
extern int16_t gPortOamY[128];

// Window 1's edges in screen x (wide view) for lines where the game worked
// them out from positions past the GBA screen's edges, which WIN1H cannot
// hold (window_buffer.c: the dim outside a room on dark floors). Used for a
// line while `valid` and its WIN1H is still `reg`; lo > hi: inverted (the
// window is x >= lo or x < hi).
struct PortWinX
{
    uint16_t reg;
    uint8_t valid;
    int16_t lo, hi;
};
extern struct PortWinX gPortWin1X[SCREEN_H_MAX];

// Wide view: on while a dungeon floor or a ground map is shown (set by the
// game patches; read when a frame starts).
extern uint8_t gPortWideView;

// Backgrounds drawn by the game for the wide view, instead of from VRAM: a
// 64x64-entry map (512x512 pixels, wrapping) and its scroll, in GBA screen
// coordinates (the pixel at GBA screen (0,0) is map pixel (scrollX, scrollY)).
#define PORT_WIDE_BLACK 0xFFFF // map entry drawn opaque black
struct PortWideBg
{
    uint16_t *map;  // NULL: the background is drawn from VRAM as usual
    int32_t scrollX, scrollY;
};
extern struct PortWideBg gPortWideBg[4];

// Backgrounds drawn from VRAM as a GBA-sized picture repeated over the wide
// screen, one copy centred (full-screen move effects), while unscrolled.
extern uint8_t gPortTileBg[4];

// Wide view: BG0's top row (the dungeon's floor, level and HP) drawn this
// many lines lower (set by the game in dungeons; 0 elsewhere).
extern uint8_t gPortHudLines;

#endif
