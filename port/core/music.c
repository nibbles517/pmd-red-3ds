// Music Options: the DS version's music (Pokémon Mystery Dungeon: Blue Rescue
// Team), played from pmd-blue.nds in the port's data folder by the NDS Music
// Player's core (port/ndsmusic).
//
// The game's menu (options_menu1.c) lists the ROM's tracks and drives
// playback through the calls below. The track plays together with the game's
// own sound: Sound_EndFrame (sound.c) hands each frame's samples to
// Port_MusicMix, which adds the track's.
//
// Music Style: DS (the game's music.c) plays, instead of the game's own music,
// the DS track each GBA song is matched with: two "slots", the background
// music and a fanfare, which follow the game's music players. The matches
// (GBA song -> DS track) are built in (sDefaultMap), unless the data folder
// has a music-map.txt, as Music Matching (a development aid) writes it.

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "port.h"
#include "../ndsmusic/nds_music.h"
#include "../ndsmusic/song.h"

#define MUSIC_FILE "pmd-blue.nds"
#define LABEL_LEN 40

typedef struct {
    uint8_t sdat;
    uint8_t isStream;
    uint16_t index;
    char label[LABEL_LEN];
} Track;

static NdsMusic sRom;
static int sOpen;
static Track *sTracks;
static int sCount;

static Song *sSong;
static int sPlaying = -1; // the track playing (or stopped mid-way: -1 once stopped)
static int sPaused, sRepeat;
static int sVolume = 190; // percent (Music Options sets it)
static uint32_t sMixRate = 2 * 18157; // twice the game's mixing rate (see Port_MusicMix)

// Music Style: DS: the game's background music and fanfare, as DS tracks
typedef struct {
    Song *song;
    int track; // -1: none
    int paused;
} Slot;
static Slot sSlots[2] = { { NULL, -1, 0 }, { NULL, -1, 0 } };

