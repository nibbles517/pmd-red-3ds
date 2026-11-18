/*
 * From the NDS Music Player (ndsmusicplayer, source/core), for the port's Music
 * Options. Files are read through the backend (Backend_File*) instead of stdio.
 */
#include "dse.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "datasource.h"
#include "mixutil.h"
#include "wave.h"

#define NUM_VOICES 16
#define NUM_TRACKS 17
#define NUM_PROGRAMS 256
#define DSE_BASE_RATE 32728.0f /* tune fields are semitones relative to the DS output rate */
#define ARM7_CLOCK 33513982u
/* DSE updates from a timer with period 64*2722 ARM7 cycles (~192.38 Hz): measured against the
 * game running in an emulator (64*2728, the Nitro driver's period, drifts 0.22% slow). */
#define FRAME_CYCLES (64u * 2722u)
#define MAX_EVENTS_PER_TICK 4096
#define ENV_MAX (127 << 23)

/* ---- small helpers ----------------------------------------------------------------- */

/* Same 0.1 dB volume curve the Nitro driver uses (shared look with the SSEQ player). */
static const int16_t scale_tbl[128] = {
    -32768, -722, -721, -651, -601, -562, -530, -503, -480, -460, -442, -425, -410, -396, -383, -371,
    -360,   -349, -339, -330, -321, -313, -305, -297, -289, -282, -276, -269, -263, -257, -251, -245,
    -239,   -234, -229, -224, -219, -214, -210, -205, -201, -196, -192, -188, -184, -180, -176, -173,
    -169,   -165, -162, -158, -155, -152, -149, -145, -142, -139, -136, -133, -130, -127, -125, -122,
    -119,   -116, -114, -111, -109, -106, -103, -101, -99,  -96,  -94,  -91,  -89,  -87,  -85,  -82,
    -80,    -78,  -76,  -74,  -72,  -70,  -68,  -66,  -64,  -62,  -60,  -58,  -56,  -54,  -52,  -50,
    -49,    -47,  -45,  -43,  -42,  -40,  -38,  -36,  -35,  -33,  -31,  -30,  -28,  -27,  -25,  -23,
    -22,    -20,  -19,  -17,  -16,  -14,  -13,  -11,  -10,  -8,   -7,   -6,   -4,   -3,   -1,   0};

static int scale(int v) { return scale_tbl[v < 0 ? 0 : v > 127 ? 127 : v]; }

static int16_t amp_tbl[724];
static int amp_ready;
static void init_amp(void) {
    if (amp_ready) return;
    amp_tbl[0] = 0;
    for (int i = 1; i < 724; i++) amp_tbl[i] = (int16_t)(32767.0 * pow(10.0, (i - 723) / 200.0) + 0.5);
    amp_ready = 1;
}

/* Envelope stage lengths, from the DSE driver's lookup tables (same values ppmdu and
 * VG Music Studio use). Index 0x7F means "forever". */
static const int16_t dur16[128] = {
    0x0000, 0x0001, 0x0002, 0x0003, 0x0004, 0x0005, 0x0006, 0x0007, 0x0008, 0x0009, 0x000A, 0x000B, 0x000C,
    0x000D, 0x000E, 0x000F, 0x0010, 0x0011, 0x0012, 0x0013, 0x0014, 0x0015, 0x0016, 0x0017, 0x0018, 0x0019,
    0x001A, 0x001B, 0x001C, 0x001D, 0x001E, 0x001F, 0x0020, 0x0023, 0x0028, 0x002D, 0x0033, 0x0039, 0x0040,
    0x0048, 0x0050, 0x0058, 0x0062, 0x006D, 0x0078, 0x0083, 0x0090, 0x009E, 0x00AC, 0x00BC, 0x00CC, 0x00DE,
    0x00F0, 0x0104, 0x0119, 0x012F, 0x0147, 0x0160, 0x017A, 0x0196, 0x01B3, 0x01D2, 0x01F2, 0x0214, 0x0238,
    0x025E, 0x0285, 0x02AE, 0x02D9, 0x0307, 0x0336, 0x0367, 0x039B, 0x03D1, 0x0406, 0x0442, 0x047E, 0x04C4,
    0x0500, 0x0546, 0x058C, 0x0622, 0x0672, 0x06CC, 0x071C, 0x0776, 0x07DA, 0x0834, 0x0898, 0x0906, 0x096A,
    0x09D8, 0x0A50, 0x0ABE, 0x0B40, 0x0BB8, 0x0C3A, 0x0CBC, 0x0D48, 0x0DDE, 0x0E6A, 0x0F00, 0x0FA0, 0x1040,
    0x10EA, 0x1194, 0x123E, 0x12F2, 0x13B0, 0x146E, 0x1536, 0x15FE, 0x16D0, 0x17A2, 0x187E, 0x195A, 0x1A40,
    0x1B30, 0x1C20, 0x1D1A, 0x1E1E, 0x1F22, 0x2030, 0x2148, 0x2260, 0x2382, 0x2710, 0x7FFF};
