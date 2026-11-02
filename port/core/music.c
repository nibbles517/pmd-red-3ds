// Music Options: the DS version's music (Pokémon Mystery Dungeon: Blue Rescue
// Team), played from pmd-blue.nds in the port's data folder by the NDS Music
// Player's core (port/ndsmusic).
//
// The game's menu (options_menu1.c) lists the ROM's tracks and drives
// playback through the calls below. The track plays together with the game's
// own sound: Sound_EndFrame (sound.c) hands each frame's samples to
// Port_MusicMix, which adds the track's.

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
static uint32_t sMixRate = 2 * 18157; // twice the game's mixing rate (see Port_MusicMix)

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

// The ROM's DS Menu icon (its first frame), in the middle of a 40x40
// picture as the game's portraits are: 5x5 tiles of 4bpp, row by row (800
// bytes), and its 16 colours as r, g, b, 0 (64 bytes). 0 if it has none.
int Port_MusicIcon(uint8_t *tiles, uint8_t *colors)
{
    const NdsBanner *b = &sRom.banner;
    if (!sOpen || !b->valid)
        return 0;
    memset(tiles, 0, 25 * 32);
    for (int y = 0; y < BANNER_ICON_W; y++)
        for (int x = 0; x < BANNER_ICON_W; x++) {
            int px = x + 4, py = y + 4; // (centred)
            int bmp = b->frame[0].bitmap; // (an animated icon: its first frame)
            int c = b->pixels[bmp][y * BANNER_ICON_W + x] & 15;
            uint8_t *t = tiles + ((py / 8) * 5 + px / 8) * 32 + (py % 8) * 4 + (px % 8) / 2;
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
    if (sSong == NULL || count <= 0 || count > 1024)
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
    if (!sPaused) {
        song_render(sSong, track, (uint32_t)count * 2);
        for (int i = 0; i < count * 4; i++) {
            int v = out2[i] + track[i];
            out2[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
    }
    *outRate = 2 * rate;
    return count * 2;
}