// GBA song (1-127 background music, 200-218 fanfares) -> DS track
#define MAP_SONGS 300
#define MAP_FILE "music-map.txt"
static int16_t sMap[MAP_SONGS]; // track list index, or -1
static const struct {
    uint16_t gba;
    uint8_t stream; // a stream (STRM), else a sequence (SSEQ)
    uint16_t index; // in the ROM's (first) sound archive
} sDefaultMap[] = {
    // From Music Matching's music-map.txt, made with Blue Rescue Team's
    // ROM (APHE): every GBA song has the DS sequence of its own number,
    // except fanfare 209, silent and missing on the DS: 210 instead.
    {   1, 0,   1 }, // RESCUE_TEAM_BASE = 001 SND_BGM_M_HABITAT_HOME01
    {   2, 0,   2 }, // FRIEND_AREA_SWAMP = 002 SND_BGM_M_HABITAT_DARK_01
    {   3, 0,   3 }, // FRIEND_AREA_CAVES = 003 SND_BGM_M_HABITAT_MYSTIC_01
    {   4, 0,   4 }, // DREAM = 004 SND_BGM_M_EVENT_DREAM_01
    {   5, 0,   5 }, // BENEVOLENT_SPIRIT = 005 SND_BGM_M_EVENT_DREAM_02
    {   6, 0,   6 }, // LEGEND_OF_NINETALES = 006 SND_BGM_M_EVENT_LEGEND_01
    {   7, 0,   7 }, // POKEMON_SQUARE = 007 SND_BGM_M_HABITAT_HIROBA
    {   8, 0,   8 }, // FILE_SELECT = 008 SND_BGM_M_HABITAT_HOME_SHINKA
    {   9, 0,   9 }, // RISING_FEAR = 009 SND_BGM_M_EVENT_FEAR
    {  10, 0,  10 }, // THERES_TROUBLE = 010 SND_BGM_M_EVENT_KINPAKU
    {  11, 0,  11 }, // BOSS_BATTLE = 011 SND_BGM_M_BOSS01
    {  12, 0,  12 }, // WELCOME_TO_THE_WORLD_OF_POKEMON = 012 SND_BGM_M_EVENT_YUME
    {  13, 0,  13 }, // A_NEW_ADVENTURE = 013 SND_BGM_M_DUNGEON_GENERAL_01
    {  14, 0,  14 }, // THUNDERWAVE_CAVE = 014 SND_BGM_M_DUNGEON_DENJIHA_01
    {  15, 0,  15 }, // SINISTER_WOODS = 015 SND_BGM_M_DUNGEON_AYASHIIMORI_01
    {  16, 0,  16 }, // FRIEND_AREA_POND = 016 SND_BGM_M_HABITAT_CALMLY_02
    {  17, 0,  17 }, // KECLEON_SHOP = 017 SND_BGM_M_SYS_SHOP
    {  18, 0,  18 }, // STOP_THIEF = 018 SND_BGM_M_SYS_STEAL
    {  19, 0,  19 }, // WORLD_CALAMITY = 019 SND_BGM_M_EVENT_DISASTER
    {  20, 0,  20 }, // GREAT_CANYON = 020 SND_BGM_M_DUNGEON_KYOUKOKU_01
    {  21, 0,  21 }, // STORMY_SEA = 021 SND_BGM_M_DUNGEON_GENERAL_02
    {  22, 0,  22 }, // SKY_TOWER = 022 SND_BGM_M_DUNGEON_TENKUU_01
    {  23, 0,  23 }, // SKY_TOWER_SUMMIT = 023 SND_BGM_M_DUNGEON_TENKUU_02
    {  24, 0,  24 }, // THE_ESCAPE = 024 SND_BGM_M_EVENT_ESCAPE_01
    {  25, 0,  25 }, // MT_BLAZE = 025 SND_BGM_M_DUNGEON_HONOU_01
    {  26, 0,  26 }, // RAYQUAZAS_DOMAIN = 026 SND_BGM_M_DUNGEON_EVENTFLOOR_02
    {  27, 0,  27 }, // FRIEND_AREA_STRATOS_LOOKOUT = 027 SND_BGM_M_HABITAT_ITADAKI_01
    {  28, 0,  28 }, // FRIEND_AREA_RAINBOW_PEAK = 028 SND_BGM_M_HABITAT_NIJI_01
    {  29, 0,  29 }, // DREAM_EATER = 029 SND_BGM_M_EVENT_ENV_MYSTIC
    {  30, 0,  30 }, // FRIEND_AREA_DEEPSEA_CURRENT = 030 SND_BGM_M_HABITAT_KAIRYU_01
    {  31, 0,  31 }, // FRIEND_AREA_SEAFLOOR_CAVE = 031 SND_BGM_M_HABITAT_KAITEI_01
    {  32, 0,  32 }, // BATTLE_WITH_RAYQUAZA = 032 SND_BGM_M_BOSS02
    {  33, 0,  33 }, // MT_BLAZE_PEAK = 033 SND_BGM_M_DUNGEON_HONOU_02
    {  34, 0,  34 }, // FRIEND_AREA_VOLCANIC_PIT = 034 SND_BGM_M_HABITAT_DAICHI_01
    {  35, 0,  35 }, // FRIEND_AREA_CRYPTIC_CAVE = 035 SND_BGM_M_HABITAT_NAZO_01
    {  36, 0,  36 }, // ESCAPE_THROUGH_THE_SNOW = 036 SND_BGM_M_EVENT_ESCAPE_02
    {  37, 0,  37 }, // THE_OTHER_SIDE = 037 SND_BGM_M_EVENT_ENV_SOUL
    {  38, 0,  38 }, // THE_MOUNTAIN_OF_FIRE = 038 SND_BGM_M_EVENT_ENV_HONOU
    {  39, 0,  39 }, // FROSTY_GROTTO = 039 SND_BGM_M_DUNGEON_JUHYOU_02
    {  40, 0,  40 }, // INTRO = 040 SND_BGM_M_SYS_TITLE_01
    {  41, 0,  41 }, // AFTERMATH = 041 SND_BGM_M_EVENT_SEPARATION_01
    {  42, 0,  42 }, // FAREWELL = 042 SND_BGM_M_EVENT_SEPARATION_02
    {  43, 0,  43 }, // TITLE_SCREEN = 043 SND_BGM_M_SYS_TITLE_02
    {  44, 0,  44 }, // CREDITS = 044 SND_BGM_M_SYS_ENDING_01
    {  45, 0,  45 }, // TIME_OF_REUNION = 045 SND_BGM_M_SYS_ENDING_02
    {  46, 0,  46 }, // OPENING_TITLE = 046 SND_BGM_M_SYS_TITLE_03
    {  51, 0,  51 }, // DUNGEON_FAIL = 051 SND_BGM_M_GAMEOVER
    {  52, 0,  52 }, // DUNGEON_COMPLETE = 052 SND_BGM_M_GAMECLEAR
    { 100, 0, 100 }, // FRIEND_AREA_UNUSED_1 = 100 SND_BGM_M_EVENT_ZUKKOKE
    { 101, 0, 101 }, // HEARTWARMING = 101 SND_BGM_M_EVENT_CALMLY
    { 102, 0, 102 }, // LAPIS_CAVE = 102 SND_BGM_M_DUNGEON_GUNJYOU_01
    { 103, 0, 103 }, // A_SUCCESSFUL_RESCUE = 103 SND_BGM_M_EVENT_CLEARD
    { 104, 0, 104 }, // FROSTY_FOREST = 104 SND_BGM_M_DUNGEON_JUHYOU_01
    { 105, 0, 105 }, // FRIEND_AREA_STEPPE = 105 SND_BGM_M_HABITAT_CALMLY_01
    { 106, 0, 106 }, // FRIEND_AREA_OCEANIC = 106 SND_BGM_M_HABITAT_MYSTIC_02
    { 107, 0, 107 }, // FRIEND_AREA_FIELD = 107 SND_BGM_M_HABITAT_CALMLY_03
    { 108, 0, 108 }, // MAGMA_CAVERN = 108 SND_BGM_M_DUNGEON_CHITEI_01
    { 109, 0, 109 }, // FRIEND_AREA_UNUSED_2 = 109 SND_BGM_M_SYS_TRAINING
    { 110, 0, 110 }, // MAKUHITA_DOJO = 110 SND_BGM_M_DUNGEON_TUTORIAL_01
    { 111, 0, 111 }, // MT_THUNDER = 111 SND_BGM_M_DUNGEON_RAIMEI_01
    { 112, 0, 112 }, // FRIEND_AREA_LAB = 112 SND_BGM_M_HABITAT_MYSTIC_06
    { 113, 0, 113 }, // SILENT_CHASM = 113 SND_BGM_M_DUNGEON_CHINMOKU_01
    { 114, 0, 114 }, // IN_THE_DEPTHS_OF_THE_PIT = 114 SND_BGM_M_DUNGEON_EVENTFLOOR_01
    { 115, 0, 115 }, // MT_FREEZE = 115 SND_BGM_M_DUNGEON_REIHOU_01
    { 116, 0, 116 }, // FRIEND_AREA_WILDS = 116 SND_BGM_M_HABITAT_MYSTIC_03
    { 117, 0, 117 }, // FRIEND_AREA_LEGENDARY_ISLAND = 117 SND_BGM_M_HABITAT_DENSETSU_01
    { 118, 0, 118 }, // FRIEND_AREA_SOUTHERN_ISLAND = 118 SND_BGM_M_HABITAT_MINAMI_01
    { 119, 0, 119 }, // FRIEND_AREA_ENCLOSED_ISLAND = 119 SND_BGM_M_HABITAT_TOZASARETA_01
    { 120, 0, 120 }, // MT_STEEL = 120 SND_BGM_M_DUNGEON_HAGANE_01
    { 121, 0, 121 }, // FRIEND_AREA_FOREST = 121 SND_BGM_M_HABITAT_MYSTIC_04
    { 122, 0, 122 }, // FRIEND_AREA_FINAL_ISLAND = 122 SND_BGM_M_HABITAT_SAIGO_01
    { 123, 0, 123 }, // MT_FREEZE_PEAK = 123 SND_BGM_M_DUNGEON_REIHOU_02
    { 124, 0, 124 }, // MAGMA_CAVERN_PIT = 124 SND_BGM_M_DUNGEON_CHITEI_02
    { 125, 0, 125 }, // TINY_WOODS = 125 SND_BGM_M_DUNGEON_CHIISANAMORI_01
    { 126, 0, 126 }, // MT_THUNDER_PEAK = 126 SND_BGM_M_DUNGEON_RAIMEI_02
    { 127, 0, 127 }, // FRIEND_AREA_HEALING_FOREST = 127 SND_BGM_M_HABITAT_IYASHI_01
    { 128, 0, 128 }, // MONSTER_HOUSE = 128 SND_BGM_M_SYS_MONSTER
    { 200, 0, 200 }, // FANFARE_200 = 200 SND_ME_M_LEVELUP
    { 201, 0, 201 }, // FANFARE_201 = 201 SND_ME_M_RANKUP
    { 202, 0, 202 }, // FANFARE_202 = 202 SND_ME_M_SEEYOU
    { 203, 0, 203 }, // FANFARE_203 = 203 SND_ME_M_REWARD
    { 204, 0, 204 }, // FANFARE_204 = 204 SND_ME_M_PARTY
    { 205, 0, 205 }, // FANFARE_205 = 205 SND_ME_M_DUNGEON_OPEN
    { 206, 0, 206 }, // FANFARE_206 = 206 SND_ME_M_AREA_OPEN
    { 207, 0, 207 }, // FANFARE_207 = 207 SND_ME_M_INFORMATION
    { 208, 0, 208 }, // FANFARE_208 = 208 SND_ME_M_FANFARE
    { 209, 0, 210 }, // FANFARE_209 = 210 SND_ME_M_EVOLUTION
    { 210, 0, 210 }, // FANFARE_210 = 210 SND_ME_M_EVOLUTION
    { 211, 0, 211 }, // FANFARE_211 = 211 SND_ME_M_EVOLUTION_02
    { 212, 0, 212 }, // FANFARE_212 = 212 SND_ME_M_ITEM_IMPORTANT
    { 213, 0, 213 }, // FANFARE_213 = 213 SND_ME_M_NEWS
    { 214, 0, 214 }, // FANFARE_214 = 214 SND_ME_M_MUSICBOX
    { 215, 0, 215 }, // FANFARE_215 = 215 SND_ME_M_EFF_ORUGORU
    { 216, 0, 216 }, // FANFARE_216 = 216 SND_ME_M_WIND_S
    { 217, 0, 217 }, // FANFARE_217 = 217 SND_ME_M_WIND_M
    { 218, 0, 218 }, // FANFARE_218 = 218 SND_ME_M_WAVE_S
};

