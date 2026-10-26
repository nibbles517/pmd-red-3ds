// Port settings: those that live in the game's own options (chosen in its
// menus, kept in its save), and those kept in a file of their own. Compiled with pmd-red's include path.

#include "global.h"
#include "game_options.h"
#include "../core/port.h"
#include "port_game.h"

#define PORT_CONFIG_VERSION 1
struct PortConfig gPortConfig = { PORT_CONFIG_VERSION };

void Port_LoadConfig(void)
{
    struct PortConfig c;
    if (Backend_LoadConfig((uint8_t *)&c, sizeof(c)) && c.version == PORT_CONFIG_VERSION)
        gPortConfig = c;
}

void Port_SaveConfig(void)
{
    Backend_StoreConfig((const uint8_t *)&gPortConfig, sizeof(gPortConfig));
}

// Others > Game Options > Main Screen (outside dungeons). Stored in the bit
// the DS version uses for its touch screen option, which the GBA game never
// reads: clear (new games, saves from a GBA) means the bottom screen, as on
// the DS; set means the top screen.
bool Port_WantsBottomScreen(void)
{
    return gGameOptionsRef == NULL || !gGameOptionsRef->touchScreen;
}