static const int32_t dur32[128] = {
    0x00000000, 0x00000004, 0x00000007, 0x0000000A, 0x0000000F, 0x00000015, 0x0000001C, 0x00000024, 0x0000002E,
    0x0000003A, 0x00000048, 0x00000057, 0x00000068, 0x0000007B, 0x00000091, 0x000000A8, 0x00000185, 0x000001BE,
    0x000001FC, 0x0000023F, 0x00000288, 0x000002D6, 0x0000032A, 0x00000385, 0x000003E5, 0x0000044C, 0x000004BA,
    0x0000052E, 0x000005A9, 0x0000062C, 0x000006B5, 0x00000746, 0x00000BCF, 0x00000CC0, 0x00000DBD, 0x00000EC6,
    0x00000FDC, 0x000010FF, 0x0000122F, 0x0000136C, 0x000014B6, 0x0000160F, 0x00001775, 0x000018EA, 0x00001A6D,
    0x00001BFF, 0x00001DA0, 0x00001F51, 0x00002C16, 0x00002E80, 0x00003100, 0x00003395, 0x00003641, 0x00003902,
    0x00003BDB, 0x00003ECA, 0x000041D0, 0x000044EE, 0x00004824, 0x00004B73, 0x00004ED9, 0x00005259, 0x000055F2,
    0x000059A4, 0x000074CC, 0x000079AB, 0x00007EAC, 0x000083CE, 0x00008911, 0x00008E77, 0x000093FF, 0x000099AA,
    0x00009F78, 0x0000A56A, 0x0000AB80, 0x0000B1BB, 0x0000B81A, 0x0000BE9E, 0x0000C547, 0x0000CC17, 0x0000FD42,
    0x000105CB, 0x00010E82, 0x00011768, 0x0001207E, 0x000129C4, 0x0001333B, 0x00013CE2, 0x000146BB, 0x000150C5,
    0x00015B02, 0x00016572, 0x00017015, 0x00017AEB, 0x000185F5, 0x00019133, 0x0001E16D, 0x0001EF07, 0x0001FCE0,
    0x00020AF7, 0x0002194F, 0x000227E6, 0x000236BE, 0x000245D7, 0x00025532, 0x000264CF, 0x000274AE, 0x000284D0,
    0x00029536, 0x0002A5E0, 0x0002B6CE, 0x0002C802, 0x000341B0, 0x000355F8, 0x00036A90, 0x00037F79, 0x000394B4,
    0x0003AA41, 0x0003C021, 0x0003D654, 0x0003ECDA, 0x000403B5, 0x00041AE5, 0x0004326A, 0x00044A45, 0x00046277,
    0x00047B00, 0x7FFFFFFF};

/* Table values are milliseconds (checked against the real engine's release/decay curves);
 * convert to sequencer frames. */
#define DSE_FRAME_HZ ((double)ARM7_CLOCK / (double)FRAME_CYCLES)
static int32_t env_frames(int param, int mult) {
    param &= 0x7F;
    double ms = mult ? (double)dur16[param] * mult : (double)dur32[param];
    double f = ms * DSE_FRAME_HZ / 1000.0;
    return f > 0x7FFFFFF0 ? 0x7FFFFFF0 : (int32_t)(f + 0.5);
}

/* Event parameter lengths for 0x90-0xFF; -1 = unknown (stop the track). */
static const int8_t evlen[0x70] = {
    /*90*/ 0, 1, 1, 2, 3, 1, -1, -1, 0, 0, -1, -1, 1, 0, 0, -1,
    /*A0*/ 1, 1, -1, -1, 1, 1, -1, -1, 2, 1, 1, 1, 1, -1, -1, 3,
    /*B0*/ 0, 1, 1, 1, 2, 1, 1, -1, -1, -1, -1, -1, 1, -1, 1, 1,
    /*C0*/ 0, -1, -1, 1, -1, -1, -1, -1, -1, -1, -1, 2, -1, -1, -1, -1,
    /*D0*/ 1, 1, 1, 2, 3, 2, 2, 2, 2, -1, -1, 1, 5, 4, -1, 1,
    /*E0*/ 1, 1, 3, 1, 5, 4, -1, 1, 1, 1, 3, -1, 5, 4, -1, 1,
    /*F0*/ 5, 4, 2, 3, -1, -1, 1, -1, 2, -1, -1, -1, -1, -1, -1, -1};
static const uint8_t fixed_rest[16] = {96, 72, 64, 48, 36, 32, 24, 18, 16, 12, 9, 8, 6, 4, 3, 2};

/* ---- banks ------------------------------------------------------------------------- */

typedef struct {
    const uint8_t *wavi; /* chunk data (offset table + entries) */
    uint32_t wavi_len, nslots;
    const uint8_t *pcmd;
    uint32_t pcmd_len;
} SampleSrc;

struct DseBank {
    uint8_t *data;
    uint32_t size;
    SampleSrc src;
};

/* Finds a chunk by label after the 0x50-byte SWDL header. */
static const uint8_t *find_chunk(const uint8_t *d, uint32_t size, const char *label, uint32_t *len) {
    uint32_t p = 0x50;
    while (p + 16 <= size) {
        uint32_t n = rd32(d + p + 12);
        if (memcmp(d + p, label, 4) == 0) {
            if (n > size - p - 16) n = size - p - 16;
            *len = n;
            return d + p + 16;
        }
        if (memcmp(d + p, "eod ", 4) == 0 || n > size - p - 16) break;
        p = (p + 16 + n + 3) & ~3u;
    }
    return NULL;
}