static void LoadMap(void);
static void StopSlots(void);

void Port_MusicClose(void);

int Port_MusicOpen(void)
{
    if (sOpen)
        return sCount;
    if (nds_music_open(&sRom, MUSIC_FILE) != NDSM_OK)
        return -1;
    sOpen = 1;

    int total = 0;
    for (int i = 0; i < sRom.num_sdats; i++)
        total += (int)(sRom.sdat[i].num_seq + sRom.sdat[i].num_strm);
    sTracks = calloc((size_t)(total ? total : 1), sizeof(Track));
    if (sTracks == NULL) {
        Port_MusicClose();
        return -1;
    }
    // As the NDS Music Player lists them: sequences, then streams.
    for (int i = 0; i < sRom.num_sdats; i++) {
        const Sdat *s = &sRom.sdat[i];
        char pre[8] = "";
        if (sRom.num_sdats > 1)
            snprintf(pre, sizeof(pre), "%d:", i + 1);
        for (uint32_t q = 0; q < s->num_seq; q++) {
            if (!s->seq[q].present)
                continue;
            Track *t = &sTracks[sCount++];
            const char *nm = sdat_seq_name(s, q);
            t->sdat = (uint8_t)i;
            t->index = (uint16_t)q;
            snprintf(t->label, sizeof(t->label), "%s%03u %s", pre, (unsigned)q, nm ? nm : "(sequence)");
        }
        for (uint32_t q = 0; q < s->num_strm; q++) {
            if (!s->strm[q].present)
                continue;
            Track *t = &sTracks[sCount++];
            const char *nm = sdat_strm_name(s, q);
            t->sdat = (uint8_t)i;
            t->isStream = 1;
            t->index = (uint16_t)q;
            snprintf(t->label, sizeof(t->label), "%sS%02u %s", pre, (unsigned)q, nm ? nm : "(stream)");
        }
    }
    if (sCount == 0) {
        Port_MusicClose();
        return -1;
    }
    LoadMap();
    return sCount;
}

