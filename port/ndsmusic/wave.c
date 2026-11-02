/*
 * From the NDS Music Player (ndsmusicplayer, source/core), for the port's Music
 * Options. Files are read through the backend (Backend_File*) instead of stdio.
 */
#include "wave.h"

#include <stdlib.h>
#include <string.h>

#include "datasource.h"

static const int16_t ima_step[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
    544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
    9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

static const int8_t ima_index[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

void adpcm_init(AdpcmState *st, const uint8_t hdr[4]) {
    st->pred = (int16_t)rd16(hdr);
    st->index = hdr[2];
    if (st->index > 88) st->index = 88;
}

static inline int16_t adpcm_nibble(AdpcmState *st, unsigned n) {
    int32_t step = ima_step[st->index];
    int32_t diff = step >> 3;
    if (n & 1) diff += step >> 2;
    if (n & 2) diff += step >> 1;
    if (n & 4) diff += step;
    if (n & 8) {
        st->pred -= diff;
        if (st->pred < -0x7FFF) st->pred = -0x7FFF; /* the DS clamps to -0x7FFF, not -0x8000 */
    } else {
        st->pred += diff;
        if (st->pred > 0x7FFF) st->pred = 0x7FFF;
    }
    st->index += ima_index[n & 7];
    if (st->index < 0) st->index = 0;
    if (st->index > 88) st->index = 88;
    return (int16_t)st->pred;
}

void adpcm_decode(AdpcmState *st, const uint8_t *src, uint32_t nbytes, int16_t *dst) {
    for (uint32_t i = 0; i < nbytes; i++) {
        *dst++ = adpcm_nibble(st, src[i] & 0x0F);
        *dst++ = adpcm_nibble(st, src[i] >> 4);
    }
}

/* SWAR: "SWAR" header (0x10), "DATA" block: magic, size, 32 reserved, u32 count @0x38,
 * then count u32 absolute offsets to SWAV bodies (no per-SWAV file header). */
uint32_t swar_count(const uint8_t *swar, uint32_t size) {
    if (!swar || size < 0x3C || memcmp(swar, "SWAR", 4) != 0) return 0;
    uint32_t n = rd32(swar + 0x38);
    if (n > (size - 0x3C) / 4) return 0;
    return n;
}

void wave_free(Wave *w) {
    free(w->data);
    memset(w, 0, sizeof *w);
}

int swar_decode(const uint8_t *swar, uint32_t size, uint32_t idx, Wave *out) {
    memset(out, 0, sizeof *out);
    uint32_t n = swar_count(swar, size);
    if (idx >= n) return -1;

    uint32_t off = rd32(swar + 0x3C + idx * 4);
    if (off > size || size - off < 12) return -1;
    const uint8_t *h = swar + off;

    /* SWAV info: u8 type, u8 loop, u16 rate, u16 timer, u16 loop_off (words), u32 len (words) */
    uint8_t type = h[0];
    uint8_t loop = h[1];
    uint16_t rate = rd16(h + 2);
    uint16_t timer = rd16(h + 4);
    uint32_t loop_words = rd16(h + 6);
    uint32_t len_words = rd32(h + 8);

    uint32_t avail = size - off - 12;
    uint64_t bytes = ((uint64_t)loop_words + len_words) * 4;
    if (bytes > avail) bytes = avail & ~3u; /* truncated file: play what's there */
    const uint8_t *src = h + 12;
    uint32_t nbytes = (uint32_t)bytes;

    uint32_t samples, loop_start;
    switch (type) {
    case WAVE_PCM8:
        samples = nbytes;
        loop_start = loop_words * 4;
        break;
    case WAVE_PCM16:
        samples = nbytes / 2;
        loop_start = loop_words * 2;
        break;
    case WAVE_ADPCM:
        if (nbytes < 4) return -1;
        samples = (nbytes - 4) * 2;
        loop_start = loop_words ? (loop_words * 4 - 4) * 2 : 0; /* loop offset includes the 4-byte header */
        break;
    default:
        return -1;
    }
    if (samples == 0) return -1;

    out->data = (int16_t *)malloc(((size_t)samples + 1) * sizeof(int16_t));
    if (!out->data) return -1;

    switch (type) {
    case WAVE_PCM8:
        for (uint32_t i = 0; i < samples; i++) out->data[i] = (int16_t)((int8_t)src[i] * 256);
        break;
    case WAVE_PCM16:
        for (uint32_t i = 0; i < samples; i++) out->data[i] = (int16_t)rd16(src + i * 2);
        break;
    case WAVE_ADPCM: {
        AdpcmState st;
        adpcm_init(&st, src);
        adpcm_decode(&st, src + 4, nbytes - 4, out->data);
        break;
    }
    }

    out->len = samples;
    out->loops = loop && loop_start < samples;
    out->loop_start = out->loops ? loop_start : 0;
    /* Guard sample so the mixer can always read data[i+1]. */
    out->data[samples] = out->loops ? out->data[out->loop_start] : 0;
    out->rate = timer ? (float)DS_TIMER_CLOCK / (float)timer : (float)rate;
    if (out->rate <= 0.0f) out->rate = 32000.0f;
    return 0;
}
