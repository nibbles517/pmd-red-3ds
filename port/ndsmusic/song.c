/*
 * From the NDS Music Player (ndsmusicplayer, source/core), for the port's Music
 * Options. Files are read through the backend (Backend_File*) instead of stdio.
 */
#include "song.h"

#include <stdlib.h>
#include <string.h>

#include "dse.h"
#include "mixutil.h"
#include "sseq.h"
#include "strm.h"

#define DEFAULT_LOOPS 2
#define DEFAULT_FADE_MS 8000
#define MAX_SONG_MS (10u * 60u * 1000u) /* songs that never end or loop get faded here */

struct Song {
    SdatSeqBundle bundle;
    SseqPlayer *seq;
    StrmPlayer *strm;
    DsePlayer *dse;
    uint32_t rate;
    uint64_t pos; /* samples rendered */

    uint64_t length;      /* fixed length in samples, 0 = loop-based */
    int loops_wanted;
    uint64_t fade_len;
    uint64_t fade_start;  /* UINT64_MAX until the fade begins */
    int done;
};

static Song *song_new(uint32_t rate) {
    Song *g = (Song *)calloc(1, sizeof *g);
    if (!g) return NULL;
    g->rate = rate;
    g->loops_wanted = DEFAULT_LOOPS;
    g->fade_len = (uint64_t)DEFAULT_FADE_MS * rate / 1000;
    g->fade_start = UINT64_MAX;
    return g;
}

Song *song_open_seq(const Sdat *s, uint32_t idx, uint32_t rate) {
    Song *g = song_new(rate);
    if (!g) return NULL;
    if (sdat_load_seq(s, idx, &g->bundle) != 0 || !(g->seq = sseq_create(&g->bundle, rate))) {
        song_close(g);
        return NULL;
    }
    return g;
}

Song *song_open_ssar(const Sdat *s, uint32_t arc, uint32_t idx, uint32_t rate) {
    Song *g = song_new(rate);
    if (!g) return NULL;
    if (sdat_load_ssar_seq(s, arc, idx, &g->bundle) != 0 || !(g->seq = sseq_create(&g->bundle, rate))) {
        song_close(g);
        return NULL;
    }
    return g;
}

Song *song_open_dse(DseSet *set, int idx, uint32_t rate) {
    Song *g = song_new(rate);
    if (!g) return NULL;
    if (!(g->dse = dse_set_open(set, idx, rate))) {
        song_close(g);
        return NULL;
    }
    return g;
}

Song *song_open_strm(const Sdat *s, uint32_t idx, uint32_t rate) {
    DataSource src = sdat_strm_source(s, idx);
    if (src.size == 0) return NULL;
    Song *g = song_new(rate);
    if (!g) return NULL;
    if (!(g->strm = strm_create(&src, rate))) {
        song_close(g);
        return NULL;
    }
    return g;
}

void song_close(Song *g) {
    if (!g) return;
    sseq_destroy(g->seq);
    strm_destroy(g->strm);
    dse_destroy(g->dse);
    sdat_free_seq(&g->bundle);
    free(g);
}

void song_set_length(Song *g, uint32_t length_ms, uint32_t fade_ms) {
    g->length = (uint64_t)length_ms * g->rate / 1000;
    g->fade_len = (uint64_t)fade_ms * g->rate / 1000;
}

void song_set_loops(Song *g, int loops, uint32_t fade_ms) {
    g->length = 0;
    g->loops_wanted = loops < 0 ? 0 : loops;
    g->fade_len = (uint64_t)fade_ms * g->rate / 1000;
    if (!g->done) g->fade_start = UINT64_MAX; /* e.g. repeat switched on mid-fade */
}

int song_loops(const Song *g) { return g->seq ? sseq_loops(g->seq) : g->dse ? dse_loops(g->dse) : strm_loops(g->strm); }

static int finished(const Song *g) {
    return g->seq ? sseq_finished(g->seq) : g->dse ? dse_finished(g->dse) : strm_finished(g->strm);
}

void song_render(Song *g, int16_t *out, uint32_t frames) {
    if (g->done) {
        memset(out, 0, frames * 4);
        return;
    }
    if (g->seq) sseq_render(g->seq, out, frames);
    else if (g->dse) dse_render(g->dse, out, frames);
    else strm_render(g->strm, out, frames);

    /* Decide when the fade starts. */
    if (g->fade_start == UINT64_MAX) {
        if (g->length) {
            if (g->pos + frames >= g->length) g->fade_start = g->length;
        } else if (g->loops_wanted > 0 && (song_loops(g) >= g->loops_wanted ||
                                           g->pos + frames >= (uint64_t)MAX_SONG_MS * g->rate / 1000)) {
            g->fade_start = g->pos;
        }
    }

    if (g->fade_start != UINT64_MAX) {
        for (uint32_t i = 0; i < frames; i++) {
            uint64_t t = g->pos + i;
            if (t < g->fade_start) continue;
            uint64_t into = t - g->fade_start;
            if (into >= g->fade_len) {
                memset(out + i * 2, 0, (frames - i) * 4);
                g->done = 1;
                break;
            }
            int32_t gain = (int32_t)(((g->fade_len - into) << 15) / g->fade_len);
            out[i * 2] = (int16_t)((out[i * 2] * gain) >> 15);
            out[i * 2 + 1] = (int16_t)((out[i * 2 + 1] * gain) >> 15);
        }
    }
    g->pos += frames;
    if (finished(g)) g->done = 1;
}

int song_done(const Song *g) { return g->done; }
uint32_t song_pos_ms(const Song *g) { return (uint32_t)(g->pos * 1000 / g->rate); }
uint16_t song_voices(const Song *g) { return g->seq ? sseq_voice_mask(g->seq) : g->dse ? dse_voice_mask(g->dse) : 0; }

void song_set_volume(Song *g, int percent) {
    int32_t gain = percent * GAIN_UNITY / 100;
    if (g->seq) sseq_set_gain(g->seq, gain);
    if (g->strm) strm_set_gain(g->strm, gain);
    if (g->dse) dse_set_gain(g->dse, gain);
}
