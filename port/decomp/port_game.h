// Interfaces between pmd-red's code and the port's own C files that read
// game state (port/core/subscreen.c). Included by both sides.
#ifndef GUARD_PORT_GAME_H
#define GUARD_PORT_GAME_H

#include "gba/types.h"

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
    u8 layout[2][2];        // [main, sub][size, scale]: placeholders for now
};
extern struct PortConfig gPortConfig;
void Port_SaveConfig(void);

#endif
