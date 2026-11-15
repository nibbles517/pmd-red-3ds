// Port settings: those that live in the game's own options (chosen in its
// menus, kept in its save), and those kept in a file of their own. Compiled with pmd-red's include path.

#include <stdio.h>
#include <string.h>
#include "global.h"
#include "game_options.h"
#include "structs/str_dungeon.h"
#include "../core/port.h"
#include "port_game.h"
#include "port_new_run.h"

#define PORT_CONFIG_INI "config.ini"

#define PORT_CONFIG_VERSION 14
struct PortConfig gPortConfig = { .version = PORT_CONFIG_VERSION, .frameSkip = PORT_FRAMESKIP_AUTO, .musicVolume = 19, .sfxVolume = 10 };

// The run options (port_new_run.c) of each Save Data, chosen at New Game; and
// those of the adventure being played. Chosen ones replace the slot's when the
// game next writes its save (Port_RunOptionsSaved; it does soon after New
// Game), so they change together with the Save Data.
u8 gPortRunOptions[PORT_RUN_OPTIONS];
static u8 sRunOptions[3][PORT_RUN_OPTIONS];
static bool8 sRunOptionsPending;

static void IniLoad(const char *text, uint32_t len);
static bool8 IniSave(void);

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
    gPortConfig.busyWait = 0; // Busy Screen: Skip
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

static void CheckConfig(void);

