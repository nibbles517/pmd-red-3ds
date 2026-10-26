// Interfaces between pmd-red's code and the port's own C files that read
// game state (port/core/subscreen.c). Included by both sides.
#ifndef GUARD_PORT_GAME_H
#define GUARD_PORT_GAME_H

#include "gba/types.h"
#include "../core/port_view.h"

// A status symbol over a Pokémon (dungeon_pokemon_sprites.c), as the game
// draws it this frame. Positions are relative to the point the Pokémon's
// sprite is placed by; `anchor` selects the pose's position set to add (0 or 3).
struct PortStatusSymbol
{
    const u8 *gfx;   // 4bpp tiles, width x height, 1D order
    u8 width, height; // in tiles
    u8 palNum;       // OBJ palette (the monster palettes from "palet")
    u8 anchor;
    s16 dx, dy;
};

// Fills up to 2 symbols; returns how many.
s32 Port_GetStatusSymbols(s32 dungeonSpriteId, struct PortStatusSymbol *out);

// Port settings that have no room in the game's save (Others > Screen/Layout
// Options). Kept by the backend in a file of their own.
struct PortConfig
{
    u8 version;
    u8 subScreenDisabled;   // Sub-Screen Settings > Enabled? (0 = Yes)
    u8 layout[2][2];        // [main, sub][size, scale]
    u8 windowShading;       // Windows Options > BG Shading: 0 Solid, 1 Translucent
    u8 unused;              // (was Border Color, now the game's own player gender option)
    // In dungeons, Others > Game Options > Windows Options: the same three
    // settings, for dungeons only. Until first changed (dungeonWindowsSet),
    // dungeons look as in the game: the BG Color and Border Color chosen
    // outside, translucent.
    u8 dungeonWindowsSet;
    u8 dungeonBgColor;      // WINDOW_COLOR_*
    u8 dungeonShading;      // 0 Solid, 1 Translucent
    u8 dungeonBorder;       // 0 Blue, 1 Pink
    // ... > Layout Options (placeholders for now)
    u8 layoutSubScreen;     // 0 Blue, 1 Sky, 2 Log, 3 M+T
    u8 layoutInfo;          // Additional Info: 0 Off, 1 On
};

// The window settings in effect: in dungeons, the dungeon's (options.c)
u8 Port_WindowColor(void);
bool8 Port_PinkBorders(void);       // the border colors of a girl player
bool8 Port_SolidDungeonWindows(void);
// The dungeon's settings as they are now (whether set or not)
void Port_GetDungeonWindows(u8 *bgColor, u8 *shading, u8 *border);
extern struct PortConfig gPortConfig;
enum { PORT_SIZE_GBA, PORT_SIZE_DS, PORT_SIZE_3DS };
// Main Screen Settings > Size is DS: the 256x192 picture and the wide view
// (port_view.h). Everything for it is off in the other sizes.
#define PORT_DS_SIZE (gPortConfig.layout[0][0] == PORT_SIZE_DS)

// Text windows (BG0/BG1): 20 rows on the GBA; 24 for the wide view, which
// moves them down 2 rows (16 lines), nearer the taller screen's bottom.
#define TEXT_ROWS ((SCREEN_H + 7) / 8)
#define WINDOW_SHIFT_TILES 2
// Lines of window backgrounds (text_2.c DrawWindowBg, per-line WIN0H)
#define WINDOW_BG_LINES SCREEN_H

extern bool8 gPortGroundNarrow; // ground_bg.c
bool8 Port_GroundViewIsNarrow(void); // ground_map.c
// ground_bg.c: the friend areas map and the world map in the wide view
struct DungeonPos;
void Port_DrawWideMapArrays(const u16 (*bg2)[64], const u16 (*bg3)[64], s32 bgX, s32 bgY);
void Port_ShowWideMapArrays(s32 bgX, s32 bgY);
void Port_WideMapFollow(struct DungeonPos *bgPos, const struct DungeonPos *mon, s32 maxX, s32 maxY);
void Port_SaveConfig(void);

#endif
