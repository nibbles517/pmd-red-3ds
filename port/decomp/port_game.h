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
// All of them at once (dx: which graphic), up to max.
s32 Port_GetAllStatusSymbols(s32 dungeonSpriteId, struct PortStatusSymbol *out, s32 max);

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
    // Controls Options (version 4)
    u8 buttonStyle;         // PORT_BUTTONS_*
    u8 touchScreenOff;      // Touch Screen: 0 On, 1 Off
    // Game Options > Change Current Save Data (version 5)
    u8 saveSlot;            // 0-2: Save Data 1-3
    // Game Options > Music Options (version 6)
    u8 musicStyle;          // PORT_MUSIC_GBA, PORT_MUSIC_DS
    u8 musicVolume;         // the DS music's volume, in tens of percent (version 7; 19 at first)
    u8 sfxStyle;            // Sound Effects: PORT_MUSIC_GBA, PORT_MUSIC_DS (version 9)
    u8 sfxVolume;           // the DS sound effects' volume, in tens of percent (version 10; 10 at first)
    u8 gpuDrawOff;          // Graphics Options > GPU Drawing: 0 On, 1 Off (version 11)
    u8 busyWait;            // Graphics Options > Busy Screen: 0 Skip, 1 Wait (version 12; Skip at first since version 14)
    u8 mainScreenTop;       // Graphics Options > Main Screen: 0 Bottom, 1 Top (in config.ini; was in the save,
                            // in the game options' touch screen bit, which now just follows it: game_options.c)
};

extern struct PortConfig gPortConfig;
enum { PORT_FRAMESKIP_OFF, PORT_FRAMESKIP_AUTO, PORT_FRAMESKIP_ALWAYS };
enum { PORT_BUTTONS_GBA, PORT_BUTTONS_DS, PORT_BUTTONS_3DS };
enum { PORT_MUSIC_GBA, PORT_MUSIC_DS }; // Music Style: the game's own music, or pmd-blue.nds's
// Controls Options > Button Style: DS or 3DS (Rotate Mode on Y, held)
#define PORT_DS_BUTTONS (gPortConfig.buttonStyle == PORT_BUTTONS_DS || gPortConfig.buttonStyle == PORT_BUTTONS_3DS)
bool8 Port_YHeld(void); // port/core/frame.c
// Set (to 2) every frame the player walks freely, with no menu open: X is
// then B (port/core/frame.c)
extern u8 gPortFieldControl;
enum { PORT_SIZE_GBA, PORT_SIZE_DS, PORT_SIZE_FIT, PORT_SIZE_3DS }; // 3DS: main screen only
enum { PORT_SUB_LAYOUT_BLUE, PORT_SUB_LAYOUT_SKY, PORT_SUB_LAYOUT_LOG, PORT_SUB_LAYOUT_MT };
// Main Screen Settings > Size is DS, Fit or 3DS: the 256x192, 320x240 or
// 400x240 picture and the wide view (port_view.h). Everything for it is off in
// GBA size.
#define PORT_DS_SIZE (gPortConfig.layout[0][0] == PORT_SIZE_DS)
#define PORT_WIDE_SIZE (gPortConfig.layout[0][0] >= PORT_SIZE_DS && gPortConfig.layout[0][0] <= PORT_SIZE_3DS)
// The dungeon camera keeps the leader at the screen's middle line, plus 12
// lower, as on the DS (y 108 of 192, 132 of 240; the GBA's 96 of 160 in GBA
// size); the corridor light circles move with it. In GBA lines past 96:
#define PORT_LEADER_SHIFT (PORT_WIDE_SIZE ? SCREEN_H / 2 - 96 + 12 : 0)

