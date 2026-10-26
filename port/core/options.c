// Port settings: those that live in the game's own options (chosen in its
// menus, kept in its save), and those kept in a file of their own. Compiled with pmd-red's include path.

#include "global.h"
#include "game_options.h"
#include "structs/str_dungeon.h"
#include "../core/port.h"
#include "port_game.h"

#define PORT_CONFIG_VERSION 1
struct PortConfig gPortConfig = { PORT_CONFIG_VERSION };

void Port_LoadConfig(void)
{
    // An older, shorter file loads too (the backend fills the rest with 0).
    struct PortConfig c;
    if (Backend_LoadConfig((uint8_t *)&c, sizeof(c)) && c.version == PORT_CONFIG_VERSION)
        gPortConfig = c;
}

void Port_GetDungeonWindows(u8 *bgColor, u8 *shading, u8 *border)
{
    if (gPortConfig.dungeonWindowsSet) {
        *bgColor = gPortConfig.dungeonBgColor;
        *shading = gPortConfig.dungeonShading & 1;
        *border = gPortConfig.dungeonBorder & 1;
    }
    else {
        *bgColor = gGameOptionsRef->windowColor;
        *shading = 1;
        *border = gGameOptionsRef->playerGender != MALE;
    }
}

u8 Port_WindowColor(void)
{
    u8 c, s, b;
    if (gDungeon == NULL)
        return gGameOptionsRef->windowColor;
    Port_GetDungeonWindows(&c, &s, &b);
    return c;
}

bool8 Port_PinkBorders(void)
{
    u8 c, s, b;
    if (gDungeon == NULL)
        return gGameOptionsRef->playerGender != MALE;
    Port_GetDungeonWindows(&c, &s, &b);
    return b;
}

bool8 Port_SolidDungeonWindows(void)
{
    u8 c, s, b;
    Port_GetDungeonWindows(&c, &s, &b);
    return !s;
}

void Port_SaveConfig(void)
{
    Backend_StoreConfig((const uint8_t *)&gPortConfig, sizeof(gPortConfig));
}

// Others > Game Options > Main Screen (outside dungeons). Stored in the bit
// the DS version uses for its touch screen option, which the GBA game never
// reads: clear (new games, saves from a GBA) means the bottom screen, as on
// the DS; set means the top screen.
// Main Screen Settings > Size: DS (port_view.h). GBA keeps the 240x160 picture.
bool Port_DsSize(void)
{
    return PORT_DS_SIZE;
}

bool Port_WantsBottomScreen(void)
{
    return gGameOptionsRef == NULL || !gGameOptionsRef->touchScreen;
}
