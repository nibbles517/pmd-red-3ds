// The picture's size and the wide view (shared by the port core and the
// patched game code, so plain C with stdint only).
//
// The 3DS picture is DS-sized, 256x192. Most of the game draws its 240x160
// picture in the middle of it, with a black border. Dungeon floors and ground
// maps use the wide view: the game draws more of the world around the same
// spot, the GBA screen's (0,0) at screen (WIDE_OX, 0) -- the camera already
// keeps the player 96 lines down, the DS screen's middle.
#ifndef PORT_VIEW_H
#define PORT_VIEW_H

#include <stdint.h>

#define SCREEN_W 256
#define SCREEN_H 192
#define WIDE_OX 8       // screen x of GBA x 0 in the wide view
#define NARROW_OX 8     // and in the narrow (GBA-sized) view
#define NARROW_OY 16

// Wide view: on while a dungeon floor or a ground map is shown (set by the
// game patches; read when a frame starts).
extern uint8_t gPortWideView;

// Backgrounds drawn by the game for the wide view, instead of from VRAM: a
// 64x32-entry map (512x256 pixels, wrapping) and its scroll, in GBA screen
// coordinates (the pixel at GBA screen (0,0) is map pixel (scrollX, scrollY)).
#define PORT_WIDE_BLACK 0xFFFF // map entry drawn opaque black
struct PortWideBg
{
    uint16_t *map;  // NULL: the background is drawn from VRAM as usual
    int32_t scrollX, scrollY;
};
extern struct PortWideBg gPortWideBg[4];

#endif
