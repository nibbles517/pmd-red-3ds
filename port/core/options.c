// Port settings: those that live in the game's own options (chosen in its
// menus, kept in its save), and those kept in a file of their own. Compiled with pmd-red's include path.

#include "global.h"
#include "game_options.h"
#include "structs/str_dungeon.h"
#include "../core/port.h"
#include "port_game.h"

#define PORT_CONFIG_VERSION 2
struct PortConfig gPortConfig = { .version = PORT_CONFIG_VERSION, .frameSkip = PORT_FRAMESKIP_AUTO };

void Port_LoadConfig(void)
{
    // An older, shorter file loads too (the backend fills the rest with 0).
    struct PortConfig c;
    if (!Backend_LoadConfig((uint8_t *)&c, sizeof(c)))
        return;
    if (c.version == 1) { // before Frame Skip: Auto
        c.version = PORT_CONFIG_VERSION;
        c.frameSkip = PORT_FRAMESKIP_AUTO;
    }
    if (c.version == PORT_CONFIG_VERSION)
        gPortConfig = c;
    memset(gPortConfig.unused1, 0, sizeof(gPortConfig.unused1));
}

int Port_FrameSkipMode(void)
{
    return gPortConfig.frameSkip;
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