static int swdl_ok(const uint8_t *d, uint32_t size) {
    return d && size >= 0x50 && memcmp(d, "swdl", 4) == 0 && rd16(d + 0x0C) == 0x415;
}

static void sample_src(const uint8_t *d, uint32_t size, SampleSrc *s) {
    memset(s, 0, sizeof *s);
    if (!swdl_ok(d, size)) return;
    s->wavi = find_chunk(d, size, "wavi", &s->wavi_len);
    s->pcmd = find_chunk(d, size, "pcmd", &s->pcmd_len);
    s->nslots = rd16(d + 0x46);
    if (s->wavi && s->nslots > s->wavi_len / 2) s->nslots = s->wavi_len / 2;
}

static const uint8_t *wavi_entry(const SampleSrc *s, uint32_t id) {
    if (!s->wavi || id >= s->nslots) return NULL;
    uint32_t o = rd16(s->wavi + id * 2);
    if (o == 0 || o > s->wavi_len || s->wavi_len - o < 64) return NULL;
    return s->wavi + o;
}

DseBank *dse_bank_open(uint8_t *swd, uint32_t size) {
    if (!swdl_ok(swd, size)) {
        free(swd);
        return NULL;
    }
    DseBank *b = (DseBank *)calloc(1, sizeof *b);
    if (!b) {
        free(swd);
        return NULL;
    }
    b->data = swd;
    b->size = size;
    sample_src(swd, size, &b->src);
    if (!b->src.wavi || !b->src.pcmd) {
        dse_bank_close(b);
        return NULL;
    }
    return b;
}

void dse_bank_close(DseBank *b) {
    if (!b) return;
    free(b->data);
    free(b);
}

/* Decodes sample `id` to 16-bit PCM. */
static int decode_sample(const SampleSrc *s, uint32_t id, Wave *w) {
    memset(w, 0, sizeof *w);
    const uint8_t *e = wavi_entry(s, id);
    if (!e || !s->pcmd) return -1;
    uint16_t fmt = rd16(e + 0x12);
    uint8_t loop = e[0x15];
    uint32_t rate = rd32(e + 0x20), pos = rd32(e + 0x24);
    uint32_t loopbeg = rd32(e + 0x28), looplen = rd32(e + 0x2C);
    uint64_t bytes = ((uint64_t)loopbeg + looplen) * 4;
    if (pos > s->pcmd_len) return -1;
    if (bytes > s->pcmd_len - pos) bytes = s->pcmd_len - pos;
    const uint8_t *src = s->pcmd + pos;
    uint32_t nb = (uint32_t)bytes, n, ls;
    switch (fmt) {
    case 0x000: n = nb; ls = loopbeg * 4; break;
    case 0x100: n = nb / 2; ls = loopbeg * 2; break;
    case 0x200:
        if (nb < 4) return -1;
        n = (nb - 4) * 2;
        ls = loopbeg ? (loopbeg * 4 - 4) * 2 : 0;
        break;
    default: return -1; /* PSG etc. not used by these games */
    }
    if (n == 0) return -1;
    w->data = (int16_t *)malloc(((size_t)n + 1) * sizeof(int16_t));
    if (!w->data) return -1;
    if (fmt == 0x000)
        for (uint32_t i = 0; i < n; i++) w->data[i] = (int16_t)((int8_t)src[i] * 256);
    else if (fmt == 0x100)
        for (uint32_t i = 0; i < n; i++) w->data[i] = (int16_t)rd16(src + i * 2);
    else {
        AdpcmState st;
        adpcm_init(&st, src);
        adpcm_decode(&st, src + 4, nb - 4, w->data);
    }
    w->len = n;
    w->loops = loop && ls < n;
    w->loop_start = w->loops ? ls : 0;
    w->data[n] = w->loops ? w->data[w->loop_start] : 0;
    w->rate = rate ? (float)rate : DSE_BASE_RATE;
    return 0;
}

/* ---- instruments ------------------------------------------------------------------- */

typedef struct {
    uint8_t lokey, hikey, lovel, hivel;
    uint16_t sid;
    int8_t ctune;
    uint8_t ftune, root, vol, pan, kgrp;
    uint8_t env_mult, atkvol, attack, decay, sustain, hold, decay2, release;
} Split;

/* Pitch LFOs, measured against the game's engine. Entry: [1] 1 = on (other values are left to the
 * game), [2] 1 = pitch, [3] wave shape, u16 +4 depth in 1/128 semitone, u16 +8 rate (2 ms per
 * unit), u16 +10 delay and u16 +12 fade-in in milliseconds. Shapes, with d = depth:
 *   1 square +2d/-2d, period 2*rate ms      2 one-sided triangle 0..+2d, period 2*rate ms
 *   3 triangle +-d starting upward, 2*rate   4 rising saw 0..+2d, period rate ms
 * Several pitch LFOs on one program add up. */
#define MAX_LFOS 4
typedef struct {
    float semis, period_ms, delay_ms, fade_ms;
    uint8_t shape;
} PitchLfo;

typedef struct {
    uint8_t present, vol, pan;
    uint8_t nlfo;
    PitchLfo lfo[MAX_LFOS];
    uint16_t nsplits;
    Split *splits;
} Program;

