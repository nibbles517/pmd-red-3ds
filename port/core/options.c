// Port settings: those that live in the game's own options (chosen in its
// menus, kept in its save), and those kept in a file of their own. Compiled with pmd-red's include path.

#include "global.h"
#include "game_options.h"
#include "structs/str_dungeon.h"
#include "../core/port.h"
#include "port_game.h"

#define PORT_CONFIG_VERSION 3
struct PortConfig gPortConfig = { .version = PORT_CONFIG_VERSION, .frameSkip = PORT_FRAMESKIP_AUTO };

void Port_LoadConfig(void)
{
    // An older, shorter file loads too (the backend fills the rest with 0).
    struct PortConfig c;
    // Dual Core until chosen: on where it is needed (an Old 3DS)
    gPortConfig.dualCore = !Backend_IsNew3DS();
    if (!Backend_LoadConfig((uint8_t *)&c, sizeof(c)))
        return;
    if (c.version == 1) { // before Frame Skip: Auto
        c.version = 2;
        c.frameSkip = PORT_FRAMESKIP_AUTO;
    }
    if (c.version == 2) { // before Dual Core
        c.version = 3;
        c.dualCore = gPortConfig.dualCore;
    }
    if (c.version == PORT_CONFIG_VERSION)
        gPortConfig = c;
    memset(gPortConfig.unused1, 0, sizeof(gPortConfig.unused1));
    Port_UpdateScreenSize();
}

// The picture's size (port_view.h): 320x240 in Fit size, else 256x192.
int gPortScreenW = 256, gPortScreenH = 192;

void Port_UpdateScreenSize(void)
{
    bool8 fit = gPortConfig.layout[0][0] == PORT_SIZE_FIT;
    gPortScreenW = fit ? 320 : 256;
    gPortScreenH = fit ? 240 : 192;
}

int Port_DualCore(void)
{
    return gPortConfig.dualCore != 0;
}

extern bool8 gPortGroundRunning; // subscreen.c

const char *Port_SceneName(void)
{
    if (gDungeon != NULL)
        return "dungeon";
    return gPortGroundRunning ? "ground" : "other";
}

const char *Port_SettingsSummary(void)
{
    static const char *const sSizes[4] = { "GBA", "DS", "Fit", "3DS" };
    static const char *const sLayouts[4] = { "Blue", "Sky", "Log", "M+T" };
    static const char *const sSkips[3] = { "Off", "Auto", "Always" };
    static char buf[64];
    snprintf(buf, sizeof(buf), "size=%s sub=%s skip=%s dual=%s", sSizes[gPortConfig.layout[0][0] & 3],
             gPortConfig.subScreenDisabled ? "off" : sLayouts[gPortConfig.layoutSubScreen & 3],
             sSkips[gPortConfig.frameSkip % 3], gPortConfig.dualCore ? "On" : "Off");
    return buf;
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
// Main Screen Settings > Size: DS or Fit (port_view.h). GBA keeps the 240x160 picture.
bool Port_WideSize(void)
{
    return PORT_WIDE_SIZE;
}

bool Port_WantsBottomScreen(void)
{
    return gGameOptionsRef == NULL || !gGameOptionsRef->touchScreen;
}