void Port_MusicStop(void)
{
    Song *g = sSong;
    sSong = NULL;
    song_close(g);
    sPlaying = -1;
    sPaused = 0;
}

void Port_MusicClose(void)
{
    Port_MusicStop();
    StopSlots(); // (a stream reads the ROM file as it plays)
    if (sOpen)
        nds_music_close(&sRom);
    sOpen = 0;
    free(sTracks);
    sTracks = NULL;
    sCount = 0;
}

int Port_MusicCount(void)
{
    return sCount;
}

const char *Port_MusicLabel(int i)
{
    return i >= 0 && i < sCount ? sTracks[i].label : "";
}

int Port_MusicIsStream(int i)
{
    return i >= 0 && i < sCount && sTracks[i].isStream;
}

const char *Port_MusicRomTitle(void)
{
    static char title[24];
    if (!sOpen)
        return "";
    snprintf(title, sizeof(title), "%s (%s)", sRom.hdr.title, sRom.hdr.gamecode);
    return title;
}

// The ROM's DS Menu icon (its first frame) at twice its size, 64x64: 8x8
// tiles of 4bpp, row by row (2048 bytes), and its 16 colours as r, g, b, 0
// (64 bytes). 0 if it has none.
int Port_MusicIcon(uint8_t *tiles, uint8_t *colors)
{
    const NdsBanner *b = &sRom.banner;
    if (!sOpen || !b->valid)
        return 0;
    memset(tiles, 0, 64 * 32);
    for (int py = 0; py < 64; py++)
        for (int px = 0; px < 64; px++) {
            int bmp = b->frame[0].bitmap; // (an animated icon: its first frame)
            int c = b->pixels[bmp][(py / 2) * BANNER_ICON_W + px / 2] & 15;
            uint8_t *t = tiles + ((py / 8) * 8 + px / 8) * 32 + (py % 8) * 4 + (px % 8) / 2;
            *t |= (px & 1) ? (uint8_t)(c << 4) : (uint8_t)c;
        }
    for (int i = 0; i < 16; i++) {
        uint16_t c = b->palette[b->frame[0].palette][i];
        colors[i * 4] = (uint8_t)((c & 31) << 3);
        colors[i * 4 + 1] = (uint8_t)(((c >> 5) & 31) << 3);
        colors[i * 4 + 2] = (uint8_t)(((c >> 10) & 31) << 3);
        colors[i * 4 + 3] = 0;
    }
    return 1;
}