typedef struct {
    uint8_t poly, lo, hi;
} KeyGroup;

/* ---- player ------------------------------------------------------------------------ */

enum { ENV_ATTACK = 1, ENV_HOLD, ENV_DECAY, ENV_DECAY2, ENV_SUSTAIN, ENV_RELEASE };

typedef struct {
    uint8_t active;
    uint8_t track, key, vel, kgrp, state;
    const Wave *wave;
    const Split *split;
    const PitchLfo *lfo;
    uint8_t nlfo;
    uint32_t frames; /* since key-on */
    uint8_t prg_vol, prg_pan;
    float base_rate;
    uint64_t pos, step;
    int32_t env, env_target, env_inc, env_left; /* env in 23-bit fixed point, 0..127 */
    int32_t length;                             /* ticks until key off; <0 = released */
    uint32_t age;
    int32_t gain_l, gain_r;
} Voice;

typedef struct {
    uint8_t active;
    uint32_t pos, end, loop_pos;
    uint8_t has_loop;
    uint32_t rest, last_rest, last_dur;
    int octave;
    uint8_t program, vol, expr, pan, bend_range;
    int16_t bend;
    int loops;
} Track;

struct DsePlayer {
    uint8_t *smd;
    uint32_t smd_size;
    Program prog[NUM_PROGRAMS];
    KeyGroup kgrp[256];
    Wave *waves; /* indexed by sample id */
    uint32_t nwaves;
    Track trk[NUM_TRACKS];
    int ntrk;
    Voice v[NUM_VOICES];
    uint32_t age_counter;
    uint16_t tempo, tpqn;
    uint32_t tempo_acc;
    int32_t gain;
    uint32_t out_rate;
    uint64_t clk_acc, clk_per_frame;
    uint64_t samples_out;
    int32_t *mix;
    uint32_t mix_cap;
#if NDSMUSIC_DSE_TRACE
    FILE *trace;
#endif
};

static void parse_programs(DsePlayer *p, const uint8_t *swd, uint32_t size) {
    uint32_t len;
    const uint8_t *pg = find_chunk(swd, size, "prgi", &len);
    if (pg) {
        uint32_t n = rd16(swd + 0x48);
        if (n > len / 2) n = len / 2;
        for (uint32_t i = 0; i < n && i < NUM_PROGRAMS; i++) {
            uint32_t o = rd16(pg + i * 2);
            if (!o || o > len || len - o < 16) continue;
            const uint8_t *h = pg + o;
            uint32_t ns = rd16(h + 2), nlfo = h[0x0B];
            uint32_t sp = o + 16 + 16 * nlfo + 16;
            if (sp > len || ns > (len - sp) / 48) continue;
            Program *pr = &p->prog[i];
            pr->splits = (Split *)calloc(ns ? ns : 1, sizeof(Split));
            if (!pr->splits) continue;
            pr->present = 1;
            pr->vol = h[4];
            pr->pan = h[5];
            for (uint32_t l = 0; l < nlfo; l++) {
                const uint8_t *e = h + 16 + 16 * l;
                uint16_t depth = rd16(e + 4), period = rd16(e + 8);
                if (e[1] != 1 || e[2] != 1 || !depth || !period || pr->nlfo >= MAX_LFOS) continue;
                PitchLfo *lf = &pr->lfo[pr->nlfo++];
                lf->semis = depth / 128.0f;
                lf->shape = e[3];
                lf->period_ms = period * (e[3] == 4 ? 1.0f : 2.0f);
                lf->delay_ms = rd16(e + 10);
                lf->fade_ms = rd16(e + 12);
            }
            pr->nsplits = (uint16_t)ns;
            for (uint32_t k = 0; k < ns; k++) {
                const uint8_t *e = pg + sp + 48 * k;
                Split *s = &pr->splits[k];
                s->lokey = e[0x04];
                s->hikey = e[0x05];
                s->lovel = e[0x08];
                s->hivel = e[0x09];
                s->sid = rd16(e + 0x12);
                s->ftune = e[0x14];
                s->ctune = (int8_t)e[0x15];
                s->root = e[0x16];
                s->vol = e[0x18];
                s->pan = e[0x19];
                s->kgrp = e[0x1A];
                s->env_mult = e[0x21];
                s->atkvol = e[0x28];
                s->attack = e[0x29];
                s->decay = e[0x2A];
                s->sustain = e[0x2B];
                s->hold = e[0x2C];
                s->decay2 = e[0x2D];
                s->release = e[0x2E];
            }
        }
    }
    for (int i = 0; i < 256; i++) {
        p->kgrp[i].poly = 0xFF;
        p->kgrp[i].lo = 0;
        p->kgrp[i].hi = NUM_VOICES - 1;
    }
    const uint8_t *kg = find_chunk(swd, size, "kgrp", &len);
    if (kg)
        for (uint32_t i = 0; i + 8 <= len; i += 8) {
            uint16_t id = rd16(kg + i);
            if (id > 255) continue;
            p->kgrp[id].poly = kg[i + 2];
            p->kgrp[id].lo = kg[i + 4] < NUM_VOICES ? kg[i + 4] : 0;
            p->kgrp[id].hi = kg[i + 5] < NUM_VOICES ? kg[i + 5] : NUM_VOICES - 1;
            if (p->kgrp[id].lo > p->kgrp[id].hi) p->kgrp[id].lo = 0;
        }
}

