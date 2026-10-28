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
    u8 unused1[6];          // (window settings of earlier versions)
    // In dungeons, Others > Game Options > Layout Options
    u8 layoutSubScreen;     // 0 Blue, 1 Sky, 2 Log, 3 M+T
    u8 layoutInfo;          // Additional Info: 0 Off, 1 On
    // Graphics Options
    u8 frameSkip;           // PORT_FRAMESKIP_* (version 2)
    u8 dualCore;            // 0 Off, 1 On (version 3; until then: On on an Old 3DS, Off on a New 3DS)
};

extern struct PortConfig gPortConfig;
enum { PORT_FRAMESKIP_OFF, PORT_FRAMESKIP_AUTO, PORT_FRAMESKIP_ALWAYS };
enum { PORT_SIZE_GBA, PORT_SIZE_DS, PORT_SIZE_FIT, PORT_SIZE_3DS }; // 3DS: not yet (as GBA)
enum { PORT_SUB_LAYOUT_BLUE, PORT_SUB_LAYOUT_SKY, PORT_SUB_LAYOUT_LOG, PORT_SUB_LAYOUT_MT };
// Main Screen Settings > Size is DS or Fit: the 256x192 or 320x240 picture
// and the wide view (port_view.h). Everything for it is off in the other sizes.
#define PORT_DS_SIZE (gPortConfig.layout[0][0] == PORT_SIZE_DS)
#define PORT_WIDE_SIZE (gPortConfig.layout[0][0] == PORT_SIZE_DS || gPortConfig.layout[0][0] == PORT_SIZE_FIT)
// The dungeon camera keeps the leader at the screen's middle line, plus 12
// lower, as on the DS (y 108 of 192, 132 of 240; the GBA's 96 of 160 in GBA
// size); the corridor light circles move with it. In GBA lines past 96:
#define PORT_LEADER_SHIFT (PORT_WIDE_SIZE ? SCREEN_H / 2 - 96 + 12 : 0)

// Text windows (BG0/BG1): 20 rows on the GBA; up to 30 for the wide view,
// which moves them down, nearer the taller screen's bottom: 16 lines in DS
// size (half the extra lines), 56 in Fit (the windows at the GBA screen's
// bottom end 25 lines above the screen's).
#define TEXT_ROWS ((SCREEN_H_MAX + 7) / 8)
#define WINDOW_SHIFT_TILES (SCREEN_H > 192 ? 7 : 2)
// Lines of window backgrounds (text_2.c DrawWindowBg, per-line WIN0H)
#define WINDOW_BG_LINES SCREEN_H_MAX

extern bool8 gPortGroundNarrow; // ground_bg.c
bool8 Port_GroundViewIsNarrow(void); // ground_map.c
// ground_bg.c: the friend areas map and the world map in the wide view
struct DungeonPos;
void Port_DrawWideMapArrays(const u16 (*bg2)[64], const u16 (*bg3)[64], s32 bgX, s32 bgY);
void Port_ShowWideMapArrays(s32 bgX, s32 bgY);
void Port_WideMapFollow(struct DungeonPos *bgPos, const struct DungeonPos *mon, s32 maxX, s32 maxY);
void Port_SaveConfig(void);

#endif