void Port_LoadConfig(void)
{
    // An older, shorter file loads too (the backend fills the rest with 0).
    struct PortConfig c;
    void *f;
    SetDefaultConfig();
    memset(sRunOptions, 0, sizeof(sRunOptions));
    // config.ini, the settings file since version 14 (CheckConfig the values)
    if ((f = Backend_FileOpen(PORT_CONFIG_INI)) != NULL) {
        uint32_t size = Backend_FileSize(f);
        static char text[8192];
        if (size >= sizeof(text))
            size = sizeof(text) - 1;
        if (Backend_FileRead(f, 0, text, size)) {
            text[size] = 0;
            IniLoad(text, size);
        }
        Backend_FileClose(f);
        CheckConfig();
        IniSave(); // (written again in full: settings left out, or not valid, as now)
        return;
    }
    // Else the older config.bin, if any; config.ini from then on
    if (!Backend_LoadConfig((uint8_t *)&c, sizeof(c))) {
        CheckConfig();
        IniSave();
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
    if (c.version == 11) { // before Busy Screen (then Triple Buffer)
        c.version = 12;
        c.busyWait = 1;
    }
    if (c.version == 12) { // Busy Screen: Wait at first (Triple Buffer, its place's setting before, froze a 3DS)
        c.version = 13;
        c.busyWait = 1;
    }
    if (c.version == 13) { // Busy Screen: Skip, the default now (tried on an Old 3DS)
        c.version = 14;
        c.busyWait = 0;
    }
    if (c.version == PORT_CONFIG_VERSION)
        gPortConfig = c;
    CheckConfig();
    IniSave();
}

// Settings out of their range (an edited config.ini, an older file) go back to
// their defaults; then the screen's size, and the current Save Data's run
// options.
static void CheckConfig(void)
{
    if (gPortConfig.frameSkip > PORT_FRAMESKIP_ALWAYS)
        gPortConfig.frameSkip = PORT_FRAMESKIP_AUTO;
    if (gPortConfig.layout[0][0] > PORT_SIZE_3DS)
        gPortConfig.layout[0][0] = PORT_SIZE_FIT;
    if (gPortConfig.layout[1][0] > PORT_SIZE_FIT) // (3DS: the main screen only)
        gPortConfig.layout[1][0] = PORT_SIZE_FIT;
    for (int i = 0; i < 2; i++)
        if (gPortConfig.layout[i][1] > PORT_SCALE_STRETCH)
            gPortConfig.layout[i][1] = PORT_SCALE_FULL;
    // (Sky and the 3DS button style were taken out: an old config's become
    // Blue and DS)
    if (gPortConfig.layoutSubScreen > PORT_SUB_LAYOUT_MT || gPortConfig.layoutSubScreen == PORT_SUB_LAYOUT_SKY)
        gPortConfig.layoutSubScreen = PORT_SUB_LAYOUT_BLUE;
    if (gPortConfig.buttonStyle >= PORT_BUTTONS_3DS)
        gPortConfig.buttonStyle = PORT_BUTTONS_DS;
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
    if (gPortConfig.mainScreenTop > 1)
        gPortConfig.mainScreenTop = 0;
    memset(gPortConfig.unused1, 0, sizeof(gPortConfig.unused1));
    gPortConfig.version = PORT_CONFIG_VERSION;
    Port_UpdateScreenSize();
    Port_RunOptionsUseSlot(gPortConfig.saveSlot);
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

// The picture's size (port_view.h): 320x240 in Fit size, 400x240 in 3DS size
// (the top screen's; Fit while the game is on the bottom screen), else 256x192.
int gPortScreenW = 256, gPortScreenH = 192;

void Port_UpdateScreenSize(void)
{
    int size = gPortConfig.layout[0][0];
    bool8 tall = size == PORT_SIZE_FIT || size == PORT_SIZE_3DS;
    gPortScreenW = size == PORT_SIZE_3DS && !Backend_GameOnBottom() ? 400 : tall ? 320 : 256;
    gPortScreenH = tall ? 240 : 192;
}

int Port_DualCore(void)
{
    return gPortConfig.dualCore != 0;
}

int Port_GpuDraw(void)
{
    return gPortConfig.gpuDrawOff == 0;
}

int Port_BusySkip(void)
{
    return gPortConfig.busyWait == 0;
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
    snprintf(buf, sizeof(buf), "size=%s sub=%s skip=%s dual=%s busy=%s", sSizes[gPortConfig.layout[0][0] & 3],
             gPortConfig.subScreenDisabled ? "off" : sLayouts[gPortConfig.layoutSubScreen & 3],
             sSkips[gPortConfig.frameSkip % 3], gPortConfig.dualCore ? "On" : "Off",
             gPortConfig.busyWait ? "Wait" : "Skip");
    return buf;
}

int Port_FrameSkipMode(void)
{
    return gPortConfig.frameSkip;
}

void Port_SaveConfig(void)
{
    IniSave();
    // (config.bin: only the test harness's backend still keeps it)
    Backend_StoreConfig((const uint8_t *)&gPortConfig, sizeof(gPortConfig));
}

// ---- Run options (per Save Data) ----

void Port_RunOptionsUseSlot(int slot)
{
    if (slot < 0 || slot > 2)
        slot = 0;
    memcpy(gPortRunOptions, sRunOptions[slot], sizeof(gPortRunOptions));
    sRunOptionsPending = FALSE;
}

// New Game (port_new_run.c): gPortRunOptions are this adventure's from now on
void Port_RunOptionsChosen(void)
{
    sRunOptionsPending = TRUE;
}

// The game wrote its save (save.c): a new adventure's run options are its
// Save Data's now
void Port_RunOptionsSaved(void)
{
    if (!sRunOptionsPending)
        return;
    sRunOptionsPending = FALSE;
    memcpy(sRunOptions[gPortConfig.saveSlot % 3], gPortRunOptions, sizeof(gPortRunOptions));
    IniSave();
}

// ---- config.ini ----
//
// Every setting of the port, by the names of the menus it is chosen in, and
// each Save Data's run options (not changed in the game: only at New Game).
// The game writes it again at start and whenever a setting changes, in its
// own words; written by hand, unknown lines are skipped, names and values are
// read in any case, and a value out of range goes back to its default.

#define INI_VOLUME 0xFF // (a value list: a volume, 10%-400%, in tens of percent)

static const char *const sIniOffOn[] = { "Off", "On", NULL };
static const char *const sIniOnOff[] = { "On", "Off", NULL };
static const char *const sIniYesNo[] = { "Yes", "No", NULL };
static const char *const sIniSkip[] = { "Off", "Auto", "Always", NULL };
static const char *const sIniBusy[] = { "Skip", "Wait", NULL };
static const char *const sIniMainScreen[] = { "Bottom", "Top", NULL };
static const char *const sIniSizes[] = { "GBA", "DS", "Fit", "3DS", NULL };
static const char *const sIniSubSizes[] = { "GBA", "DS", "Fit", NULL };
static const char *const sIniScales[] = { "None", "Full", "Stretch", NULL };
static const char *const sIniLayouts[] = { "Blue", "Sky", "Log", "M+T", NULL };
static const char *const sIniButtons[] = { "GBA", "DS", "3DS", NULL };
static const char *const sIniStyles[] = { "GBA", "DS", NULL };
static const char *const sIniSlots[] = { "1", "2", "3", NULL };
// The run options, by their names in the menu (port_new_run.c sNames)
static const char *const sRunOptionNames[PORT_RUN_OPTIONS] = {
    "Increased Sitrus Berries", "Increased Life Seeds", "Increased Vitamins", "Blue Exclusive Pok\xC3\xA9mon",
    "Shiny Pok\xC3\xA9mon", "Option 6", "Option 7", "Normalized Shop Prices",
};

typedef struct {
    const char *section, *key;
    u8 *value;
    const char *const *names; // value i is names[i]; NULL: INI_VOLUME
} IniSetting;

static const IniSetting sIni[] = {
    { "Graphics Options", "Frame Skip", &gPortConfig.frameSkip, sIniSkip },
    { "Graphics Options", "Dual Core", &gPortConfig.dualCore, sIniOffOn },
    { "Graphics Options", "GPU Drawing", &gPortConfig.gpuDrawOff, sIniOnOff },
    { "Graphics Options", "Busy Screen", &gPortConfig.busyWait, sIniBusy },
    { "Graphics Options", "Main Screen", &gPortConfig.mainScreenTop, sIniMainScreen },
    { "Main Screen Settings", "Size", &gPortConfig.layout[0][0], sIniSizes },
    { "Main Screen Settings", "Scale", &gPortConfig.layout[0][1], sIniScales },
    { "Sub-Screen Settings", "Enabled", &gPortConfig.subScreenDisabled, sIniYesNo },
    { "Sub-Screen Settings", "Size", &gPortConfig.layout[1][0], sIniSubSizes },
    { "Sub-Screen Settings", "Scale", &gPortConfig.layout[1][1], sIniScales },
    { "Layout Options", "Sub-Screen Layout", &gPortConfig.layoutSubScreen, sIniLayouts },
    { "Controls Options", "Button Style", &gPortConfig.buttonStyle, sIniButtons },
    { "Music Options", "Music Style", &gPortConfig.musicStyle, sIniStyles },
    { "Music Options", "Sound Effects", &gPortConfig.sfxStyle, sIniStyles },
    { "Music Options", "Music Volume", &gPortConfig.musicVolume, NULL },
    { "Music Options", "SFX Volume", &gPortConfig.sfxVolume, NULL },
    { "Save Data", "Current Save Data", &gPortConfig.saveSlot, sIniSlots },
};

static int IniLower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

// Same text, any case, spaces at either end of `a` (len bytes) skipped
static bool8 IniSame(const char *a, int len, const char *b)
{
    while (len > 0 && (*a == ' ' || *a == '\t')) { a++; len--; }
    while (len > 0 && (a[len - 1] == ' ' || a[len - 1] == '\t')) len--;
    for (; len > 0 && *b; a++, b++, len--)
        if (IniLower(*a) != IniLower(*b))
            return FALSE;
    return len == 0 && *b == 0;
}

static void IniValue(const IniSetting *s, const char *v, int len)
{
    if (s->names == NULL) { // "190%" (the % may be left out)
        int n = 0, digits = 0;
        for (int i = 0; i < len; i++) {
            if (v[i] >= '0' && v[i] <= '9') { n = n * 10 + v[i] - '0'; digits++; if (n > 1000) n = 1000; }
            else if (v[i] != ' ' && v[i] != '\t' && v[i] != '%') return;
        }
        if (digits > 0)
            *s->value = (u8)((n + 5) / 10); // CheckConfig: 1-40
        return;
    }
    for (int i = 0; s->names[i] != NULL; i++) {
        if (IniSame(v, len, s->names[i])) {
            *s->value = (u8)i;
            return;
        }
    }
}

static void IniLoad(const char *text, uint32_t len)
{
    const char *end = text + len, *section = "";
    int sectionLen = 0;

    while (text < end) {
        const char *line = text, *eol = text, *eq = NULL;
        while (eol < end && *eol != '\n' && *eol != '\r')
            eol++;
        text = eol + 1;
        while (line < eol && (*line == ' ' || *line == '\t'))
            line++;
        // a comment after the value: ; (at the line's start too)
        const char *stop = line;
        while (stop < eol && *stop != ';' && *stop != '#')
            stop++;
        if (stop == line)
            continue;
        if (*line == '[') {
            const char *close = line;
            while (close < stop && *close != ']')
                close++;
            section = line + 1;
            sectionLen = (int)(close - section);
            continue;
        }
        for (const char *p = line; p < stop; p++)
            if (*p == '=') { eq = p; break; }
        if (eq == NULL)
            continue;
        int keyLen = (int)(eq - line), valueLen = (int)(stop - eq - 1);
        // Save Data 1-3: Option 1-8
        for (int slot = 0; slot < 3; slot++) {
            static const char *const sSlotNames[3] = { "Save Data 1", "Save Data 2", "Save Data 3" };
            if (!IniSame(section, sectionLen, sSlotNames[slot]))
                continue;
            for (int i = 0; i < PORT_RUN_OPTIONS; i++) {
                char key[16];
                snprintf(key, sizeof(key), "Option %d", i + 1); // (as written before they had names)
                if (IniSame(line, keyLen, sRunOptionNames[i]) || IniSame(line, keyLen, key)
                    || (i == 3 && IniSame(line, keyLen, "Blue Exclusive Pokemon"))
                    || (i == 4 && IniSame(line, keyLen, "Shiny Pokemon"))) { // (é typed as e)
                    if (IniSame(eq + 1, valueLen, "On")) sRunOptions[slot][i] = 1;
                    else if (IniSame(eq + 1, valueLen, "Off")) sRunOptions[slot][i] = 0;
                }
            }
        }
        for (unsigned i = 0; i < sizeof(sIni) / sizeof(sIni[0]); i++)
            if (IniSame(section, sectionLen, sIni[i].section) && IniSame(line, keyLen, sIni[i].key))
                IniValue(&sIni[i], eq + 1, valueLen);
    }
}

static bool8 IniSave(void)
{
    static char text[8192];
    int n = 0;
    const char *section = NULL;

#define INI_PRINT(...) do { if (n < (int)sizeof(text)) n += snprintf(text + n, sizeof(text) - n, __VA_ARGS__); } while (0)
    INI_PRINT("; Pokemon Mystery Dungeon: Red Rescue Team, 3DS port: settings\n"
              ";\n"
              "; The game writes this file again when it starts and whenever its\n"
              "; settings change (your own comments are not kept).\n"
              "; After each setting, its choices. A setting left out, or not one\n"
              "; of its choices, is its default. The run options are chosen once,\n"
              "; at New Game, for that Save Data's adventure.\n");
    for (unsigned i = 0; i < sizeof(sIni) / sizeof(sIni[0]); i++) {
        const IniSetting *s = &sIni[i];
        char value[16], line[64];
        if (section != s->section) {
            section = s->section;
            INI_PRINT("\n[%s]\n", section);
        }
        if (s->names == NULL)
            snprintf(value, sizeof(value), "%d%%", *s->value * 10);
        else
            snprintf(value, sizeof(value), "%s", s->names[*s->value]); // (CheckConfig: in range)
        snprintf(line, sizeof(line), "%s = %s", s->key, value);
        INI_PRINT("%-32s; ", line);
        if (s->names == NULL)
            INI_PRINT("10%% to 400%%, in steps of 10%%\n");
        else
            for (int k = 0; s->names[k] != NULL; k++)
                INI_PRINT("%s%s", s->names[k], s->names[k + 1] ? ", " : "\n");
    }
    for (int slot = 0; slot < 3; slot++) {
        INI_PRINT("\n[Save Data %d]\n", slot + 1);
        for (int i = 0; i < PORT_RUN_OPTIONS; i++)
            INI_PRINT("%s = %s\n", sRunOptionNames[i], sRunOptions[slot][i] ? "On" : "Off");
    }
#undef INI_PRINT
    if (n >= (int)sizeof(text))
        return FALSE;
    return Backend_FileWrite(PORT_CONFIG_INI, text, (uint32_t)n);
}

// Others > Game Options > Main Screen (outside dungeons). Stored in the bit
// the DS version uses for its touch screen option, which the GBA game never
// reads: clear (new games, saves from a GBA) means the bottom screen, as on
// the DS; set means the top screen.
// Main Screen Settings > Size: DS, Fit or 3DS (port_view.h). GBA keeps the 240x160 picture.
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
    // Main Screen is kept in config.ini (gPortConfig.mainScreenTop): the game
    // options' bit is set from it whenever they are loaded or reset
    // (game_options.c); the menus change the bit, which is written there.
    if (gGameOptionsRef != NULL && (gGameOptionsRef->touchScreen != 0) != gPortConfig.mainScreenTop) {
        gPortConfig.mainScreenTop = gGameOptionsRef->touchScreen != 0;
        Port_SaveConfig();
    }
    return !gPortConfig.mainScreenTop;
}
