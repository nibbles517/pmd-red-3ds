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

#endif
