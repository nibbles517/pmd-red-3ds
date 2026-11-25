// Music Options: the DS version's music (Pokémon Mystery Dungeon: Blue Rescue
// Team), played from its ROM by the NDS Music Player's core (port/ndsmusic).
// The ROM is wherever it was picked on the SD card (Music Options finds it:
// gPortDsRom, config.ini's DS ROM), or else pmd-blue.nds in the port's data
// folder.
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

#ifndef PORT_SFX_SLOTS
#define PORT_SFX_SLOTS 6 // one for each GBA sound effect player (port_game.h)
#endif
#include "../ndsmusic/nds_music.h"
#include "../ndsmusic/song.h"

#define MUSIC_FILE "pmd-blue.nds"
#define LABEL_LEN 40

typedef struct {
    uint8_t sdat;
    uint8_t isStream;
    uint8_t isEffect; // a sound effect: entry index of sequence archive arc
    uint8_t isDse;    // a DSE song (Mystery Dungeon: Explorers etc.): index = its sRom.dse.songs index (sdat 0xFF)
    uint16_t arc;
    uint16_t index;
    uint16_t gain;    // its own volume, percent (sDefaultMusicGain/sDefaultGain, ds-volumes.txt)
    uint16_t defGain; // ... the built-in one (sDefaultMusicGain/sDefaultGain)
    char label[LABEL_LEN];
} Track;

static NdsMusic sRom;
static int sOpen;
// Music Options > Extra Music: Explorers of Sky's ROM (gPortExtraRom), its DSE
// songs played on some dungeon floors (Port_ExtraMusicFor). A slot plays one
// as track EXTRA_TRACK + its index in sExtra.dse.
static NdsMusic sExtra;
static int sExtraOpen;
#define EXTRA_TRACK 0x4000
// The Music Box's Custom DS Music (in dungeons): another game's ROM picked on
// the SD card, its music played in the background music's slot as track
// CUSTOM_TRACK + its index in sCustomTracks.
static NdsMusic sCustom;
static int sCustomOpen;
#define CUSTOM_TRACK 0x6000
static uint8_t *sExtraGain;     // each of its songs' own volume (percent; extra-volumes.txt)
static int sExtraVolume = 100;  // Music Options > Extra Volume (percent): all its songs
#define EXTRA_GAIN_MAX 200
static int ExtraVolume(int track) // (its volume, percent: Extra Volume and the song's own)
{
    int idx = track - EXTRA_TRACK;
    return sExtraOpen && sExtraGain != NULL && idx >= 0 && idx < sExtra.dse.num ? sExtraVolume * sExtraGain[idx] / 100 : sExtraVolume;
}
static Track *sTracks;
static int sCount, sSongCount; // all the tracks; the music (the sound effects come after it)

static Song *sSong;
static int sPlaying = -1; // the track playing (or stopped mid-way: -1 once stopped)
static int sPaused, sRepeat;
static int sVolume = 190; // percent (Music Options sets it)
static int sSfxVolume = 100; // ... the sound effects' (theirs, in the Music Player too)
static uint32_t sMixRate = 2 * 18157; // twice the game's mixing rate (see Port_MusicMix)

// Music Style: DS: the game's background music and fanfare, as DS tracks;
// Sound Effects: DS: its sound effects (one slot for each GBA sound effect player)
enum { SLOT_BGM, SLOT_FANFARE, SLOT_SFX, SLOTS = SLOT_SFX + PORT_SFX_SLOTS };
typedef struct {
    Song *song;     // (NULL: none, or an effect that ended)
    int track;      // -1: none
    unsigned serial;// (an effect: started again when it changes)
    int want;       // what the game asked for, opened when the next frame is made
    unsigned wantSerial;
    int loop;
    int ending, ended; // an effect: to end at its next loop (asked; done)
    int paused;
    int volume64;   // the GBA player's volume (applied when the next frame is made)
} Slot;
static Slot sSlots[SLOTS];
static int sSlotsInit;

// --- Making the sound: on the second core ----------------------------------------
//
// Each frame, Sound_EndFrame (Port_MusicMix) adds the DS songs' samples made
// during the frame before, and has the next frame's made: on the second core
// (Port_MusicRenderJob, Backend_RunAsync) while the game runs its frame, when
// Dual Core is on, else at once. So the DS music is a frame later than the
// game's own sound (about 17 ms).
//
// While that job runs it alone touches the songs. Of the calls below, those
// of the player that open, close or change a song first wait for it to finish
// (MusicSync); the slots (what the game plays) never wait: what is asked of
// them (track, pause, volume) is kept and done when the next job is given;
// the questions (done? where?) are answered from what the last job found.

enum { JOB_PLAYER, JOB_SLOT0, JOB_SONGS = JOB_SLOT0 + SLOTS };
static Song *sJobSong[JOB_SONGS];   // what the job makes (NULL: nothing)
static int16_t sJobOut[JOB_SONGS][2048 * 2];
static uint8_t sJobHas[JOB_SONGS];  // ... made into sJobOut
static uint32_t sJobCount;          // samples a song (at the mixing rate)
static int sJobPending;             // given and not waited for
static struct {
    uint8_t done[JOB_SONGS];
    unsigned posMs, voices;
    int loops;
} sJobFound, sFound;                // what the job found out; as last collected

void Port_MusicRenderJob(void *unused)
{
    (void)unused;
    for (int k = 0; k < JOB_SONGS; k++) {
        Song *g = sJobSong[k];
        sJobHas[k] = 0;
        if (g == NULL)
            continue;
        song_render(g, sJobOut[k], sJobCount);
        sJobHas[k] = 1;
        sJobFound.done[k] = (uint8_t)song_done(g);
        if (k == JOB_PLAYER) {
            sJobFound.posMs = song_pos_ms(g);
            sJobFound.loops = song_loops(g);
            sJobFound.voices = song_voices(g);
        }
    }
}

// The job (if any) finished; what it found collected
static void MusicSync(void)
{
    if (!sJobPending)
        return;
    Backend_WaitAsync();
    sJobPending = 0;
    sFound = sJobFound;
}

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
static void LoadSfxMap(void);
static void StopSlots(void);

void Port_MusicClose(void);

// Another game's ROM picked in Music Options: its music there until the game
// is closed (not kept in config.ini); "" for none
static char sSessionRom[256];

static const char *RomPath(void)
{
    return sSessionRom[0] ? sSessionRom : gPortDsRom[0] ? gPortDsRom : MUSIC_FILE;
}

const char *Port_MusicRomPath(void)
{
    return sSessionRom[0] ? sSessionRom : gPortDsRom;
}

int Port_MusicOpen(void)
{
    if (sOpen)
        return sCount;
    MusicSync();
    if (nds_music_open(&sRom, RomPath()) != NDSM_OK)
        return -1;
    sOpen = 1;

    int total = 0;
    for (int i = 0; i < sRom.num_sdats; i++) {
        total += (int)(sRom.sdat[i].num_seq + sRom.sdat[i].num_strm);
        for (uint32_t a = 0; a < sRom.sdat[i].num_seqarc; a++) {
            SdatSsarEntry *ents;
            total += (int)sdat_ssar_entries(&sRom.sdat[i], a, &ents);
            free(ents);
        }
    }
    total += sRom.dse.num; // (games on Procyon's DSE engine: its songs)
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
    // Games on the DSE engine (Mystery Dungeon: Explorers): music from SOUND/BGM and SOUND/ME,
    // as the player lists it
    for (int i = 0; i < sRom.dse.num && sCount < total; i++) {
        const DseSong *d = &sRom.dse.songs[i];
        Track *t = &sTracks[sCount++];
        t->sdat = 0xFF; // (no SDAT's: not matched with the GBA songs)
        t->isDse = 1;
        t->index = (uint16_t)i;
        if (d->group == DSE_GROUP_ME)
            snprintf(t->label, sizeof(t->label), "ME%02u %s", (unsigned)d->number, d->label);
        else
            snprintf(t->label, sizeof(t->label), "%03u %s", (unsigned)d->number, d->label);
    }
    sSongCount = sCount;
    // Then the sound effects (sequence archives' entries), as the player lists them
    for (int i = 0; i < sRom.num_sdats; i++) {
        const Sdat *s = &sRom.sdat[i];
        char pre[8] = "";
        if (sRom.num_sdats > 1)
            snprintf(pre, sizeof(pre), "%d:", i + 1);
        for (uint32_t a = 0; a < s->num_seqarc; a++) {
            SdatSsarEntry *ents;
            uint32_t n = sdat_ssar_entries(s, a, &ents);
            const char *an = sdat_ssar_name(s, a);
            for (uint32_t q = 0; q < n && sCount < total; q++) {
                if (!ents[q].valid)
                    continue;
                Track *t = &sTracks[sCount++];
                const char *nm = sdat_ssar_entry_name(s, a, q);
                t->sdat = (uint8_t)i;
                t->isEffect = 1;
                t->arc = (uint16_t)a;
                t->index = (uint16_t)q;
                snprintf(t->label, sizeof(t->label), "%sE%02u.%03u %s", pre, (unsigned)a, (unsigned)q,
                         nm ? nm : an ? an : "(effect)");
            }
            free(ents);
        }
    }
    if (sCount == 0) {
        Port_MusicClose();
        return -1;
    }
    LoadMap();
    LoadSfxMap();
    return sCount;
}

