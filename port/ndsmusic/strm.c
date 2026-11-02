/*
 * From the NDS Music Player (ndsmusicplayer, source/core), for the port's Music
 * Options. Files are read through the backend (Backend_File*) instead of stdio.
 */
#include "strm.h"

#include <stdlib.h>
#include <string.h>

#include "mixutil.h"
#include "wave.h"

struct StrmPlayer {
    DataSource src;
    uint8_t type, loop, nch;
    float rate;
    uint32_t loop_off, total, data_off;
    uint32_t nblocks, blen, bsamples, last_blen, last_bsamples;

    uint8_t *raw;     /* one channel's block */
    int16_t *buf[2];  /* decoded block per channel */
    uint32_t block, idx, block_samples;
    uint32_t abs_pos; /* source sample index of buf[..][idx] */

    int32_t s0[2], s1[2];
    uint64_t frac, step; /* 32.32 */
    int ended, loops;
    int32_t gain;
};

static int decode_block(StrmPlayer *s, uint32_t b) {
    if (b >= s->nblocks) return -1;
    int last = b == s->nblocks - 1;
    uint32_t ns = last ? s->last_bsamples : s->bsamples;
    uint32_t bl = last ? s->last_blen : s->blen;
    if (ns > s->bsamples || bl > s->blen) return -1;

    for (uint32_t c = 0; c < s->nch; c++) {
        uint64_t addr = (uint64_t)s->data_off + (uint64_t)b * s->blen * s->nch + (uint64_t)c * bl;
        if (addr > 0xFFFFFFFFu || ds_read(&s->src, (uint32_t)addr, s->raw, bl) != 0) return -1;
        int16_t *dst = s->buf[c];
        switch (s->type) {
        case WAVE_PCM8:
            if (ns > bl) ns = bl;
            for (uint32_t i = 0; i < ns; i++) dst[i] = (int16_t)((int8_t)s->raw[i] * 256);
            break;
        case WAVE_PCM16:
            if (ns > bl / 2) ns = bl / 2;
            for (uint32_t i = 0; i < ns; i++) dst[i] = (int16_t)rd16(s->raw + i * 2);
            break;
        default: {
            if (bl < 4) return -1;
            if (ns > (bl - 4) * 2) ns = (bl - 4) * 2;
            AdpcmState st;
            adpcm_init(&st, s->raw);
            adpcm_decode(&st, s->raw + 4, (ns + 1) / 2, dst);
            break;
        }
        }
    }
    s->block = b;
    s->block_samples = ns;
    s->idx = 0;
    s->abs_pos = b * s->bsamples;
    return 0;
}

static int seek(StrmPlayer *s, uint32_t sample) {
    if (decode_block(s, sample / s->bsamples) != 0) return -1;
    s->idx = sample % s->bsamples;
    s->abs_pos = sample;
    return 0;
}

static void next_frame(StrmPlayer *s, int32_t out[2]) {
    if (!s->ended && (s->abs_pos >= s->total || s->idx >= s->block_samples)) {
        int ok;
        if (s->abs_pos >= s->total) {
            ok = s->loop && s->loop_off < s->total && seek(s, s->loop_off) == 0;
            if (ok) s->loops++;
        } else {
            ok = decode_block(s, s->block + 1) == 0 && s->block_samples > 0;
        }
        if (!ok) s->ended = 1;
    }
    if (s->ended) {
        out[0] = out[1] = 0;
        return;
    }
    out[0] = s->buf[0][s->idx];
    out[1] = s->buf[s->nch > 1 ? 1 : 0][s->idx];
    s->idx++;
    s->abs_pos++;
}

StrmPlayer *strm_create(const DataSource *src, uint32_t out_rate) {
    uint8_t h[0x40];
    if (ds_read(src, 0, h, sizeof h) != 0 || memcmp(h, "STRM", 4) != 0 || memcmp(h + 0x10, "HEAD", 4) != 0)
        return NULL;
    const uint8_t *hd = h + 0x10;

    StrmPlayer *s = (StrmPlayer *)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->src = *src;
    s->type = hd[0x08];
    s->loop = hd[0x09];
    s->nch = hd[0x0A];
    uint16_t rate = rd16(hd + 0x0C), timer = rd16(hd + 0x0E);
    s->loop_off = rd32(hd + 0x10);
    s->total = rd32(hd + 0x14);
    s->data_off = rd32(hd + 0x18);
    s->nblocks = rd32(hd + 0x1C);
    s->blen = rd32(hd + 0x20);
    s->bsamples = rd32(hd + 0x24);
    s->last_blen = rd32(hd + 0x28);
    s->last_bsamples = rd32(hd + 0x2C);
    s->rate = timer ? (float)DS_TIMER_CLOCK / timer : (float)rate;

    if (s->type > WAVE_ADPCM || s->nch < 1 || s->nch > 2 || s->nblocks == 0 || s->bsamples == 0 ||
        s->blen == 0 || s->blen > 0x100000 || s->bsamples > 0x200000 || s->rate <= 0.0f)
        goto fail;

    s->raw = (uint8_t *)malloc(s->blen);
    for (int c = 0; c < s->nch; c++) s->buf[c] = (int16_t *)malloc(s->bsamples * sizeof(int16_t));
    if (!s->raw || !s->buf[0] || (s->nch > 1 && !s->buf[1])) goto fail;
    if (decode_block(s, 0) != 0) goto fail;

    s->gain = GAIN_UNITY;
    s->step = (uint64_t)((double)s->rate / out_rate * 4294967296.0);
    next_frame(s, s->s0);
    next_frame(s, s->s1);
    return s;

fail:
    strm_destroy(s);
    return NULL;
}

void strm_destroy(StrmPlayer *s) {
    if (!s) return;
    free(s->raw);
    free(s->buf[0]);
    free(s->buf[1]);
    free(s);
}

void strm_render(StrmPlayer *s, int16_t *out, uint32_t frames) {
    for (uint32_t i = 0; i < frames; i++) {
        int32_t f = (int32_t)(s->frac >> 17); /* 15-bit so full-scale jumps can't overflow */
        for (int c = 0; c < 2; c++) {
            int32_t v = s->s0[c] + (((s->s1[c] - s->s0[c]) * f) >> 15);
            out[i * 2 + c] = soft_clip((v * s->gain) >> 8);
        }
        s->frac += s->step;
        while (s->frac >= (1ull << 32)) {
            s->frac -= 1ull << 32;
            s->s0[0] = s->s1[0];
            s->s0[1] = s->s1[1];
            next_frame(s, s->s1);
        }
    }
}

int strm_finished(const StrmPlayer *s) { return s->ended && s->s0[0] == 0 && s->s0[1] == 0; }
int strm_loops(const StrmPlayer *s) { return s->loops; }

void strm_set_gain(StrmPlayer *s, int32_t gain) { s->gain = gain < 0 ? 0 : gain > GAIN_MAX ? GAIN_MAX : gain; }
