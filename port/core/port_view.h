// The picture's size and the wide view (shared by the port core and the
// patched game code, so plain C with stdint only).
//
// Main Screen Settings > Size: DS, a 256x192 picture; Fit, 320x240 (the
// bottom screen's size); 3DS, 400x240 (the top screen's; Fit while the game
// is on the bottom screen). Most of the game draws its 240x160 picture in the
// middle of it, with a black border. Dungeon floors and ground maps use the
// wide view: the game draws more of the world around the same spot, the GBA
// screen's (0,0) at screen (WIDE_OX, 0), the camera keeping the player near
// the screen's middle (SCREEN_H / 2 lines down).
#ifndef PORT_VIEW_H
#define PORT_VIEW_H

#include <stdint.h>

#define SCREEN_W_MAX 400
#define SCREEN_H_MAX 240
// The current size (DS, Fit or 3DS; 256x192 in GBA size), kept up to date by
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

// The framed view's border in the picture's own colour (near its top left)
// instead of black: set by the game while a white screen shows (the opening
// warning and Nintendo screens), so it covers the whole drawing size.
extern uint8_t gPortBorderFill;

// The title screen in the wide view (set by the game every frame it shows
// it, like gPortWideView): its sea carried on to the sides (past the GBA's
// sea band) and down to the screen's bottom (frame.c: ListTitleSea on the
// GPU, ExtendTitleSea when drawn by the CPU).
extern uint8_t gPortTitleView;

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
// ... then also drawn this many pixels further left, and its HP bar (BG0
// tiles from column 18) from gPortHudBar instead: 4bpp tiles of 8 rows, one
// pixel per HP, up to PORT_HUD_BAR_MAX pixels (the game's own: 96).
extern uint8_t gPortHudLeft;
#define PORT_HUD_BAR_MAX 160
extern uint32_t gPortHudBar[PORT_HUD_BAR_MAX];

// GPU Drawing's fades (docs/GPU.md): for each palette entry as it is in
// palette memory, the colour it was made from (its base) and the fade that
// made it: each channel base * mul / 256 + add (add: 0-255, on the 8-bit
// scale; mul 256 and add 0: no fade). Kept by the game's palette buffer
// (bg_palette_buffer.c) and copied when the buffer is (value: the colour then;
// an entry whose palette memory differs was written some other way).
typedef struct {
    uint16_t mul;
    uint8_t add[3];
    uint8_t pad[3]; // (8 bytes: the game's code makes every structure's size a multiple of 4)
} PortPalFade;
_Static_assert(sizeof(PortPalFade) == 8, "PortPalFade: the game's size and the core's must match");
extern uint16_t gPortPalBase[512], gPortPalValue[512];
extern PortPalFade gPortPalFade[512];

#endif