void Port_MusicStop(void)
{
    MusicSync();
    Song *g = sSong;
    sSong = NULL;
    song_close(g);
    sPlaying = -1;
    sPaused = 0;
}

void Port_MusicClose(void)
{
    MusicSync();
    Port_MusicStop();
    StopSlots(); // (a stream reads the ROM file as it plays)
    if (sOpen)
        nds_music_close(&sRom);
    sOpen = 0;
    free(sTracks);
    sTracks = NULL;
    sCount = sSongCount = 0;
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

int Port_MusicIsEffect(int i)
{
    return i >= 0 && i < sCount && sTracks[i].isEffect;
}

int Port_MusicSongCount(void)
{
    return sSongCount;
}

// A track's song, at the mixing rate
static Song *OpenTrack(const Track *t)
{
    if (t->isDse)
        return song_open_dse(&sRom.dse, t->index, sMixRate);
    const Sdat *s = &sRom.sdat[t->sdat];
    if (t->isEffect)
        return song_open_ssar(s, t->arc, t->index, sMixRate);
    return t->isStream ? song_open_strm(s, t->index, sMixRate) : song_open_seq(s, t->index, sMixRate);
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

// Music Options found a ROM (path on the SD card, "/..."): opened in place of
// the one before. 1: Blue Rescue Team's, kept (config.ini) for next time;
// 0: another game's, its music in Music Options only, until the game is
// closed; -1: not a ROM that opens (the one before stays)
int Port_MusicTryRom(const char *path)
{
    char before[256];
    int ok;

    if (strlen(path) >= sizeof(sSessionRom))
        return -1;
    memcpy(before, sSessionRom, sizeof(before));
    Port_MusicClose();
    snprintf(sSessionRom, sizeof(sSessionRom), "%s", path);
    ok = Port_MusicOpen() > 0;
    if (!ok) {
        memcpy(sSessionRom, before, sizeof(sSessionRom));
        return -1;
    }
    if (!Port_MusicRomIsBlue())
        return 0;
    sSessionRom[0] = 0;
    snprintf(gPortDsRom, sizeof(gPortDsRom), "%s", path);
    Port_SaveConfig();
    return 1;
}

// --- GBA song -> DS track ---------------------------------------------------

static int FindTrack(int stream, int index)
{
    for (int i = 0; i < sCount; i++)
        if (sTracks[i].sdat == 0 && !sTracks[i].isEffect && sTracks[i].isStream == stream && sTracks[i].index == index)
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

// --- GBA sound effect -> DS sound effect ------------------------------------------
//
// Each of the game's sound effects (songs 300-999) is matched with one of
// the ROM's (a sequence archive entry). Built in: the same number in the
// first archive (GBA sound effect n = DS E00.n; Sound Effect Matching showed
// the DS version kept the GBA's numbering), when the ROM has that entry.
// sfx-map.txt (as Sound Effect Matching wrote it, while the matches were
// made) changes those it names: "SE <id> = E<archive>.<entry>" ("= NONE"
// lines are left as built in). Each DS track's own volume: below.

#define SFX_FIRST 300
#define SFX_LAST 999
#define SFX_FILE "sfx-map.txt"
static int16_t sSfxMap[SFX_LAST - SFX_FIRST + 1]; // track index, -1: none

static int FindEffect(int arc, int index)
{
    for (int i = sSongCount; i < sCount; i++)
        if (sTracks[i].sdat == 0 && sTracks[i].isEffect && sTracks[i].arc == arc && sTracks[i].index == index)
            return i;
    return -1;
}

#define SFX_GAIN_MAX 1000
#define MUSIC_GAIN_MAX 500
#define VOL_FILE "ds-volumes.txt"

// Each DS track's own volume (percent, 100: as the ROM has it), so that it's
// as loud in the port as the GBA song or sound effect it stands for: measured
// (ds-vs-gba-loudness.xlsx: the loudness of each, the DS music at the Music
// volume 190%; songs over their first 20 s, sound effects their loudest
// 0.4 s). ds-volumes.txt changes them ("MUSIC SEQ <n> = <percent>",
// "SFX E<archive>.<entry> = <percent>"): DS Volume Balance (Game Options,
// options_menu1.c) writes it, with only the volumes that aren't the
// built-in ones (so a player's file is their changes, to be merged into
// these tables; and a later build's own changes to them still count).
static const struct {
    uint16_t seq, percent;
} sDefaultMusicGain[] = {
    { 1, 115 }, { 2, 150 }, { 3, 135 }, { 4, 75 }, { 5, 85 }, { 6, 75 }, { 7, 110 }, { 8, 80 },
    { 9, 85 }, { 10, 125 }, { 11, 120 }, { 12, 90 }, { 14, 95 }, { 15, 135 }, { 16, 150 }, { 17, 105 },
    { 18, 150 }, { 19, 125 }, { 20, 135 }, { 21, 95 }, { 22, 105 }, { 23, 215 }, { 24, 130 }, { 25, 105 },
    { 26, 170 }, { 27, 130 }, { 28, 140 }, { 29, 145 }, { 30, 265 }, { 31, 170 }, { 32, 105 }, { 33, 115 },
    { 34, 135 }, { 35, 140 }, { 36, 130 }, { 37, 80 }, { 38, 195 }, { 39, 105 }, { 40, 135 }, { 41, 95 },
    { 42, 95 }, { 43, 165 }, { 44, 85 }, { 45, 175 }, { 46, 145 }, { 51, 190 }, { 52, 315 }, { 100, 220 },
    { 101, 195 }, { 102, 165 }, { 103, 235 }, { 104, 180 }, { 105, 165 }, { 106, 170 }, { 107, 240 }, { 108, 265 },
    { 109, 275 }, { 110, 180 }, { 111, 285 }, { 112, 385 }, { 113, 305 }, { 114, 90 }, { 115, 215 }, { 116, 175 },
    { 117, 220 }, { 118, 395 }, { 119, 290 }, { 120, 140 }, { 121, 120 }, { 122, 265 }, { 123, 245 }, { 124, 165 },
    { 125, 150 }, { 126, 280 }, { 127, 265 }, { 128, 180 }, { 200, 255 }, { 201, 245 }, { 202, 150 }, { 203, 235 },
    { 204, 290 }, { 205, 330 }, { 206, 300 }, { 207, 175 }, { 208, 290 }, { 210, 385 }, { 211, 240 }, { 212, 240 },
    { 213, 240 }, { 214, 145 }, { 215, 265 }, { 216, 110 }, { 217, 80 }, { 218, 155 },
};

static const struct {
    uint16_t entry, percent;
} sDefaultGain[] = {
    { 301, 540 }, { 302, 230 }, { 303, 210 }, { 304, 1000 }, { 305, 630 }, { 306, 170 }, { 307, 110 }, { 308, 210 },
    { 309, 220 }, { 310, 240 }, { 311, 90 }, { 312, 180 }, { 330, 250 }, { 331, 310 }, { 332, 180 }, { 333, 180 },
    { 336, 320 }, { 337, 670 }, { 338, 300 }, { 339, 420 }, { 340, 170 }, { 341, 180 }, { 342, 290 }, { 343, 330 },
    { 344, 120 }, { 345, 360 }, { 346, 160 }, { 347, 150 }, { 348, 180 }, { 351, 120 }, { 352, 350 }, { 353, 230 },
    { 354, 180 }, { 355, 220 }, { 356, 140 }, { 357, 160 }, { 358, 320 }, { 359, 130 }, { 360, 280 }, { 361, 130 },
    { 362, 270 }, { 363, 170 }, { 365, 60 }, { 366, 190 }, { 367, 170 }, { 368, 240 }, { 369, 170 }, { 370, 550 },
    { 371, 220 }, { 372, 200 }, { 390, 170 }, { 391, 150 }, { 392, 140 }, { 400, 130 }, { 401, 150 }, { 402, 120 },
    { 403, 190 }, { 404, 370 }, { 405, 290 }, { 406, 170 }, { 407, 160 }, { 408, 230 }, { 409, 140 }, { 410, 140 },
    { 412, 140 }, { 413, 200 }, { 414, 170 }, { 415, 210 }, { 416, 160 }, { 417, 190 }, { 418, 130 }, { 419, 330 },
    { 420, 260 }, { 421, 140 }, { 422, 290 }, { 423, 200 }, { 425, 200 }, { 426, 120 }, { 450, 140 }, { 451, 270 },
    { 452, 260 }, { 453, 230 }, { 454, 240 }, { 455, 410 }, { 456, 1000 }, { 457, 280 }, { 458, 190 }, { 459, 330 },
    { 460, 190 }, { 461, 1000 }, { 462, 1000 }, { 463, 470 }, { 464, 750 }, { 465, 470 }, { 466, 310 }, { 467, 1000 },
    { 468, 300 }, { 469, 380 }, { 470, 190 }, { 471, 270 }, { 472, 270 }, { 473, 300 }, { 474, 230 }, { 475, 230 },
    { 476, 180 }, { 477, 230 }, { 478, 130 }, { 479, 320 }, { 480, 290 }, { 481, 260 }, { 482, 140 }, { 483, 210 },
    { 484, 150 }, { 485, 400 }, { 486, 300 }, { 487, 300 }, { 488, 180 }, { 489, 150 }, { 490, 150 }, { 491, 300 },
    { 492, 200 }, { 493, 90 }, { 494, 160 }, { 495, 80 }, { 497, 150 }, { 498, 130 }, { 499, 90 }, { 500, 290 },
    { 501, 300 }, { 502, 140 }, { 503, 130 }, { 504, 150 }, { 505, 220 }, { 506, 80 }, { 507, 180 }, { 508, 360 },
    { 509, 240 }, { 510, 230 }, { 511, 1000 }, { 512, 420 }, { 513, 180 }, { 514, 400 }, { 515, 330 }, { 516, 360 },
    { 517, 280 }, { 518, 180 }, { 600, 160 }, { 601, 200 }, { 602, 230 }, { 603, 180 }, { 604, 170 }, { 605, 200 },
    { 606, 160 }, { 607, 210 }, { 608, 170 }, { 609, 110 }, { 610, 210 }, { 611, 180 }, { 612, 110 }, { 613, 140 },
    { 614, 200 }, { 615, 170 }, { 616, 220 }, { 617, 170 }, { 618, 170 }, { 619, 180 }, { 620, 180 }, { 621, 160 },
    { 622, 190 }, { 623, 150 }, { 624, 240 }, { 625, 190 }, { 626, 200 }, { 627, 240 }, { 628, 170 }, { 629, 120 },
    { 630, 210 }, { 631, 180 }, { 632, 130 }, { 636, 120 }, { 638, 130 }, { 639, 170 }, { 640, 130 }, { 641, 180 },
    { 642, 160 }, { 643, 190 }, { 644, 250 }, { 645, 190 }, { 646, 200 }, { 647, 140 }, { 648, 230 }, { 649, 180 },
    { 650, 200 }, { 651, 170 }, { 652, 220 }, { 653, 220 }, { 654, 310 }, { 655, 260 }, { 656, 120 }, { 658, 240 },
    { 659, 160 }, { 660, 140 }, { 661, 190 }, { 662, 190 }, { 663, 190 }, { 664, 210 }, { 665, 280 }, { 666, 190 },
    { 667, 210 }, { 668, 150 }, { 669, 120 }, { 670, 270 }, { 671, 130 }, { 672, 200 }, { 673, 170 }, { 674, 130 },
    { 675, 190 }, { 676, 130 }, { 677, 130 }, { 678, 130 }, { 679, 160 }, { 680, 170 }, { 681, 140 }, { 682, 150 },
    { 683, 140 }, { 684, 140 }, { 685, 150 }, { 686, 180 }, { 687, 180 }, { 688, 130 }, { 689, 130 }, { 690, 140 },
    { 691, 140 }, { 692, 260 }, { 693, 210 }, { 694, 210 }, { 695, 150 }, { 696, 160 }, { 697, 550 }, { 698, 160 },
    { 699, 210 }, { 700, 200 }, { 701, 130 }, { 702, 190 }, { 703, 280 }, { 704, 170 }, { 705, 200 }, { 706, 140 },
    { 707, 170 }, { 708, 200 }, { 709, 150 }, { 710, 190 }, { 711, 230 }, { 712, 190 }, { 713, 220 }, { 714, 210 },
    { 715, 190 }, { 716, 140 }, { 717, 190 }, { 718, 200 }, { 719, 230 }, { 720, 140 }, { 721, 180 }, { 722, 170 },
    { 723, 160 }, { 724, 160 }, { 725, 140 }, { 726, 110 }, { 727, 120 }, { 728, 210 }, { 729, 210 }, { 730, 270 },
    { 731, 270 }, { 732, 150 }, { 733, 160 }, { 734, 160 }, { 735, 190 }, { 736, 150 }, { 737, 210 }, { 738, 260 },
    { 739, 220 }, { 740, 290 }, { 741, 180 }, { 742, 170 }, { 743, 170 }, { 744, 180 }, { 745, 120 }, { 746, 140 },
    { 747, 170 }, { 748, 110 }, { 749, 130 }, { 750, 230 }, { 751, 210 }, { 752, 180 }, { 753, 190 }, { 754, 230 },
    { 756, 260 }, { 757, 170 }, { 758, 160 }, { 759, 150 }, { 760, 200 }, { 761, 160 }, { 762, 150 }, { 763, 220 },
    { 764, 180 }, { 765, 240 }, { 766, 240 }, { 767, 180 }, { 768, 150 }, { 769, 330 }, { 770, 150 }, { 771, 240 },
    { 772, 360 }, { 773, 300 }, { 774, 160 }, { 775, 260 }, { 776, 120 }, { 777, 170 }, { 778, 190 }, { 779, 170 },
    { 780, 200 }, { 781, 140 }, { 782, 150 }, { 783, 150 }, { 784, 190 }, { 785, 240 }, { 786, 210 }, { 787, 230 },
    { 788, 200 }, { 789, 380 }, { 790, 160 }, { 791, 150 }, { 792, 170 }, { 793, 160 }, { 794, 170 }, { 795, 80 },
    { 796, 80 }, { 797, 80 }, { 798, 140 }, { 799, 130 }, { 800, 190 }, { 801, 210 }, { 802, 120 }, { 803, 160 },
    { 805, 180 }, { 808, 180 }, { 809, 200 }, { 810, 180 }, { 811, 180 }, { 812, 130 }, { 813, 160 }, { 814, 290 },
    { 815, 200 }, { 816, 160 }, { 817, 160 }, { 818, 370 }, { 819, 170 }, { 820, 190 }, { 821, 200 }, { 822, 250 },
    { 823, 270 }, { 824, 210 }, { 825, 150 }, { 826, 170 }, { 827, 200 }, { 828, 180 }, { 829, 160 }, { 830, 200 },
    { 831, 220 }, { 832, 230 }, { 833, 180 }, { 834, 230 }, { 835, 210 }, { 836, 210 }, { 837, 170 }, { 838, 190 },
    { 839, 170 }, { 840, 190 }, { 841, 310 }, { 842, 170 }, { 843, 230 }, { 844, 190 }, { 845, 200 }, { 847, 140 },
    { 848, 150 }, { 849, 170 }, { 850, 110 }, { 851, 150 }, { 852, 140 }, { 853, 150 }, { 854, 160 }, { 855, 150 },
    { 856, 180 }, { 857, 170 }, { 858, 110 }, { 859, 190 }, { 860, 230 }, { 861, 190 }, { 862, 180 }, { 863, 180 },
    { 864, 150 }, { 865, 210 }, { 866, 180 }, { 867, 170 }, { 868, 240 }, { 869, 200 }, { 870, 210 }, { 871, 210 },
    { 872, 270 }, { 873, 200 }, { 874, 180 }, { 875, 220 }, { 876, 230 }, { 877, 210 }, { 878, 230 }, { 879, 200 },
    { 880, 200 }, { 881, 170 }, { 882, 210 }, { 883, 200 }, { 884, 320 }, { 885, 250 }, { 886, 130 }, { 887, 180 },
    { 888, 310 }, { 889, 200 }, { 890, 220 }, { 891, 280 }, { 892, 270 }, { 893, 160 }, { 894, 240 }, { 895, 190 },
    { 896, 150 }, { 897, 200 }, { 898, 270 }, { 899, 240 }, { 900, 180 }, { 901, 140 }, { 902, 160 }, { 903, 240 },
    { 904, 200 }, { 905, 200 }, { 906, 120 }, { 907, 140 }, { 908, 150 }, { 909, 130 }, { 910, 160 }, { 911, 140 },
    { 912, 140 }, { 913, 150 }, { 914, 150 }, { 915, 190 }, { 918, 150 }, { 919, 210 }, { 920, 190 }, { 921, 270 },
    { 922, 170 }, { 924, 370 }, { 925, 170 }, { 926, 190 }, { 927, 200 }, { 928, 250 }, { 929, 190 }, { 930, 270 },
    { 931, 460 }, { 932, 220 }, { 933, 120 }, { 934, 290 }, { 935, 240 }, { 936, 450 }, { 937, 300 }, { 938, 240 },
    { 939, 180 },
};


static int FindSeq(int seq)
{
    for (int i = 0; i < sSongCount; i++)
        if (sTracks[i].sdat == 0 && !sTracks[i].isStream && sTracks[i].index == seq)
            return i;
    return -1;
}

static int ClampGain(const Track *t, int pct)
{
    int max = t->isEffect ? SFX_GAIN_MAX : MUSIC_GAIN_MAX;
    return pct < 0 ? 0 : pct > max ? max : pct;
}

// Writes every DS track's volume to ds-volumes.txt
static int SaveVolumes(void)
{
    size_t cap = (size_t)sCount * 72 + 512, len = 0;
    char *text = malloc(cap);
    if (text == NULL)
        return 0;
    len += (size_t)snprintf(text + len, cap - len,
                            "# DS volumes changed from the built-in ones (percent; 100 = as the ROM has it),\n"
                            "# as set in Game Options > DS Volume Balance (or by hand: restart the game after).\n"
                            "# Music: on top of the Music volume. Sound effects: on top of the SFX volume.\n"
                            "# Delete a line (or this file) to get the built-in volume back.\n");
    for (int t = 0; t < sSongCount && len < cap; t++)
        if (sTracks[t].sdat == 0 && !sTracks[t].isStream && sTracks[t].gain != sTracks[t].defGain)
            len += (size_t)snprintf(text + len, cap - len, "MUSIC SEQ %03u = %u  # (built-in %u) %s\n",
                                    (unsigned)sTracks[t].index, (unsigned)sTracks[t].gain,
                                    (unsigned)sTracks[t].defGain, sTracks[t].label + 4);
    for (int t = sSongCount; t < sCount && len < cap; t++)
        if (sTracks[t].sdat == 0 && sTracks[t].isEffect && sTracks[t].gain != sTracks[t].defGain)
            len += (size_t)snprintf(text + len, cap - len, "SFX E%02u.%03u = %u  # (built-in %u)\n",
                                    (unsigned)sTracks[t].arc, (unsigned)sTracks[t].index, (unsigned)sTracks[t].gain,
                                    (unsigned)sTracks[t].defGain);
    if (len > cap)
        len = cap;
    int ok = Backend_FileWrite(VOL_FILE, text, (uint32_t)len);
    free(text);
    return ok;
}

static void LoadVolumes(void)
{
    for (int i = 0; i < sCount; i++)
        sTracks[i].gain = 100;
    for (size_t i = 0; i < sizeof(sDefaultMusicGain) / sizeof(sDefaultMusicGain[0]); i++) {
        int t = FindSeq(sDefaultMusicGain[i].seq);
        if (t >= 0)
            sTracks[t].gain = sDefaultMusicGain[i].percent;
    }
    for (size_t i = 0; i < sizeof(sDefaultGain) / sizeof(sDefaultGain[0]); i++) {
        int t = FindEffect(0, sDefaultGain[i].entry);
        if (t >= 0)
            sTracks[t].gain = sDefaultGain[i].percent;
    }
    for (int i = 0; i < sCount; i++)
        sTracks[i].defGain = sTracks[i].gain;
    void *f = Backend_FileOpen(VOL_FILE);
    if (f == NULL)
        return; // (none changed)
    uint32_t size = Backend_FileSize(f);
    char *text = malloc(size + 1);
    if (text != NULL && Backend_FileRead(f, 0, text, size)) {
        text[size] = '\0';
        for (char *line = text; line != NULL && *line; ) {
            char *next = strchr(line, '\n');
            if (next != NULL)
                *next++ = '\0';
            int a, b, pct, t = -1;
            if (sscanf(line, "MUSIC SEQ %d = %d", &a, &pct) == 2)
                t = FindSeq(a);
            else if (sscanf(line, "SFX E%d.%d = %d", &a, &b, &pct) == 3)
                t = FindEffect(a, b);
            if (t >= 0)
                sTracks[t].gain = (uint16_t)ClampGain(&sTracks[t], pct);
            line = next;
        }
        Backend_Log("music: %s loaded", VOL_FILE);
    }
    free(text);
    Backend_FileClose(f);
}

static void LoadSfxMap(void)
{
    LoadVolumes();
    for (int i = 0; i <= SFX_LAST - SFX_FIRST; i++)
        sSfxMap[i] = (int16_t)FindEffect(0, SFX_FIRST + i); // (the built-in matches)
    void *f = Backend_FileOpen(SFX_FILE);
    if (f == NULL)
        return;
    uint32_t size = Backend_FileSize(f);
    char *text = malloc(size + 1);
    if (text != NULL && Backend_FileRead(f, 0, text, size)) {
        text[size] = '\0';
        for (char *line = text; line != NULL && *line; ) {
            char *next = strchr(line, '\n');
            if (next != NULL)
                *next++ = '\0';
            int se, arc, idx;
            if (sscanf(line, "SE %d = E%d.%d", &se, &arc, &idx) == 3 && se >= SFX_FIRST && se <= SFX_LAST)
                sSfxMap[se - SFX_FIRST] = (int16_t)FindEffect(arc, idx);
            line = next;
        }
        Backend_Log("music: %s loaded", SFX_FILE);
    }
    free(text);
    Backend_FileClose(f);
}

static int TrackGain(int track)
{
    return sOpen && track >= 0 && track < sCount ? sTracks[track].gain : 100;
}

// A DS track's own volume (percent), and its built-in one
int Port_TrackGainGet(int track)
{
    return TrackGain(track);
}

int Port_TrackGainDefault(int track)
{
    return sOpen && track >= 0 && track < sCount ? sTracks[track].defGain : 100;
}

void Port_TrackGainSet(int track, int percent)
{
    if (sOpen && track >= 0 && track < sCount)
        sTracks[track].gain = (uint16_t)ClampGain(&sTracks[track], percent);
}

int Port_TrackGainMax(int track)
{
    return sOpen && track >= 0 && track < sCount && !sTracks[track].isEffect ? MUSIC_GAIN_MAX : SFX_GAIN_MAX;
}

// Writes ds-volumes.txt (the volumes changed from the built-in ones)
int Port_TrackGainsSave(void)
{
    return sOpen && SaveVolumes();
}

// A DS sound effect's own volume (percent; 100 for anything else)
int Port_SfxGainGet(int track)
{
    return sOpen && track >= 0 && track < sCount && sTracks[track].isEffect ? sTracks[track].gain : 100;
}

void Port_SfxGainSet(int track, int percent)
{
    if (sOpen && track >= 0 && track < sCount && sTracks[track].isEffect)
        sTracks[track].gain = (uint16_t)ClampGain(&sTracks[track], percent);
}

int Port_SfxMapGet(int se)
{
    return se >= SFX_FIRST && se <= SFX_LAST ? sSfxMap[se - SFX_FIRST] : -1;
}

static Song *CustomOpenTrack(int i);

// --- Music Style: DS -------------------------------------------------------------

static void InitSlots(void)
{
    if (sSlotsInit)
        return;
    sSlotsInit = 1;
    for (int i = 0; i < SLOTS; i++) {
        sSlots[i].track = sSlots[i].want = -1;
        sSlots[i].volume64 = 64;
    }
}

static void StopSlots(void)
{
    InitSlots();
    for (int i = 0; i < SLOTS; i++) {
        song_close(sSlots[i].song);
        sSlots[i].song = NULL;
        sSlots[i].track = sSlots[i].want = -1;
        sSlots[i].paused = 0;
    }
}

// What the game asked of the slots, done (no job running: from Port_MusicMix)
static void ApplySlots(void)
{
    for (int i = 0; i < SLOTS; i++) {
        Slot *s = &sSlots[i];
        int want = (s->want >= CUSTOM_TRACK ? sCustomOpen : s->want >= EXTRA_TRACK ? sExtraOpen : sOpen) ? s->want : -1;
        if (want != s->track || s->wantSerial != s->serial) {
            song_close(s->song);
            s->song = NULL;
            s->track = want;
            s->serial = s->wantSerial;
            s->ended = 0;
            if (want >= CUSTOM_TRACK) { // (the Music Box's Custom DS Music)
                s->song = CustomOpenTrack(want - CUSTOM_TRACK);
                if (s->song != NULL)
                    song_set_loops(s->song, 0, 0);
            }
            else if (want >= EXTRA_TRACK) { // (Extra Music: an Explorers of Sky song)
                s->song = song_open_dse(&sExtra.dse, want - EXTRA_TRACK, sMixRate);
                if (s->song != NULL)
                    song_set_loops(s->song, s->loop ? 0 : 1, 0);
            }
            else if (want >= 0 && want < sCount) {
                s->song = OpenTrack(&sTracks[want]);
                if (s->song != NULL)
                    song_set_loops(s->song, s->loop ? 0 : i >= SLOT_SFX ? 0 : 1, 0);
            }
            sFound.done[JOB_SLOT0 + i] = sJobFound.done[JOB_SLOT0 + i] = 0;
        }
        if (s->song != NULL && s->ending && !s->ended) {
            // (the GBA effect ended: a looping DS one ends at its next loop, briefly faded)
            song_set_loops(s->song, song_loops(s->song) + 1, 150);
            s->ended = 1;
        }
        else if (i >= SLOT_SFX && s->song != NULL && sFound.done[JOB_SLOT0 + i]) {
            song_close(s->song); // an effect that ended (until another starts)
            s->song = NULL;
        }
    }
}

// Slot 0 (background music) or 1 (fanfare): which track plays (-1: none),
// whether it loops for ever, is paused, and its volume (0-64, the GBA
// player's, which follows fades). A track that changes starts over.
void Port_MusicSlot(int slot, int track, int loop, int paused, int volume64)
{
    InitSlots();
    Slot *s = &sSlots[slot & 1];
    if (track != s->want)
        sFound.done[JOB_SLOT0 + (slot & 1)] = 0;
    s->want = track;
    s->loop = loop;
    s->paused = paused;
    s->volume64 = volume64 < 0 ? 0 : volume64 > 64 ? 64 : volume64; // (set when the next frame is made)
}

// Sound effect slot (0 to PORT_SFX_SLOTS - 1): its track (-1: none), played
// from the start each time serial changes, and looping (if it does) until
// ending (the GBA effect ended by itself): then it ends at its next loop;
// volume 0-64 (the GBA player's: its fades).
void Port_MusicSfxSlot(int slot, int track, unsigned serial, int ending, int volume64)
{
    InitSlots();
    if (slot < 0 || slot >= PORT_SFX_SLOTS)
        return;
    Slot *s = &sSlots[SLOT_SFX + slot];
    s->want = track;
    s->wantSerial = serial;
    s->ending = ending;
    s->loop = 0;
    s->paused = 0;
    s->volume64 = volume64 < 0 ? 0 : volume64 > 64 ? 64 : volume64;
}

int Port_MusicSlotDone(int slot)
{
    const Slot *s = &sSlots[slot & 1];
    return s->song == NULL || sFound.done[JOB_SLOT0 + (slot & 1)];
}

static void ApplyTiming(Song *g)
{
    if (sRepeat)
        song_set_loops(g, 0, 0); // forever
    else if (sPlaying >= 0 && sPlaying < sCount && sTracks[sPlaying].isEffect)
        song_set_loops(g, 1, 1500); // (looping effects: once round, a short fade)
    else
        song_set_loops(g, 2, 8000); // two loops, then an 8 s fade
}

int Port_MusicPlay(int i)
{
    if (i >= EXTRA_TRACK) { // (Extra Music's songs: heard in its volumes' list)
        if (!sExtraOpen || i - EXTRA_TRACK >= sExtra.dse.num)
            return 0;
        Port_MusicStop();
        Song *g = song_open_dse(&sExtra.dse, i - EXTRA_TRACK, sMixRate);
        sPlaying = i;
        if (g == NULL)
            return 0;
        song_set_loops(g, 0, 0); // (for ever, until stopped)
        song_set_volume(g, ExtraVolume(i));
        sSong = g;
        sFound.done[JOB_PLAYER] = sJobFound.done[JOB_PLAYER] = 0;
        sFound.posMs = sJobFound.posMs = 0;
        sFound.loops = sJobFound.loops = 0;
        sFound.voices = sJobFound.voices = 0;
        return 1;
    }
    if (!sOpen || i < 0 || i >= sCount)
        return 0;
    Port_MusicStop(); // (a stream reads the ROM file as it plays)
    Song *g = OpenTrack(&sTracks[i]);
    sPlaying = i;
    if (g == NULL)
        return 0;
    ApplyTiming(g);
    song_set_volume(g, (sTracks[i].isEffect ? sSfxVolume : sVolume) * sTracks[i].gain / 100);
    sSong = g;
    sFound.done[JOB_PLAYER] = sJobFound.done[JOB_PLAYER] = 0;
    sFound.posMs = sJobFound.posMs = 0;
    sFound.loops = sJobFound.loops = 0;
    sFound.voices = sJobFound.voices = 0;
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
    sVolume = percent; // (set when the next frame is made)
}

void Port_MusicSetSfxVolume(int percent)
{
    sSfxVolume = percent; // (set when the next frame is made)
}

void Port_MusicSetRepeat(int repeat)
{
    MusicSync();
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
    return sSong != NULL && sFound.done[JOB_PLAYER];
}

unsigned Port_MusicPosMs(void)
{
    return sSong != NULL ? sFound.posMs : 0;
}

int Port_MusicLoops(void)
{
    return sSong != NULL ? sFound.loops : 0;
}

unsigned Port_MusicVoices(void)
{
    return sSong != NULL ? sFound.voices : 0;
}

// One frame of the game's sound (count stereo samples at rate): with the ROM
// open, the frame goes out at twice the rate, with the DS songs (made at that
// rate, near the DS's own 32728 Hz) added: the samples made during the last
// frame (see above). Returns the samples now in out2 (0: send the game's
// frame as it is).
int Port_MusicMix(const int16_t *in, int count, int rate, int16_t *out2, int *outRate)
{
    static int16_t last[2];

    if (rate > 0)
        sMixRate = 2 * (uint32_t)rate;
    // (at twice the rate whenever the ROM is open, so that the rate doesn't
    // change, nor the sound with it, between one track and the next)
    if ((!sOpen && !sExtraOpen && !sCustomOpen) || count <= 0 || count > 1024)
        return 0;
    MusicSync(); // the samples made for this frame
    InitSlots();
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
    if (sJobCount == (uint32_t)count * 2) { // (else the first frame, or a new rate: nothing made yet)
        for (int k = 0; k < JOB_SONGS; k++) { // the player's track, then the slots'
            if (!sJobHas[k])
                continue;
            const int16_t *track = sJobOut[k];
            for (int i = 0; i < count * 4; i++) {
                int v = out2[i] + track[i];
                out2[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
            }
        }
    }
    // The next frame's: the volumes as they are now, then made on the second
    // core while the game runs (Dual Core), else now
    ApplySlots(); // (the tracks the game asked for since)
    if (sSong != NULL)
        song_set_volume(sSong, sPlaying >= EXTRA_TRACK ? ExtraVolume(sPlaying)
                               : sPlaying >= 0 && sPlaying < sCount
                                   ? (sTracks[sPlaying].isEffect ? sSfxVolume : sVolume) * sTracks[sPlaying].gain / 100
                                   : sVolume);
    int any = 0;
    sJobSong[JOB_PLAYER] = sPaused ? NULL : sSong;
    any |= sJobSong[JOB_PLAYER] != NULL;
    for (int k = 0; k < SLOTS; k++) {
        Slot *s = &sSlots[k];
        if (s->song != NULL)
            song_set_volume(s->song, k >= SLOT_SFX ? sSfxVolume * s->volume64 / 64 * Port_SfxGainGet(s->track) / 100
                                     : s->track >= CUSTOM_TRACK ? sVolume * s->volume64 / 64
                                     : s->track >= EXTRA_TRACK ? ExtraVolume(s->track) * s->volume64 / 64 // (Extra Volume: its own)
                                                   : sVolume * s->volume64 / 64 * TrackGain(s->track) / 100);
        sJobSong[JOB_SLOT0 + k] = s->paused ? NULL : s->song;
        any |= sJobSong[JOB_SLOT0 + k] != NULL;
    }
    sJobCount = (uint32_t)count * 2;
    if (!any) {
        memset(sJobHas, 0, sizeof(sJobHas)); // (nothing playing: no job)
    }
    else if (Port_DualCore()) {
        Backend_RunAsync(Port_MusicRenderJob, NULL);
        sJobPending = 1;
    }
    else {
        Port_MusicRenderJob(NULL);
        sFound = sJobFound;
    }
    *outRate = 2 * rate;
    return count * 2;
}

// --- Extra Music ---------------------------------------------------------------
//
// Music Options > Extra Music: On: on the dungeon floors given below (or in
// extra-music.txt), Explorers of Sky's songs play instead of the floor's own
// music, from its ROM (picked in Music Options, gPortExtraRom; that game's
// only: C2SE, C2SP, C2SJ). The game (port_extra_music.c) asks which song
// for the floor, then plays it in the background music's slot.

char gPortExtraRom[256];
const char *Port_ExtraMusicRomPath(void) { return gPortExtraRom; }
static void LoadExtraGains(void);
static char sExtraTried[256]; // (a ROM that wouldn't open: not tried again each frame)

static int IsSky(const NdsMusic *m)
{
    const char *c = m->hdr.gamecode;
    return c[0] == 'C' && c[1] == '2' && c[2] == 'S';
}

void Port_ExtraMusicClose(void)
{
    MusicSync();
    if (!sExtraOpen)
        return;
    for (int i = 0; i < SLOTS; i++) // (its songs read the ROM as they play)
        if (sSlots[i].track >= EXTRA_TRACK) {
            song_close(sSlots[i].song);
            sSlots[i].song = NULL;
            sSlots[i].track = -1;
        }
    if (sPlaying >= EXTRA_TRACK)
        Port_MusicStop();
    nds_music_close(&sExtra);
    sExtraOpen = 0;
    free(sExtraGain);
    sExtraGain = NULL;
}

// The ROM picked (gPortExtraRom) open: Explorers of Sky's, with its songs
int Port_ExtraMusicOpen(void)
{
    if (sExtraOpen)
        return 1;
    if (gPortExtraRom[0] == 0 || strcmp(sExtraTried, gPortExtraRom) == 0)
        return 0;
    snprintf(sExtraTried, sizeof(sExtraTried), "%s", gPortExtraRom);
    MusicSync();
    if (nds_music_open(&sExtra, gPortExtraRom) != NDSM_OK)
        return 0;
    if (!IsSky(&sExtra) || sExtra.dse.num == 0) {
        nds_music_close(&sExtra);
        return 0;
    }
    sExtraOpen = 1;
    sExtraTried[0] = 0;
    LoadExtraGains();
    InitSlots();
    return 1;
}

// Extra Music on: its ROM opened and its songs' shared sample bank (bgm.swd,
// megabytes) read now, at start or when the settings are saved, instead of
// at the first Extra Music song in a dungeon (options.c: on loading and saving
// the settings; `on`: Extra Music is on)
void Port_ExtraMusicPreload(int on)
{
    if (!on || !Port_ExtraMusicOpen())
        return;
    DseSet *s = &sExtra.dse;
    int g = DSE_GROUP_BGM;
    if (s->bank[g] != NULL || !s->has_bank[g])
        return;
    MusicSync();
    uint8_t *b = ds_load(&s->src, s->bank_off[g], s->bank_len[g]);
    if (b != NULL)
        s->bank[g] = dse_bank_open(b, s->bank_len[g]); // (takes it)
    Backend_Log("extra music: bank read (%u bytes)", (unsigned)s->bank_len[g]);
}

// extra-volumes.txt in the data folder: each song's own volume ("BGM 044 =
// 85"; 100 for one not there)
#define EXTRA_VOLUMES_FILE "extra-volumes.txt"
static void LoadExtraGains(void)
{
    free(sExtraGain);
    sExtraGain = malloc((size_t)sExtra.dse.num);
    if (sExtraGain == NULL)
        return;
    memset(sExtraGain, 100, (size_t)sExtra.dse.num);
    void *f = Backend_FileOpen(EXTRA_VOLUMES_FILE);
    if (f == NULL)
        return;
    uint32_t size = Backend_FileSize(f);
    char *text = size < 65536 ? malloc(size + 1) : NULL;
    if (text != NULL && Backend_FileRead(f, 0, text, size)) {
        text[size] = 0;
        for (char *line = text; line != NULL && *line; ) {
            char *next = strchr(line, '\n');
            if (next != NULL)
                *next++ = 0;
            int n, g;
            if (sscanf(line, " BGM %d = %d", &n, &g) == 2 && g >= 0 && g <= EXTRA_GAIN_MAX) {
                int idx = dse_set_find(&sExtra.dse, DSE_GROUP_BGM, (uint32_t)n);
                if (idx >= 0)
                    sExtraGain[idx] = (uint8_t)g;
            }
            line = next;
        }
    }
    free(text);
    Backend_FileClose(f);
}

// Extra Music's songs' own volumes (percent), by song number; Extra Volume
// (all of them: set by the game each frame, from Music Options)
int Port_ExtraGainGet(int n)
{
    int idx = sExtraOpen ? dse_set_find(&sExtra.dse, DSE_GROUP_BGM, (uint32_t)n) : -1;
    return idx >= 0 && sExtraGain != NULL ? sExtraGain[idx] : 100;
}

void Port_ExtraGainSet(int n, int percent)
{
    int idx = sExtraOpen ? dse_set_find(&sExtra.dse, DSE_GROUP_BGM, (uint32_t)n) : -1;
    if (idx >= 0 && sExtraGain != NULL)
        sExtraGain[idx] = (uint8_t)(percent < 0 ? 0 : percent > EXTRA_GAIN_MAX ? EXTRA_GAIN_MAX : percent);
}

int Port_ExtraGainMax(void) { return EXTRA_GAIN_MAX; }

void Port_ExtraMusicSetVolume(int percent)
{
    sExtraVolume = percent;
}

// Writes extra-volumes.txt (the songs not at 100%)
int Port_ExtraGainsSave(void)
{
    if (!sExtraOpen || sExtraGain == NULL)
        return 0;
    char *text = malloc(96 + (size_t)sExtra.dse.num * 32);
    if (text == NULL)
        return 0;
    int len = snprintf(text, 96, "; Extra Music: each Explorers of Sky song's own volume (percent)\n");
    for (int i = 0; i < sExtra.dse.num; i++)
        if (sExtraGain[i] != 100 && sExtra.dse.songs[i].group == DSE_GROUP_BGM)
            len += snprintf(text + len, 32, "BGM %03u = %u\n", (unsigned)sExtra.dse.songs[i].number, sExtraGain[i]);
    int ok = Backend_FileWrite(EXTRA_VOLUMES_FILE, text, (uint32_t)len);
    free(text);
    return ok;
}

// Music Options found a ROM for Extra Music (a path on the SD card): 1
// Explorers of Sky's, kept (config.ini) and open; 0 another game's (even
// Explorers of Time or Darkness): not taken; -1 not a ROM that opens
int Port_ExtraMusicTryRom(const char *path)
{
    static NdsMusic m; // (large: not on the stack)
    if (strlen(path) >= sizeof(gPortExtraRom))
        return -1;
    MusicSync();
    if (nds_music_open(&m, path) != NDSM_OK)
        return -1;
    int sky = IsSky(&m) && m.dse.num > 0;
    nds_music_close(&m);
    if (!sky)
        return 0;
    Port_ExtraMusicClose();
    snprintf(gPortExtraRom, sizeof(gPortExtraRom), "%s", path);
    sExtraTried[0] = 0;
    Port_SaveConfig();
    return Port_ExtraMusicOpen() ? 1 : -1;
}

// The slot track of Explorers of Sky's background music number n (its
// bgmNNNN.smd), or -1
int Port_ExtraMusicTrack(int n)
{
    if (!Port_ExtraMusicOpen())
        return -1;
    int idx = dse_set_find(&sExtra.dse, DSE_GROUP_BGM, (uint32_t)n);
    return idx >= 0 ? EXTRA_TRACK + idx : -1;
}

// Which floors play which song: dungeon (its number in the game: constants/
// dungeon.h), its floors first..last (as shown: 1F, B5F...), and Explorers of
// Sky's song number; or EXTRA_GBA | one of the game's own songs (constants/
// bg_music.h), played there in place of the floor's own (through its DS
// match with Music Style: DS), to keep a song on for longer than the game does.
typedef struct {
    uint8_t dungeon, first, last;
    uint16_t song;
} ExtraRule;
#define EXTRA_GBA 0x8000
#define ALL_FLOORS 0, 255
enum { // (constants/dungeon.h, constants/bg_music.h)
    D_STORMY_SEA = 19, D_SILVER_TRENCH = 20, D_METEOR_CAVE = 21, D_WESTERN_CAVE = 23, D_WISH_CAVE = 26,
    D_BURIED_RELIC = 27, D_PITFALL_VALLEY = 28, D_NORTHERN_RANGE = 29, D_DESERT_REGION = 31,
    D_SOUTHERN_CAVERN = 32, D_WYVERN_HILL = 33, D_FIERY_FIELD = 34, D_NORTHWIND_FIELD = 35, D_SOLAR_CAVE = 36,
    D_LIGHTNING_FIELD = 37, D_DARKNIGHT_RELIC = 38, D_MURKY_CAVE = 40, D_GRAND_SEA = 41, D_REMAINS_ISLAND = 44,
    D_MARVELOUS_SEA = 45, D_FANTASY_STRAIT = 46, D_HOWLING_FOREST = 53, D_WATERFALL_POND = 56,
    D_UNOWN_RELIC = 57, D_JOYOUS_TOWER = 58, D_FAR_OFF_SEA = 59, D_MT_FARAWAY = 60,
    MUS_A_NEW_ADVENTURE = 13, // (Buried Relic's own)
    MUS_STORMY_SEA = 21,
};
static const ExtraRule sExtraBuiltIn[] = {
    { D_HOWLING_FOREST, ALL_FLOORS, 149 },
    { D_FIERY_FIELD, ALL_FLOORS, 34 },
    { D_LIGHTNING_FIELD, ALL_FLOORS, 32 },
    { D_NORTHWIND_FIELD, ALL_FLOORS, 38 },
    { D_WATERFALL_POND, ALL_FLOORS, 24 },
    { D_MT_FARAWAY, 1, 14, 155 },
    { D_MT_FARAWAY, 15, 29, 157 },
    { D_MT_FARAWAY, 30, 39, 158 },
    { D_NORTHERN_RANGE, ALL_FLOORS, 145 },
    { D_PITFALL_VALLEY, ALL_FLOORS, 147 },
    { D_MURKY_CAVE, ALL_FLOORS, 39 },
    { D_BURIED_RELIC, 1, 35, EXTRA_GBA | MUS_A_NEW_ADVENTURE }, // (its own song, all the way)
    { D_BURIED_RELIC, 36, 70, 156 },
    { D_BURIED_RELIC, 71, 99, 146 },
    { D_DESERT_REGION, ALL_FLOORS, 57 },
    { D_STORMY_SEA, 1, 39, EXTRA_GBA | MUS_STORMY_SEA }, // (its own song, all the way)
    { D_SILVER_TRENCH, 1, 39, 46 },
    { D_SILVER_TRENCH, 40, 69, 54 },
    { D_SILVER_TRENCH, 70, 98, 55 },
    { D_WESTERN_CAVE, 1, 49, 31 },
    { D_WESTERN_CAVE, 50, 98, 30 },
    { D_METEOR_CAVE, 1, 10, 48 },
    { D_METEOR_CAVE, 11, 19, 49 },
    { D_WISH_CAVE, 40, 49, 153 },
    { D_WISH_CAVE, 50, 59, 154 },
    { D_WISH_CAVE, 60, 69, 151 },
    { D_WISH_CAVE, 70, 79, 144 },
    { D_WISH_CAVE, 80, 89, 50 },
    { D_WISH_CAVE, 90, 98, 51 },
    { D_GRAND_SEA, ALL_FLOORS, 60 },
    { D_FAR_OFF_SEA, ALL_FLOORS, 37 },
    { D_SOLAR_CAVE, 1, 10, 43 },
    { D_SOLAR_CAVE, 11, 20, 44 },
    { D_MARVELOUS_SEA, ALL_FLOORS, 45 },
    { D_FANTASY_STRAIT, ALL_FLOORS, 152 },
    { D_REMAINS_ISLAND, ALL_FLOORS, 52 },
    { D_DARKNIGHT_RELIC, ALL_FLOORS, 41 },
    { D_SOUTHERN_CAVERN, ALL_FLOORS, 42 },
    { D_UNOWN_RELIC, ALL_FLOORS, 61 },
    { D_WYVERN_HILL, ALL_FLOORS, 29 },
    { D_JOYOUS_TOWER, 1, 50, 148 },
    { D_JOYOUS_TOWER, 51, 99, 150 },
};
#define EXTRA_RULES_MAX 256
static ExtraRule sExtraRules[EXTRA_RULES_MAX];
static int sExtraRuleCount = -1; // -1: not read yet

// extra-music.txt in the data folder, if it's there, in place of the
// built-in ones: "<dungeon> <first>-<last> = <song>" (or one floor:
// "<dungeon> <floor> = <song>"), all numbers; "= gba <song>" for one of the
// game's own songs; ; or # starts a comment
static void LoadExtraRules(void)
{
    sExtraRuleCount = 0;
    void *f = Backend_FileOpen("extra-music.txt");
    if (f == NULL) {
        for (unsigned i = 0; i < sizeof(sExtraBuiltIn) / sizeof(sExtraBuiltIn[0]); i++)
            if (sExtraBuiltIn[i].dungeon != 0xFF && sExtraRuleCount < EXTRA_RULES_MAX)
                sExtraRules[sExtraRuleCount++] = sExtraBuiltIn[i];
        return;
    }
    uint32_t size = Backend_FileSize(f);
    char *text = size < 65536 ? malloc(size + 1) : NULL;
    if (text != NULL && Backend_FileRead(f, 0, text, size)) {
        text[size] = 0;
        for (char *line = text; line != NULL && *line; ) {
            char *next = strchr(line, '\n');
            if (next != NULL)
                *next++ = 0;
            for (char *c = line; *c; c++)
                if (*c == ';' || *c == '#') { *c = 0; break; }
            int gba = 0;
            for (char *c = line; *c; c++) // "gba <song>": the game's own song
                if ((c[0] == 'g' || c[0] == 'G') && (c[1] == 'b' || c[1] == 'B') && (c[2] == 'a' || c[2] == 'A')) {
                    gba = 1;
                    c[0] = c[1] = c[2] = ' ';
                    break;
                }
            int d, a, b, song;
            if (sExtraRuleCount < EXTRA_RULES_MAX
                && (sscanf(line, " %d %d - %d = %d", &d, &a, &b, &song) == 4
                    || (sscanf(line, " %d %d = %d", &d, &a, &song) == 3 && (b = a, 1)))
                && d >= 0 && d < 255 && a >= 0 && b >= a && b < 256 && song >= 0 && song < EXTRA_GBA) {
                ExtraRule r = { (uint8_t)d, (uint8_t)a, (uint8_t)b, (uint16_t)(song | (gba ? EXTRA_GBA : 0)) };
                sExtraRules[sExtraRuleCount++] = r;
            }
            line = next;
        }
    }
    free(text);
    Backend_FileClose(f);
}

// The song for this dungeon's floor: Explorers of Sky's song number, or
// 0x8000 | one of the game's own songs (constants/bg_music.h), or -1 (the
// floor's own music)
int Port_ExtraMusicFor(int dungeon, int floor)
{
    if (sExtraRuleCount < 0)
        LoadExtraRules();
    for (int i = 0; i < sExtraRuleCount; i++)
        if (sExtraRules[i].dungeon == dungeon && floor >= sExtraRules[i].first && floor <= sExtraRules[i].last)
            return sExtraRules[i].song;
    return -1;
}

// The dungeons' names (the ones the built-in floors name), for the songs' list
static const struct { uint8_t id; const char *name; } sExtraDungeonNames[] = {
    { D_STORMY_SEA, "Stormy Sea" }, { D_SILVER_TRENCH, "Silver Trench" }, { D_METEOR_CAVE, "Meteor Cave" },
    { D_WESTERN_CAVE, "Western Cave" }, { D_WISH_CAVE, "Wish Cave" }, { D_BURIED_RELIC, "Buried Relic" },
    { D_PITFALL_VALLEY, "Pitfall Valley" }, { D_NORTHERN_RANGE, "Northern Range" }, { D_DESERT_REGION, "Desert Region" },
    { D_SOUTHERN_CAVERN, "Southern Cavern" }, { D_WYVERN_HILL, "Wyvern Hill" }, { D_FIERY_FIELD, "Fiery Field" },
    { D_NORTHWIND_FIELD, "Northwind Field" }, { D_SOLAR_CAVE, "Solar Cave" }, { D_LIGHTNING_FIELD, "Lightning Field" },
    { D_DARKNIGHT_RELIC, "Darknight Relic" }, { D_MURKY_CAVE, "Murky Cave" }, { D_GRAND_SEA, "Grand Sea" },
    { D_REMAINS_ISLAND, "Remains Island" }, { D_MARVELOUS_SEA, "Marvelous Sea" }, { D_FANTASY_STRAIT, "Fantasy Strait" },
    { D_HOWLING_FOREST, "Howling Forest" }, { D_WATERFALL_POND, "Waterfall Pond" }, { D_UNOWN_RELIC, "Unown Relic" },
    { D_JOYOUS_TOWER, "Joyous Tower" }, { D_FAR_OFF_SEA, "Far-off Sea" }, { D_MT_FARAWAY, "Mt. Faraway" },
};

// The Explorers of Sky songs the floors name (each once, by number): up to max
int Port_ExtraMusicSongs(int *out, int max)
{
    int n = 0;
    if (sExtraRuleCount < 0)
        LoadExtraRules();
    for (int i = 0; i < sExtraRuleCount; i++) {
        int song = sExtraRules[i].song, k;
        if (song & EXTRA_GBA)
            continue;
        for (k = 0; k < n && out[k] != song; k++)
            ;
        if (k == n && n < max)
            out[n++] = song;
    }
    for (int i = 1; i < n; i++) // (by number)
        for (int k = i; k > 0 && out[k - 1] > out[k]; k--) {
            int t = out[k]; out[k] = out[k - 1]; out[k - 1] = t;
        }
    return n;
}

// Where song n plays: "Solar Cave 11F-20F" (the first floors naming it; "+" if more)
void Port_ExtraMusicSongWhere(int song, char *out, int size)
{
    int found = 0;
    out[0] = 0;
    if (sExtraRuleCount < 0)
        LoadExtraRules();
    for (int i = 0; i < sExtraRuleCount; i++) {
        const ExtraRule *r = &sExtraRules[i];
        if (r->song != song)
            continue;
        if (found++) {
            snprintf(out + strlen(out), size - strlen(out), " +");
            break;
        }
        const char *name = NULL;
        for (unsigned k = 0; k < sizeof(sExtraDungeonNames) / sizeof(sExtraDungeonNames[0]); k++)
            if (sExtraDungeonNames[k].id == r->dungeon)
                name = sExtraDungeonNames[k].name;
        char d[24];
        if (name == NULL)
            snprintf(d, sizeof(d), "Dungeon %u", r->dungeon);
        if (r->first == 0 && r->last == 255)
            snprintf(out, size, "%s", name != NULL ? name : d);
        else if (r->first == r->last)
            snprintf(out, size, "%s %uF", name != NULL ? name : d, r->first);
        else
            snprintf(out, size, "%s %uF-%uF", name != NULL ? name : d, r->first, r->last);
    }
}

// --- The Music Box (in dungeons) ----------------------------------------------------
//
// Using it, the player picks a song (port_music_box.c): from Blue Rescue
// Team's ROM (the one Music Options plays: the music's tracks, by index), from
// Explorers of Sky's (Extra Music's: its songs, by index), or from another
// game's ROM picked on the SD card (Custom DS Music: its sequences and
// streams, or songs on the DSE engine). The game plays the song's slot track
// in place of the floor's music, as Extra Music does.

// Blue Rescue Team's ROM open (the one Music Options plays)
int Port_MusicBoxBlueReady(void)
{
    return Port_MusicOpen() > 0 && Port_MusicRomIsBlue() && sSongCount > 0;
}

int Port_MusicBoxBlueCount(void) { return sOpen ? sSongCount : 0; }
const char *Port_MusicBoxBlueLabel(int i) { return i >= 0 && i < sSongCount ? sTracks[i].label : ""; }
int Port_MusicBoxBlueTrack(int i) { return i >= 0 && i < sSongCount ? i : -1; }

// Explorers of Sky's ROM (Extra Music's) open, with its songs
int Port_MusicBoxSkyReady(void)
{
    return Port_ExtraMusicOpen() && sExtra.dse.num > 0;
}

int Port_MusicBoxSkyCount(void) { return sExtraOpen ? sExtra.dse.num : 0; }

const char *Port_MusicBoxSkyLabel(int i)
{
    static char label[LABEL_LEN];
    if (!sExtraOpen || i < 0 || i >= sExtra.dse.num)
        return "";
    const DseSong *d = &sExtra.dse.songs[i];
    if (d->group == DSE_GROUP_ME)
        snprintf(label, sizeof(label), "ME%02u %s", (unsigned)d->number, d->label);
    else
        snprintf(label, sizeof(label), "%03u %s", (unsigned)d->number, d->label);
    return label;
}

int Port_MusicBoxSkyTrack(int i) { return sExtraOpen && i >= 0 && i < sExtra.dse.num ? EXTRA_TRACK + i : -1; }

// Custom DS Music: the ROM's music, as the NDS Music Player lists it
// (sequences, then streams; songs on the DSE engine after)
typedef struct {
    uint8_t sdat, kind; // kind: 0 sequence, 1 stream, 2 DSE song
    uint16_t index;
    char label[LABEL_LEN];
} CustomTrack;
static CustomTrack *sCustomTracks;
static int sCustomCount;
static char sCustomPath[256];

static Song *CustomOpenTrack(int i)
{
    if (!sCustomOpen || i < 0 || i >= sCustomCount)
        return NULL;
    const CustomTrack *t = &sCustomTracks[i];
    if (t->kind == 2)
        return song_open_dse(&sCustom.dse, t->index, sMixRate);
    const Sdat *s = &sCustom.sdat[t->sdat];
    return t->kind == 1 ? song_open_strm(s, t->index, sMixRate) : song_open_seq(s, t->index, sMixRate);
}

static void CustomClose(void)
{
    MusicSync();
    for (int i = 0; i < SLOTS; i++) // (its songs read the ROM as they play)
        if (sSlots[i].track >= CUSTOM_TRACK) {
            song_close(sSlots[i].song);
            sSlots[i].song = NULL;
            sSlots[i].track = -1;
        }
    if (sCustomOpen)
        nds_music_close(&sCustom);
    sCustomOpen = 0;
    free(sCustomTracks);
    sCustomTracks = NULL;
    sCustomCount = 0;
    sCustomPath[0] = 0;
}

// A ROM picked on the SD card (path "/..."): its music's count (and it open,
// in place of the one before), or -1 (not a ROM that opens, or no music: the
// one before stays)
int Port_MusicBoxCustomTryRom(const char *path)
{
    static NdsMusic m; // (large: not on the stack)
    int total = 0;

    if (sCustomOpen && strcmp(path, sCustomPath) == 0)
        return sCustomCount;
    MusicSync();
    if (nds_music_open(&m, path) != NDSM_OK)
        return -1;
    for (int i = 0; i < m.num_sdats; i++)
        total += (int)(m.sdat[i].num_seq + m.sdat[i].num_strm);
    total += m.dse.num;
    CustomTrack *list = total > 0 ? calloc((size_t)total, sizeof(CustomTrack)) : NULL;
    if (list == NULL) {
        nds_music_close(&m);
        return -1;
    }
    int n = 0;
    for (int i = 0; i < m.num_sdats; i++) {
        const Sdat *s = &m.sdat[i];
        char pre[8] = "";
        if (m.num_sdats > 1)
            snprintf(pre, sizeof(pre), "%d:", i + 1);
        for (uint32_t q = 0; q < s->num_seq && n < total; q++) {
            if (!s->seq[q].present)
                continue;
            const char *nm = sdat_seq_name(s, q);
            CustomTrack *t = &list[n++];
            t->sdat = (uint8_t)i;
            t->index = (uint16_t)q;
            snprintf(t->label, sizeof(t->label), "%s%03u %s", pre, (unsigned)q, nm ? nm : "(sequence)");
        }
        for (uint32_t q = 0; q < s->num_strm && n < total; q++) {
            if (!s->strm[q].present)
                continue;
            const char *nm = sdat_strm_name(s, q);
            CustomTrack *t = &list[n++];
            t->sdat = (uint8_t)i;
            t->kind = 1;
            t->index = (uint16_t)q;
            snprintf(t->label, sizeof(t->label), "%sS%02u %s", pre, (unsigned)q, nm ? nm : "(stream)");
        }
    }
    for (int i = 0; i < m.dse.num && n < total; i++) {
        const DseSong *d = &m.dse.songs[i];
        CustomTrack *t = &list[n++];
        t->kind = 2;
        t->index = (uint16_t)i;
        if (d->group == DSE_GROUP_ME)
            snprintf(t->label, sizeof(t->label), "ME%02u %s", (unsigned)d->number, d->label);
        else
            snprintf(t->label, sizeof(t->label), "%03u %s", (unsigned)d->number, d->label);
    }
    if (n == 0) {
        free(list);
        nds_music_close(&m);
        return -1;
    }
    CustomClose();
    sCustom = m;
    sCustomOpen = 1;
    sCustomTracks = list;
    sCustomCount = n;
    snprintf(sCustomPath, sizeof(sCustomPath), "%s", path);
    InitSlots();
    return n;
}

const char *Port_MusicBoxCustomPath(void) { return sCustomPath; }
int Port_MusicBoxCustomCount(void) { return sCustomOpen ? sCustomCount : 0; }
const char *Port_MusicBoxCustomLabel(int i) { return sCustomOpen && i >= 0 && i < sCustomCount ? sCustomTracks[i].label : ""; }
int Port_MusicBoxCustomTrack(int i) { return sCustomOpen && i >= 0 && i < sCustomCount ? CUSTOM_TRACK + i : -1; }