static int load_samples(DsePlayer *p, const DseBank *bank, const uint8_t *swd, uint32_t swd_size) {
    /* A song bank with its own sample data (pcmd present, size not the 0xAAAAxxxx marker) wins. */
    SampleSrc local, src;
    sample_src(swd, swd_size, &local);
    uint32_t pcmd_marker = swd_size >= 0x44 ? rd32(swd + 0x40) : 0;
    if (local.pcmd && local.wavi && (pcmd_marker & 0xFFFF0000u) != 0xAAAA0000u) src = local;
    else if (bank) src = bank->src;
    else return 0;

    p->nwaves = src.nslots;
    p->waves = (Wave *)calloc(p->nwaves ? p->nwaves : 1, sizeof(Wave));
    if (!p->waves) return 0;
    for (int i = 0; i < NUM_PROGRAMS; i++)
        for (uint32_t k = 0; k < p->prog[i].nsplits; k++) {
            uint16_t sid = p->prog[i].splits[k].sid;
            if (sid < p->nwaves && !p->waves[sid].data) decode_sample(&src, sid, &p->waves[sid]);
        }
    return 1;
}

/* ---- envelope ------------------------------------------------------------------------ */

static void env_plan(Voice *v, int target, int param) {
    if ((param & 0x7F) == 0x7F) { /* stay here forever */
        v->env_left = -1;
        v->env_inc = 0;
        return;
    }
    v->env_target = target << 23;
    v->env_left = env_frames(param, v->split->env_mult);
    v->env_inc = v->env_left > 0 ? (v->env_target - v->env) / v->env_left : 0;
}

/* Moves to the next stage after `from`, skipping zero-length ones. */
static void env_next(Voice *v, int from) {
    const Split *s = v->split;
    for (int st = from + 1;; st++) {
        switch (st) {
        case ENV_HOLD:
            if (!s->hold) continue;
            v->env = ENV_MAX;
            env_plan(v, 127, s->hold);
            break;
        case ENV_DECAY:
            if (!s->decay) {
                v->env = s->sustain << 23;
                continue;
            }
            env_plan(v, s->sustain, s->decay);
            break;
        case ENV_DECAY2:
            if (!s->decay2) {
                st = ENV_SUSTAIN;
                v->env_left = -1;
                v->env_inc = 0;
                break;
            }
            env_plan(v, 0, s->decay2);
            break;
        default: /* past decay2: silent */
            v->env = 0;
            v->state = ENV_RELEASE;
            v->env_left = 0;
            return;
        }
        v->state = (uint8_t)st;
        return;
    }
}

static void env_start(Voice *v) {
    const Split *s = v->split;
    if (s->attack) {
        v->env = s->atkvol << 23;
        v->state = ENV_ATTACK;
        env_plan(v, 127, s->attack);
    } else {
        v->env = ENV_MAX;
        env_next(v, ENV_ATTACK);
    }
}

static void env_release(Voice *v) {
    if (v->state == ENV_RELEASE) return;
    v->state = ENV_RELEASE;
    v->length = -1;
    env_plan(v, 0, v->split->release);
}

/* Returns 0 once the voice has finished. */
static int env_step(Voice *v) {
    if (v->env_left < 0) return 1; /* holding */
    if (v->env_left > 0) {
        v->env_left--;
        v->env += v->env_inc;
        if (v->env < 0) v->env = 0;
        if (v->env > ENV_MAX) v->env = ENV_MAX;
        return 1;
    }
    v->env = v->env_target;
    if (v->state == ENV_RELEASE) return 0;
    env_next(v, v->state);
    return v->state != ENV_RELEASE || v->env > 0 || v->env_left > 0;
}

/* ---- voices -------------------------------------------------------------------------- */

static int voice_level(const Voice *v) { return v->env >> 23; }

static int alloc_voice(DsePlayer *p, const KeyGroup *kg, int kgrp_id) {
    /* Key group polyphony: steal the oldest note of the same group when full. */
    if (kg->poly != 0xFF && kg->poly > 0) {
        int count = 0, oldest = -1;
        for (int i = 0; i < NUM_VOICES; i++) {
            const Voice *v = &p->v[i];
            if (!v->active || v->kgrp != kgrp_id || v->state == ENV_RELEASE) continue;
            count++;
            if (oldest < 0 || v->age < p->v[oldest].age) oldest = i;
        }
        if (count >= kg->poly && oldest >= 0) return oldest;
    }
    int best = -1, best_score = 0;
    for (int i = kg->lo; i <= kg->hi; i++) {
        const Voice *v = &p->v[i];
        if (!v->active) return i;
        /* releasing and quiet voices go first */
        int score = (v->state == ENV_RELEASE ? 0 : 256) + voice_level(v);
        if (best < 0 || score < best_score || (score == best_score && v->age < p->v[best].age)) {
            best = i;
            best_score = score;
        }
    }
    return best;
}

