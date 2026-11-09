// Port settings: those that live in the game's own options (chosen in its
// menus, kept in its save), and those kept in a file of their own. Compiled with pmd-red's include path.

#include <string.h>
#include "global.h"
#include "game_options.h"
#include "structs/str_dungeon.h"
#include "../core/port.h"
#include "port_game.h"

#define PORT_CONFIG_VERSION 13
struct PortConfig gPortConfig = { .version = PORT_CONFIG_VERSION, .frameSkip = PORT_FRAMESKIP_AUTO, .musicVolume = 19, .sfxVolume = 10 };

// The settings before any are chosen (no config file yet). Those kept in the
// game's own options have theirs in InitializeGameOptions (game_options.c):
// Main Screen Bottom, BG and Border Color Blue (the border follows the
// player's gender), the dungeon map Clear.
static void SetDefaultConfig(void)
{
    memset(&gPortConfig, 0, sizeof(gPortConfig));
    gPortConfig.version = PORT_CONFIG_VERSION;
    // Graphics Options
    gPortConfig.frameSkip = Backend_IsNew3DS() ? PORT_FRAMESKIP_OFF : PORT_FRAMESKIP_AUTO;
    gPortConfig.dualCore = 1;
    gPortConfig.tripleBufferOff = 1; // Triple Buffer: Off (for now)
    gPortConfig.layout[0][0] = PORT_SIZE_FIT; // Main Screen Settings: Size Fit,
    gPortConfig.layout[0][1] = 1;             // Scale Full
    gPortConfig.subScreenDisabled = 0;        // Sub-Screen Settings: Enabled? Yes,
    gPortConfig.layout[1][0] = PORT_SIZE_FIT; // Size Fit,
    gPortConfig.layout[1][1] = 1;             // Scale Full
    // Controls Options
    gPortConfig.buttonStyle = PORT_BUTTONS_DS;
    gPortConfig.touchScreenOff = 0; // On
    // Music Options (DS music only with Blue Rescue Team's ROM, though)
    gPortConfig.musicStyle = PORT_MUSIC_DS;
    gPortConfig.musicVolume = 19;
    gPortConfig.sfxStyle = PORT_MUSIC_DS; // (the DS's sound effects)
    gPortConfig.sfxVolume = 10;
    // Layout Options (in dungeons)
    gPortConfig.layoutSubScreen = PORT_SUB_LAYOUT_BLUE;
    gPortConfig.layoutInfo = 1; // Additional Info: On
}

void Port_LoadConfig(void)
{
    // An older, shorter file loads too (the backend fills the rest with 0).
    struct PortConfig c;
    SetDefaultConfig();
    if (!Backend_LoadConfig((uint8_t *)&c, sizeof(c))) {
        Port_UpdateScreenSize();
        return;
    }
    if (c.version == 1) { // before Frame Skip: Auto
        c.version = 2;
        c.frameSkip = PORT_FRAMESKIP_AUTO;
    }
    if (c.version == 2) { // before Dual Core
        c.version = 3;
        c.dualCore = !Backend_IsNew3DS(); // (as it was then: on an Old 3DS)
    }
    if (c.version == 3) { // before Controls Options (GBA buttons, touch screen on)
        c.version = 4;
        c.buttonStyle = PORT_BUTTONS_GBA;
        c.touchScreenOff = 0;
    }
    if (c.version == 4) { // before Change Current Save Data (Save Data 1)
        c.version = 5;
        c.saveSlot = 0;
    }
    if (c.version == 5) { // before Music Options (GBA music)
        c.version = 6;
        c.musicStyle = PORT_MUSIC_GBA;
    }
    if (c.version == 6) { // before the DS music's volume (150%, the player's)
        c.version = 7;
        c.musicVolume = 15;
    }
    if (c.version == 7) { // the DS music's volume: 190% at first (was 150%)
        c.version = 8;
        if (c.musicVolume == 15)
            c.musicVolume = 19;
    }
    if (c.version == 8) { // before Sound Effects (DS, as the music's default)
        c.version = 9;
        c.sfxStyle = PORT_MUSIC_DS;
    }
    if (c.version == 9) { // before their own volume (they had the music's)
        c.version = 10;
        c.sfxVolume = 10;
    }
    if (c.version == 10) { // before GPU Drawing (On)
        c.version = 11;
        c.gpuDrawOff = 0;
    }
    if (c.version == 11) { // before Triple Buffer
        c.version = 12;
        c.tripleBufferOff = 1;
    }
    if (c.version == 12) { // Triple Buffer: Off at first (On froze a 3DS at first)
        c.version = 13;
        c.tripleBufferOff = 1;
    }
    if (c.version == PORT_CONFIG_VERSION)
        gPortConfig = c;
    if (gPortConfig.musicStyle > PORT_MUSIC_DS)
        gPortConfig.musicStyle = PORT_MUSIC_GBA;
    if (gPortConfig.sfxStyle > PORT_MUSIC_DS)
        gPortConfig.sfxStyle = PORT_MUSIC_GBA;
    if (gPortConfig.musicVolume < 1 || gPortConfig.musicVolume > 40)
        gPortConfig.musicVolume = 19;
    if (gPortConfig.sfxVolume < 1 || gPortConfig.sfxVolume > 40)
        gPortConfig.sfxVolume = 10;
    if (gPortConfig.saveSlot > 2)
        gPortConfig.saveSlot = 0;
    memset(gPortConfig.unused1, 0, sizeof(gPortConfig.unused1));
    Port_UpdateScreenSize();
}

int Port_ScreenScale(int screen)
{
    int s = gPortConfig.layout[screen ? 1 : 0][1];
    return s <= PORT_SCALE_STRETCH ? s : PORT_SCALE_NONE;
}

int Port_ConfigSaveSlot(void)
{
    return gPortConfig.saveSlot;
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

int Port_GpuDraw(void)
{
    return gPortConfig.gpuDrawOff == 0;
}

int Port_TripleBuffer(void)
{
    return gPortConfig.tripleBufferOff == 0;
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
    snprintf(buf, sizeof(buf), "size=%s sub=%s skip=%s dual=%s triple=%s", sSizes[gPortConfig.layout[0][0] & 3],
             gPortConfig.subScreenDisabled ? "off" : sLayouts[gPortConfig.layoutSubScreen & 3],
             sSkips[gPortConfig.frameSkip % 3], gPortConfig.dualCore ? "On" : "Off",
             gPortConfig.tripleBufferOff ? "Off" : "On");
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

bool Port_DsButtons(void)
{
    return PORT_DS_BUTTONS;
}

bool Port_WantsBottomScreen(void)
{
    return gGameOptionsRef == NULL || !gGameOptionsRef->touchScreen;
}