const char *Port_MusicGameCode(void)
{
    return sOpen ? sRom.hdr.gamecode : "";
}

// Blue Rescue Team's ROM: APHE (North America), APHP (Europe), APHJ (Japan)
int Port_MusicRomIsBlue(void)
{
    const char *c = sRom.hdr.gamecode;
    return sOpen && c[0] == 'A' && c[1] == 'P' && c[2] == 'H' && (c[3] == 'E' || c[3] == 'P' || c[3] == 'J');
}

// --- GBA song -> DS track ---------------------------------------------------

static int FindTrack(int stream, int index)
{
    for (int i = 0; i < sCount; i++)
        if (sTracks[i].sdat == 0 && sTracks[i].isStream == stream && sTracks[i].index == index)
            return i;
    return -1;
}

// The built-in matches, then music-map.txt's ("GBA <song> = SEQ <n>",
// "= STRM <n>" or "= NONE"; anything after a '#' is a comment).
static void LoadMap(void)
{
    for (int i = 0; i < MAP_SONGS; i++)
        sMap[i] = -1;
    for (size_t i = 0; i < sizeof(sDefaultMap) / sizeof(sDefaultMap[0]); i++)
        if (sDefaultMap[i].gba > 0 && sDefaultMap[i].gba < MAP_SONGS)
            sMap[sDefaultMap[i].gba] = (int16_t)FindTrack(sDefaultMap[i].stream, sDefaultMap[i].index);

    void *f = Backend_FileOpen(MAP_FILE);
    if (f == NULL)
        return;
    uint32_t size = Backend_FileSize(f);
    char *text = malloc(size + 1);
    if (text != NULL && Backend_FileRead(f, 0, text, size)) {
        text[size] = '\0';
        for (int i = 0; i < MAP_SONGS; i++)
            sMap[i] = -1; // (the file has every match)
        for (char *line = text; line != NULL && *line; ) {
            char *next = strchr(line, '\n');
            if (next != NULL)
                *next++ = '\0';
            int gba, n;
            char kind[8];
            if (sscanf(line, "GBA %d = %7s %d", &gba, kind, &n) >= 2 && gba > 0 && gba < MAP_SONGS) {
                if (strcmp(kind, "SEQ") == 0)
                    sMap[gba] = (int16_t)FindTrack(0, n);
                else if (strcmp(kind, "STRM") == 0)
                    sMap[gba] = (int16_t)FindTrack(1, n);
            }
            line = next;
        }
        Backend_Log("music: %s loaded", MAP_FILE);
    }
    free(text);
    Backend_FileClose(f);
}