// Text windows (BG0/BG1): 20 rows on the GBA; up to 30 for the wide view,
// which moves them down, nearer the taller screen's bottom: 16 lines in DS
// size (half the extra lines); in Fit 56 for the windows in the GBA screen's
// bottom half (those at its bottom end 25 lines above the screen's), 24 for
// the others (text_1.c WindowShiftTiles).
#define TEXT_ROWS ((SCREEN_H_MAX + 7) / 8)
#define WINDOW_SHIFT_TILES (SCREEN_H > 192 ? 7 : 2)
#define WINDOW_SHIFT_TILES_TOP 3
// Fit size: the dungeon minimap one row lower (dungeon_map.c), below the
// floor, level and HP, which are drawn 4 lines lower (gPortHudLines)
#define PORT_MINIMAP_SHIFT (PORT_WIDE_SIZE && SCREEN_H > 192 ? 1 : 0)
// Fit size: the dungeon message log (L+B) shows 11 messages instead of 8
#define PORT_MESSAGE_LOG_TALL (PORT_WIDE_SIZE && SCREEN_H > 192)
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
// Music Options (port/core/music.c): the DS version's music, from pmd-blue.nds
int Port_MusicOpen(void); // the number of tracks, or -1 (no file, or not usable)
void Port_MusicClose(void);
int Port_MusicCount(void);
const char *Port_MusicLabel(int track);
int Port_MusicIsStream(int track);
int Port_MusicIsEffect(int track); // a sound effect (listed after the music)
int Port_MusicSongCount(void); // the music's tracks (the first ones)
const char *Port_MusicRomTitle(void);
int Port_MusicIcon(u8 *tiles, u8 *colors); // the DS Menu icon, 64x64 (8x8 tiles)
const char *Port_MusicGameCode(void); // e.g. "APHE" (Blue Rescue Team, North America)
int Port_MusicRomIsBlue(void); // APHE, APHP or APHJ
// Music Options finding the ROM: a path on the SD card ("/roms/nds/x.nds");
// 1 Blue Rescue Team's (kept), 0 another game's (until closed), -1 not one
int Port_MusicTryRom(const char *path);
const char *Port_MusicRomPath(void); // the ROM picked ("": none, pmd-blue.nds)
#ifndef PORT_DIR_ENTRY_DEFINED
#define PORT_DIR_ENTRY_DEFINED
typedef struct { // (128 bytes, whatever the ABI pads a struct to)
    char name[124];
    unsigned char isDir;
    unsigned char pad[3];
} PortDirEntry;
int Backend_DirList(const char *dir, PortDirEntry *out, int max); // a folder of the SD card
#endif
// GBA song -> DS track (track list index, -1: none); Music Matching saves them
int Port_MusicMapGet(int gbaSong);
void Port_MusicMapSet(int gbaSong, int track);
int Port_MusicMapSave(const int *songs, const char *const *names, int count);
// GBA sound effect (300-999) -> DS sound effect (track list index, -1: none)
int Port_SfxMapGet(int se);
// A DS track's own volume, percent (built in; ds-volumes.txt: DS Volume
// Balance's changes, which Port_TrackGainsSave writes)
int Port_TrackGainGet(int track);
int Port_TrackGainDefault(int track);
void Port_TrackGainSet(int track, int percent);
int Port_TrackGainMax(int track);
int Port_TrackGainsSave(void);
int Port_SfxGainGet(int track); // (100 for a track that isn't a sound effect)
void Port_SfxGainSet(int track, int percent);
extern s16 gPortMatchBgm; // DS Volume Balance: the DS song playing under the sound effects (-1: none)
// Music Style: DS (music.c): slot 0 the background music, 1 a fanfare
void Port_MusicSlot(int slot, int track, int loop, int paused, int volume64);
int Port_MusicSlotDone(int slot);
#ifndef PORT_SFX_SLOTS
#define PORT_SFX_SLOTS 6 // one for each GBA sound effect player
#endif
void Port_MusicSfxSlot(int slot, int track, unsigned serial, int ending, int volume64);
bool8 Port_DsSfxOn(void); // music.c: Sound Effects DS, with Blue Rescue Team's ROM
bool8 Port_DsMusicOn(void); // music.c: Music Style DS, with Blue Rescue Team's ROM
void Port_DsMusicRecheck(void); // after Music Options: its settings, its ROM again
extern bool8 gPortDsMusicOff; // Music Matching: the game's own music only
int Port_MusicPlay(int track); // 0 if it can't be played
void Port_MusicStop(void);
int Port_MusicPlaying(void); // the track playing, or -1
void Port_MusicSetPaused(int paused);
int Port_MusicPaused(void);
void Port_MusicSetRepeat(int repeat);
int Port_MusicRepeat(void);
int Port_MusicDone(void);
unsigned Port_MusicPosMs(void);
int Port_MusicLoops(void);
unsigned Port_MusicVoices(void);
// The buttons held (port/core/port.h KEY_BIT_*, with X and Y); with
// gPortRawXY set, X is X instead of A or B (DS and 3DS buttons).
unsigned Port_KeysHeld(void);
#define PORT_KEY_Y (1 << 10)
#define PORT_KEY_X (1 << 11)
#define PORT_KEY_ZL (1 << 12)
#define PORT_KEY_ZR (1 << 13)
#define PORT_KEY_CPAD_UP (1 << 14)
#define PORT_KEY_CPAD_DOWN (1 << 15)
// A dialogue box portrait (MonPortraitMsg.unkE) of 8x8 tiles: Music Options'
// ROM icon, at twice its size (string_format.c)
#define PORT_PORTRAIT_ICON 0x80
void Port_MusicSetVolume(int percent); // 10-400; 100: as the DS plays it
void Port_MusicSetSfxVolume(int percent); // the sound effects' (the same)
extern u8 gPortRawXY;
extern u8 gPortItemSortMenu; // set each frame an item list SELECT sorts is open: Y is SELECT there (port/core/frame.c)
// Change Current Save Data: Save Data 1-3 (port/core/save.c)
void Port_ChangeSaveSlot(int slot);
void Port_RunOptionsChosen(void); // options.c (port_new_run.c, after New Game's Yes)

#endif