static void start_voice(DsePlayer *p, int ti, const Program *pr, const Split *s, int key, int vel, uint32_t dur) {
    if (s->sid >= p->nwaves || !p->waves[s->sid].data) return;
    int vi = alloc_voice(p, &p->kgrp[s->kgrp], s->kgrp);
    if (vi < 0) return;
    Voice *v = &p->v[vi];
    memset(v, 0, sizeof *v);
    v->active = 1;
    v->track = (uint8_t)ti;
    v->key = (uint8_t)key;
    v->vel = (uint8_t)vel;
    v->kgrp = s->kgrp;
    v->wave = &p->waves[s->sid];
    v->split = s;
    v->prg_vol = pr->vol;
    v->prg_pan = pr->pan;
    v->lfo = pr->lfo;
    v->nlfo = pr->nlfo;
    v->age = ++p->age_counter;
    v->length = (int32_t)dur;
    /* The tune fields give the sample's rate as semitones from 32728 Hz. */
    if (s->ctune || s->ftune) v->base_rate = DSE_BASE_RATE * exp2f((s->ctune + s->ftune / 256.0f) / 12.0f);
    else v->base_rate = v->wave->rate;
    env_start(v);
#if NDSMUSIC_DSE_TRACE
    if (p->trace)
        fprintf(p->trace, "K %.4f %d sid=%u key=%d root=%d vel=%d dur=%u trk=%d prg=%d\n",
                (double)p->samples_out / p->out_rate, vi, s->sid, key, s->root, vel, (unsigned)dur, ti,
                p->trk[ti].program);
#endif
}

static void note_on(DsePlayer *p, int ti, int key, int vel, uint32_t dur) {
    const Program *pr = &p->prog[p->trk[ti].program];
    if (!pr->present) return;
    /* Every zone covering the key and velocity plays (zones can overlap to layer samples). */
    for (uint32_t k = 0; k < pr->nsplits; k++) {
        const Split *c = &pr->splits[k];
        if (key >= c->lokey && key <= c->hikey && vel >= c->lovel && vel <= c->hivel)
            start_voice(p, ti, pr, c, key, vel, dur);
    }
}

static void voice_update(DsePlayer *p, Voice *v) {
    if (v->length == 0) env_release(v);
    if (!env_step(v)) {
        v->active = 0;
        return;
    }
    const Track *t = &p->trk[v->track];
    const Split *s = v->split;
    float semis = (float)(v->key - s->root) + (float)t->bend * t->bend_range / 8192.0f;
    for (int i = 0; i < v->nlfo; i++) {
        const PitchLfo *lf = &v->lfo[i];
        float ms = (float)v->frames * (float)(1000.0 / DSE_FRAME_HZ) - lf->delay_ms;
        if (ms < 0) continue;
        float depth = lf->semis;
        if (lf->fade_ms > 0 && ms < lf->fade_ms) depth *= ms / lf->fade_ms;
        float ph = ms / lf->period_ms, w;
        ph -= floorf(ph);
        switch (lf->shape) {
        case 1: w = ph < 0.5f ? 2.0f : -2.0f; break;
        case 2: ph += ph < 0.25f ? 0.75f : -0.25f; /* triangle shifted to start at 0 */
                w = 1.0f + (ph < 0.25f ? 4.0f * ph : ph < 0.75f ? 2.0f - 4.0f * ph : 4.0f * ph - 4.0f); break;
        case 4: w = 2.0f * ph; break;
        default: w = ph < 0.25f ? 4.0f * ph : ph < 0.75f ? 2.0f - 4.0f * ph : 4.0f * ph - 4.0f; break;
        }
        semis += depth * w;
    }
    v->frames++;
    float freq = v->base_rate * exp2f(semis / 12.0f);
    float stepf = freq / (float)p->out_rate;
    if (stepf > 64.0f) stepf = 64.0f;
    v->step = (uint64_t)(stepf * 4294967296.0f);

    int32_t vol = scale(v->vel) + scale(voice_level(v)) + scale(t->vol) + scale(t->expr) + scale(v->prg_vol) +
                  scale(s->vol);
    if (vol < -723) vol = -723;
    int32_t amp = amp_tbl[vol + 723];
    int32_t pan = t->pan + (s->pan - 64) + (v->prg_pan - 64);
    if (pan < 0) pan = 0;
    if (pan > 127) pan = 127;
    v->gain_l = (amp * (127 - pan)) >> 7;
    v->gain_r = (amp * pan) >> 7;
}

/* ---- sequencer ----------------------------------------------------------------------- */

static int rd8(DsePlayer *p, Track *t) { return t->pos < t->end ? p->smd[t->pos++] : -1; }

static void track_stop(Track *t) { t->active = 0; }