int Port_MusicMapGet(int gba)
{
    return gba > 0 && gba < MAP_SONGS ? sMap[gba] : -1;
}

void Port_MusicMapSet(int gba, int track)
{
    if (gba > 0 && gba < MAP_SONGS)
        sMap[gba] = (int16_t)(track >= 0 && track < sCount ? track : -1);
}

// Writes every match (songs[i], named names[i]) to music-map.txt.
int Port_MusicMapSave(const int *songs, const char *const *names, int count)
{
    size_t cap = (size_t)count * 96 + 256, len = 0;
    char *text = malloc(cap);
    if (text == NULL)
        return 0;
    len += (size_t)snprintf(text + len, cap - len,
                            "# Music Matching: GBA song = DS track of %s\n", Port_MusicRomTitle());
    for (int i = 0; i < count && len < cap; i++) {
        int t = Port_MusicMapGet(songs[i]);
        if (t < 0)
            len += (size_t)snprintf(text + len, cap - len, "GBA %d = NONE  # %s\n", songs[i], names[i]);
        else
            len += (size_t)snprintf(text + len, cap - len, "GBA %d = %s %u  # %s = %s\n", songs[i],
                                    sTracks[t].isStream ? "STRM" : "SEQ", (unsigned)sTracks[t].index,
                                    names[i], sTracks[t].label);
    }
    if (len > cap)
        len = cap;
    int ok = Backend_FileWrite(MAP_FILE, text, (uint32_t)len);
    free(text);
    return ok;
}

// --- Music Style: DS -------------------------------------------------------------

static void StopSlots(void)
{
    for (int i = 0; i < 2; i++) {
        song_close(sSlots[i].song);
        sSlots[i].song = NULL;
        sSlots[i].track = -1;
        sSlots[i].paused = 0;
    }
}

// Slot 0 (background music) or 1 (fanfare): which track plays (-1: none),
// whether it loops for ever, is paused, and its volume (0-64, the GBA
// player's, which follows fades). A track that changes starts over.
void Port_MusicSlot(int slot, int track, int loop, int paused, int volume64)
{
    Slot *s = &sSlots[slot & 1];
    if (!sOpen)
        track = -1;
    if (track != s->track) {
        song_close(s->song);
        s->song = NULL;
        s->track = track;
        if (track >= 0 && track < sCount) {
            const Track *t = &sTracks[track];
            const Sdat *sd = &sRom.sdat[t->sdat];
            s->song = t->isStream ? song_open_strm(sd, t->index, sMixRate) : song_open_seq(sd, t->index, sMixRate);
            if (s->song != NULL)
                song_set_loops(s->song, loop ? 0 : 1, 0);
        }
    }
    s->paused = paused;
    if (s->song != NULL)
        song_set_volume(s->song, sVolume * (volume64 < 0 ? 0 : volume64 > 64 ? 64 : volume64) / 64);
}

