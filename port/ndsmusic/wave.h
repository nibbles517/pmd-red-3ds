/*
 * From the NDS Music Player (ndsmusicplayer, source/core), for the port's Music
 * Options. Files are read through the backend (Backend_File*) instead of stdio.
 */
/*
 * wave.h - DS sample formats (PCM8 / PCM16 / IMA-ADPCM) and SWAR/SWAV access.
 * Samples are decoded to signed 16-bit once, at song load, so the mixer only
 * ever deals with one format and ADPCM loops need no saved decoder state.
 */
#ifndef NDSMUSIC_WAVE_H
#define NDSMUSIC_WAVE_H

#include <stdint.h>

/* ARM7 bus clock / 2: the DS sound channel timers tick at this rate. */
#define DS_TIMER_CLOCK 16756991u

enum { WAVE_PCM8 = 0, WAVE_PCM16 = 1, WAVE_ADPCM = 2 };

typedef struct {
    int16_t *data;       /* malloc'd, len samples (+1 guard sample for interpolation) */
    uint32_t len;        /* total samples */
    uint32_t loop_start; /* first sample of the loop (valid if loops) */
    uint8_t loops;
    float rate;          /* native sample rate in Hz */
} Wave;

/* Streaming IMA-ADPCM decoder state (used by STRM). */
typedef struct {
    int32_t pred;
    int32_t index;
} AdpcmState;

void adpcm_init(AdpcmState *st, const uint8_t hdr[4]);
/* Decodes `nbytes` bytes (2 samples each, low nibble first). */
void adpcm_decode(AdpcmState *st, const uint8_t *src, uint32_t nbytes, int16_t *dst);

/* Number of SWAVs in a SWAR (0 if invalid). */
uint32_t swar_count(const uint8_t *swar, uint32_t size);
/* Decodes SWAV #idx of a SWAR. Returns 0 on success. */
int swar_decode(const uint8_t *swar, uint32_t size, uint32_t idx, Wave *out);
void wave_free(Wave *w);

#endif