static void exec_event(DsePlayer *p, int ti) {
    Track *t = &p->trk[ti];
    int c = rd8(p, t);
    if (c < 0) {
        track_stop(t);
        return;
    }
    if (c < 0x80) { /* note: velocity = c */
        int a = rd8(p, t);
        if (a < 0) {
            track_stop(t);
            return;
        }
        int nbytes = (a >> 6) & 3;
        t->octave += ((a >> 4) & 3) - 2;
        int k = a & 0x0F;
        if (nbytes) {
            uint32_t d = 0;
            for (int i = 0; i < nbytes; i++) {
                int b = rd8(p, t);
                if (b < 0) break;
                d = (d << 8) | (uint32_t)b;
            }
            t->last_dur = d;
        }
        if (k < 12) {
            int key = t->octave * 12 + k;
            if (key >= 0 && key < 128) note_on(p, ti, key, c, t->last_dur);
        }
        return;
    }
    if (c < 0x90) {
        t->last_rest = t->rest = fixed_rest[c - 0x80];
        return;
    }
    int n = evlen[c - 0x90];
    if (n < 0 || t->end - t->pos < (uint32_t)n) {
        track_stop(t);
        return;
    }
    const uint8_t *a = p->smd + t->pos;
    t->pos += (uint32_t)n;
    switch (c) {
    case 0x90: t->rest = t->last_rest; break;
    case 0x91: {
        int32_t r = (int32_t)t->last_rest + (int8_t)a[0];
        t->last_rest = t->rest = r < 0 ? 0 : (uint32_t)r;
        break;
    }
    case 0x92: t->last_rest = t->rest = a[0]; break;
    case 0x93: t->last_rest = t->rest = (uint32_t)(a[0] | a[1] << 8); break;
    case 0x94: t->last_rest = t->rest = (uint32_t)(a[0] | a[1] << 8 | a[2] << 16); break;
    case 0x98: /* end of track */
        if (t->has_loop) {
            t->pos = t->loop_pos;
            t->loops++;
        } else {
            track_stop(t);
        }
        break;
    case 0x99:
        t->loop_pos = t->pos;
        t->has_loop = 1;
        break;
    case 0xA0: t->octave = a[0]; break;
    case 0xA1: t->octave += (int8_t)a[0]; break;
    case 0xA4:
    case 0xA5: p->tempo = a[0] ? a[0] : 1; break;
    case 0xAC: t->program = a[0]; break;
    case 0xD7: t->bend = (int16_t)(a[0] << 8 | a[1]); break; /* big-endian, +-8192 = full range */
    case 0xDB: t->bend_range = a[0]; break;
    case 0xE0: t->vol = a[0] & 0x7F; break;
    case 0xE3: t->expr = a[0] & 0x7F; break;
    case 0xE8: t->pan = a[0] & 0x7F; break;
    default: break; /* modulation, LFO and other rarely-used controls: ignored for now */
    }
}

static void seq_tick(DsePlayer *p) {
    for (int i = 0; i < NUM_VOICES; i++)
        if (p->v[i].active && p->v[i].length > 0) p->v[i].length--;
    for (int ti = 0; ti < p->ntrk; ti++) {
        Track *t = &p->trk[ti];
        if (!t->active) continue;
        if (t->rest > 0 && --t->rest > 0) continue;
        int guard = 0;
        while (t->active && t->rest == 0) {
            if (++guard > MAX_EVENTS_PER_TICK) {
                track_stop(t);
                break;
            }
            exec_event(p, ti);
        }
    }
}

static void seq_frame(DsePlayer *p) {
    p->tempo_acc += (uint32_t)p->tempo * p->tpqn;
    while (p->tempo_acc >= 60u * 192u) {
        p->tempo_acc -= 60u * 192u;
        seq_tick(p);
    }
    for (int i = 0; i < NUM_VOICES; i++) {
        Voice *v = &p->v[i];
        if (!v->active) continue;
        voice_update(p, v);
#if NDSMUSIC_DSE_TRACE
        if (p->trace && v->active)
            fprintf(p->trace, "S %.4f %d sid=%u env=%d amp=%d pan=%d st=%d step=%llu\n", (double)p->samples_out / p->out_rate, i,
                    v->split->sid, voice_level(v), (v->gain_l + v->gain_r), (v->gain_r * 127) / (v->gain_l + v->gain_r + 1),
                    v->state, (unsigned long long)v->step);
#endif
    }
}

/* ---- mixing -------------------------------------------------------------------------- */

static void mix_voice(Voice *v, int32_t *mix, uint32_t n) {
    const Wave *w = v->wave;
    uint64_t end = (uint64_t)w->len << 32;
    uint64_t loop_len = (uint64_t)(w->len - w->loop_start) << 32;
    int32_t gl = v->gain_l, gr = v->gain_r;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t idx = (uint32_t)(v->pos >> 32);
        int32_t frac = (int32_t)((v->pos >> 17) & 0x7FFF);
        int32_t s0 = w->data[idx], s1 = w->data[idx + 1];
        int32_t s = s0 + (((s1 - s0) * frac) >> 15);
        mix[i * 2] += (s * gl) >> 15;
        mix[i * 2 + 1] += (s * gr) >> 15;
        v->pos += v->step;
        if (v->pos >= end) {
            if (!w->loops || loop_len == 0) {
                v->active = 0;
                return;
            }
            while (v->pos >= end) v->pos -= loop_len;
        }
    }
}