int Port_MusicSlotDone(int slot)
{
    const Slot *s = &sSlots[slot & 1];
    return s->song == NULL || song_done(s->song);
}

static void ApplyTiming(Song *g)
{
    if (sRepeat)
        song_set_loops(g, 0, 0); // forever
    else
        song_set_loops(g, 2, 8000); // two loops, then an 8 s fade
}

int Port_MusicPlay(int i)
{
    if (!sOpen || i < 0 || i >= sCount)
        return 0;
    Port_MusicStop(); // (a stream reads the ROM file as it plays)
    const Track *t = &sTracks[i];
    const Sdat *s = &sRom.sdat[t->sdat];
    Song *g = t->isStream ? song_open_strm(s, t->index, sMixRate) : song_open_seq(s, t->index, sMixRate);
    sPlaying = i;
    if (g == NULL)
        return 0;
    ApplyTiming(g);
    song_set_volume(g, sVolume);
    sSong = g;
    return 1;
}

int Port_MusicPlaying(void)
{
    return sSong != NULL ? sPlaying : -1;
}

void Port_MusicSetPaused(int paused)
{
    sPaused = paused && sSong != NULL;
}

int Port_MusicPaused(void)
{
    return sPaused;
}

void Port_MusicSetVolume(int percent)
{
    sVolume = percent;
    if (sSong != NULL)
        song_set_volume(sSong, percent);
}

void Port_MusicSetRepeat(int repeat)
{
    sRepeat = repeat;
    if (sSong != NULL)
        ApplyTiming(sSong);
}

int Port_MusicRepeat(void)
{
    return sRepeat;
}

int Port_MusicDone(void)
{
    return sSong != NULL && song_done(sSong);
}

unsigned Port_MusicPosMs(void)
{
    return sSong != NULL ? song_pos_ms(sSong) : 0;
}

int Port_MusicLoops(void)
{
    return sSong != NULL ? song_loops(sSong) : 0;
}

unsigned Port_MusicVoices(void)
{
    return sSong != NULL ? song_voices(sSong) : 0;
}

// One frame of the game's sound (count stereo samples at rate): with a track
// playing, the frame goes out at twice the rate, with the track (rendered at
// that rate, near the DS's own 32728 Hz) added. Returns the samples now in
// out2 (0: send the game's frame as it is).
int Port_MusicMix(const int16_t *in, int count, int rate, int16_t *out2, int *outRate)
{
    static int16_t last[2];
    static int16_t track[2048 * 2];

    if (rate > 0)
        sMixRate = 2 * (uint32_t)rate;
    // (at twice the rate whenever the ROM is open, so that the rate doesn't
    // change, nor the sound with it, between one track and the next)
    if (!sOpen || count <= 0 || count > 1024)
        return 0;
    // The game's samples, at twice the rate (each one, then the average with the next)
    for (int i = 0; i < count; i++) {
        for (int c = 0; c < 2; c++) {
            int16_t prev = i == 0 ? last[c] : in[(i - 1) * 2 + c];
            out2[i * 4 + c] = (int16_t)((prev + in[i * 2 + c]) / 2);
            out2[i * 4 + 2 + c] = in[i * 2 + c];
        }
    }
    last[0] = in[(count - 1) * 2];
    last[1] = in[(count - 1) * 2 + 1];
    for (int k = -1; k < 2; k++) { // the player's track, then the slots'
        Song *g = k < 0 ? (sPaused ? NULL : sSong) : (sSlots[k].paused ? NULL : sSlots[k].song);
        if (g == NULL)
            continue;
        song_render(g, track, (uint32_t)count * 2);
        for (int i = 0; i < count * 4; i++) {
            int v = out2[i] + track[i];
            out2[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
    }
    *outRate = 2 * rate;
    return count * 2;
}