void dse_render(DsePlayer *p, int16_t *out, uint32_t frames) {
    while (frames) {
        if (p->clk_acc >= p->clk_per_frame) {
            p->clk_acc -= p->clk_per_frame;
            seq_frame(p);
        }
        uint64_t left = p->clk_per_frame - p->clk_acc;
        uint32_t n = (uint32_t)((left + ARM7_CLOCK - 1) / ARM7_CLOCK);
        if (n == 0) n = 1;
        if (n > frames) n = frames;
        if (n > p->mix_cap) n = p->mix_cap;
        memset(p->mix, 0, n * 2 * sizeof(int32_t));
        for (int i = 0; i < NUM_VOICES; i++)
            if (p->v[i].active) mix_voice(&p->v[i], p->mix, n);
        for (uint32_t i = 0; i < n * 2; i++) out[i] = soft_clip((int32_t)(((int64_t)p->mix[i] * p->gain) >> 8));
        out += n * 2;
        frames -= n;
        p->clk_acc += (uint64_t)n * ARM7_CLOCK;
        p->samples_out += n;
    }
}

/* ---- lifecycle ----------------------------------------------------------------------- */

int dse_is_smdl(const uint8_t *d, uint32_t size) {
    return d && size >= 0x80 && memcmp(d, "smdl", 4) == 0 && rd16(d + 0x0C) == 0x415 && memcmp(d + 0x40, "song", 4) == 0;
}

void dse_smd_label(const uint8_t *smd, uint32_t size, char out[17]) {
    out[0] = 0;
    if (size < 0x30) return;
    memcpy(out, smd + 0x20, 16);
    out[16] = 0;
    for (int i = 0; out[i]; i++) {
        if (out[i] == '.') { /* names are truncated file names: drop the partial ".SMD" */
            out[i] = 0;
            break;
        }
        if (out[i] < 0x20 || out[i] > 0x7E) out[i] = '?';
    }
}

DsePlayer *dse_create(const DseBank *bank, const uint8_t *smd, uint32_t smd_size, const uint8_t *swd,
                      uint32_t swd_size, uint32_t out_rate) {
    if (!dse_is_smdl(smd, smd_size) || !swdl_ok(swd, swd_size) || out_rate == 0) return NULL;
    init_amp();
    DsePlayer *p = (DsePlayer *)calloc(1, sizeof *p);
    if (!p) return NULL;
    p->smd = (uint8_t *)malloc(smd_size);
    p->mix_cap = 1024;
    p->mix = (int32_t *)malloc(p->mix_cap * 2 * sizeof(int32_t));
    if (!p->smd || !p->mix) {
        dse_destroy(p);
        return NULL;
    }
    memcpy(p->smd, smd, smd_size);
    p->smd_size = smd_size;
    p->out_rate = out_rate;
    p->clk_per_frame = (uint64_t)FRAME_CYCLES * out_rate;
    p->clk_acc = p->clk_per_frame;
    p->gain = GAIN_UNITY;
    p->tempo = 120;
    p->tpqn = rd16(smd + 0x40 + 0x12);
    if (p->tpqn == 0) p->tpqn = 48;

    parse_programs(p, swd, swd_size);
    if (!load_samples(p, bank, swd, swd_size)) {
        dse_destroy(p);
        return NULL;
    }

    /* Tracks follow the 64-byte song chunk: "trk " header (16 bytes), 4-byte preamble, events. */
    uint32_t pos = 0x80;
    while (pos + 20 <= smd_size && p->ntrk < NUM_TRACKS && memcmp(p->smd + pos, "trk ", 4) == 0) {
        uint32_t len = rd32(p->smd + pos + 12);
        if (len > smd_size - pos - 16) len = smd_size - pos - 16;
        Track *t = &p->trk[p->ntrk++];
        t->active = len > 4;
        t->pos = pos + 16 + 4;
        t->end = pos + 16 + len;
        t->octave = 4;
        t->vol = 127;
        t->expr = 127;
        t->pan = 64;
        t->bend_range = 2;
        pos = (pos + 16 + len + 3) & ~3u;
    }
    if (p->ntrk == 0) {
        dse_destroy(p);
        return NULL;
    }
    return p;
}

void dse_destroy(DsePlayer *p) {
    if (!p) return;
    for (int i = 0; i < NUM_PROGRAMS; i++) free(p->prog[i].splits);
    if (p->waves)
        for (uint32_t i = 0; i < p->nwaves; i++) wave_free(&p->waves[i]);
    free(p->waves);
    free(p->smd);
    free(p->mix);
    free(p);
}

int dse_finished(const DsePlayer *p) {
    for (int i = 0; i < p->ntrk; i++)
        if (p->trk[i].active) return 0;
    for (int i = 0; i < NUM_VOICES; i++)
        if (p->v[i].active) return 0;
    return 1;
}

int dse_loops(const DsePlayer *p) {
    int m = -1;
    for (int i = 0; i < p->ntrk; i++) {
        const Track *t = &p->trk[i];
        if (!t->has_loop) continue;
        if (m < 0 || t->loops < m) m = t->loops;
    }
    return m < 0 ? 0 : m;
}

uint16_t dse_voice_mask(const DsePlayer *p) {
    uint16_t m = 0;
    for (int i = 0; i < NUM_VOICES; i++)
        if (p->v[i].active && voice_level(&p->v[i]) > 4) m |= (uint16_t)(1u << i);
    return m;
}

void dse_set_gain(DsePlayer *p, int32_t g) { p->gain = g < 0 ? 0 : g > GAIN_MAX ? GAIN_MAX : g; }
#if NDSMUSIC_DSE_TRACE
void dse_set_trace(DsePlayer *p, FILE *f) { p->trace = f; }
#endif
