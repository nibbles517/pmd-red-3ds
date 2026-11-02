/*
 * From the NDS Music Player (ndsmusicplayer, source/core), for the port's Music
 * Options. Files are read through the backend (Backend_File*) instead of stdio.
 */
#include "sseq.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "datasource.h"
#include "wave.h"

#define NUM_CH 16
#define NUM_TRK 16
#define STACK_DEPTH 3
#define AMPL_MIN (-723 * 128)
/* PSG and noise run from hardware timer 0x1000000/(262*8) at the base key, i.e. middle C (~262 Hz),
 * as in the Nitro sound driver. Each square-wave cycle is 8 duty steps. */
#define PSG_BASE_RATE ((float)DS_TIMER_CLOCK / (float)(0x1000000 / (262 * 8)))
#define MAX_CMDS_PER_TICK 4096
/* Output above this level is softly compressed instead of hard-clipped. */
#define SOFT_KNEE 24576

/* One sequencer frame = 64 * 2728 ARM7 cycles (~5.2 ms, ~192 Hz), as in the DS SDK. */
#define ARM7_CLOCK 33513982u
#define FRAME_CYCLES (64u * 2728u)

enum { K_PCM, K_PSG, K_NOISE };
enum { ST_ATTACK, ST_DECAY, ST_SUSTAIN, ST_RELEASE };
enum { OVR_NONE, OVR_RANDOM, OVR_VAR };
enum { ARG_U8, ARG_S8, ARG_U16, ARG_S16, ARG_VL };
enum { STK_CALL, STK_LOOP };

/* ---- tables ----------------------------------------------------------------------- */

/* Volume curve in 0.1 dB units: 400*log10(x/127), as used by the Nitro sound driver. */
static const int16_t scale_tbl[128] = {
    -32768, -722, -721, -651, -601, -562, -530, -503, -480, -460, -442, -425, -410, -396, -383, -371,
    -360,   -349, -339, -330, -321, -313, -305, -297, -289, -282, -276, -269, -263, -257, -251, -245,
    -239,   -234, -229, -224, -219, -214, -210, -205, -201, -196, -192, -188, -184, -180, -176, -173,
    -169,   -165, -162, -158, -155, -152, -149, -145, -142, -139, -136, -133, -130, -127, -125, -122,
    -119,   -116, -114, -111, -109, -106, -103, -101, -99,  -96,  -94,  -91,  -89,  -87,  -85,  -82,
    -80,    -78,  -76,  -74,  -72,  -70,  -68,  -66,  -64,  -62,  -60,  -58,  -56,  -54,  -52,  -50,
    -49,    -47,  -45,  -43,  -42,  -40,  -38,  -36,  -35,  -33,  -31,  -30,  -28,  -27,  -25,  -23,
    -22,    -20,  -19,  -17,  -16,  -14,  -13,  -11,  -10,  -8,   -7,   -6,   -4,   -3,   -1,   0};

static const uint8_t attack_tbl[19] = {0x00, 0x01, 0x05, 0x0E, 0x1A, 0x26, 0x33, 0x3F, 0x49, 0x54,
                                       0x5C, 0x64, 0x6D, 0x74, 0x7B, 0x7F, 0x84, 0x89, 0x8F};

/* Quarter sine, 0..127. */
static const uint8_t sine_tbl[33] = {0,  6,  12, 19, 25, 31, 37, 43, 49,  54,  60,  65,  71,  76,  81,  85, 90,
                                     94, 98, 102, 106, 109, 112, 115, 117, 120, 122, 123, 125, 126, 126, 127, 127};

static int16_t amp_tbl[724]; /* 0.1 dB units -723..0 -> Q15 linear gain */
static int tables_ready;

static void init_tables(void) {
    if (tables_ready) return;
    amp_tbl[0] = 0;
    for (int i = 1; i < 724; i++) amp_tbl[i] = (int16_t)(32767.0 * pow(10.0, (i - 723) / 200.0) + 0.5);
    tables_ready = 1;
}

static int scale(int v) {
    if (v < 0) v = 0;
    if (v > 127) v = 127;
    return scale_tbl[v];
}

static int cnv_attack(int a) {
    if (a & 0x80) a = 0x7F;
    return a >= 0x6D ? attack_tbl[0x7F - a] : 0xFF - a;
}

static int cnv_fall(int f) {
    if (f & 0x80) f = 0x7F;
    if (f == 0x7F) return 0xFFFF;
    if (f == 0x7E) return 0x3C00;
    if (f < 0x32) return f * 2 + 1;
    return 0x1E00 / (0x7E - f);
}

static int sine(uint16_t phase) {
    int i = phase >> 9; /* 0..127 */
    int q = i & 31;
    switch (i >> 5) {
    case 0: return sine_tbl[q];
    case 1: return sine_tbl[32 - q];
    case 2: return -sine_tbl[q];
    default: return -sine_tbl[32 - q];
    }
}

/* ---- data ---------------------------------------------------------------------------- */

typedef struct {
    uint8_t type; /* 1 PCM, 2 PSG, 3 noise */
    uint16_t swav, swar;
    uint8_t key, a, d, s, r, pan;
} NoteDef;

typedef struct {
    uint8_t active, kind, state;
    uint8_t track;
    uint8_t key, base_key, vel, prio;
    uint8_t inst_pan;
    const Wave *wave;
    float base_rate;
    uint64_t pos, step; /* 32.32 */
    uint8_t duty;
    uint16_t lfsr;
    int16_t noise_out;
    int32_t ampl;
    int32_t attack_lvl, decay_rate, sustain_lvl, release_rate;
    int32_t length; /* ticks left; 0 = until released */
    int32_t sweep_pitch, sweep_len, sweep_cnt;
    uint16_t mod_delay_cnt, mod_phase;
    int32_t gain_l, gain_r; /* Q15 */
} Chan;

typedef struct {
    uint32_t pos;
    uint8_t count, type;
} StackEnt;

typedef struct {
    uint8_t active;
    uint32_t pos;
    int32_t wait;
    uint8_t notewait, tie, porta, cmp;
    uint8_t ovr, if_pending;
    int tie_ch;
    uint16_t program;
    uint8_t vol, expr, pan, bend_range, prio;
    int8_t transpose, bend;
    uint8_t porta_key, porta_time;
    int16_t sweep_pitch;
    uint8_t att, dec, sus, rel; /* 0xFF = instrument default */
    uint8_t mod_depth, mod_speed, mod_type, mod_range;
    uint16_t mod_delay;
    StackEnt stack[STACK_DEPTH];
    uint8_t sp;
    int loops;
} Track;

struct SseqPlayer {
    const uint8_t *seq;
    uint32_t seq_size, data_off;
    const uint8_t *bank;
    uint32_t bank_size;
    Wave *waves[SDAT_MAX_WAVEARCS];
    uint32_t nwaves[SDAT_MAX_WAVEARCS];

    Track trk[NUM_TRK];
    Chan ch[NUM_CH];
    uint16_t tempo;
    int32_t tempo_acc;
    uint8_t master_vol, seq_vol;
    uint16_t chan_mask;
    int16_t vars[32];
    uint32_t rng;
    int loops;

    uint32_t out_rate;
    uint64_t clk_acc, clk_per_frame; /* in ARM7 cycles * out_rate */
    int32_t *mix;
    uint32_t mix_cap;
};

/* ---- instruments ------------------------------------------------------------------- */

static int read_def(const uint8_t *b, uint32_t sz, uint32_t off, uint8_t type, NoteDef *d) {
    if (type < 1 || type > 3 || off > sz || sz - off < 10) return 0;
    const uint8_t *p = b + off;
    d->type = type;
    d->swav = rd16(p);
    d->swar = rd16(p + 2);
    d->key = p[4];
    d->a = p[5];
    d->d = p[6];
    d->s = p[7];
    d->r = p[8];
    d->pan = p[9];
    return 1;
}

static int bank_lookup(const uint8_t *b, uint32_t sz, uint32_t prog, int key, NoteDef *d) {
    if (sz < 0x3C) return 0;
    uint32_t n = rd32(b + 0x38);
    if (prog >= n || 0x3C + prog * 4 + 4 > sz) return 0;
    const uint8_t *rec = b + 0x3C + prog * 4;
    uint8_t type = rec[0];
    uint32_t off = rd16(rec + 1);
    if (type == 0) return 0;
    if (type < 16) return read_def(b, sz, off, type, d);
    if (off > sz) return 0;
    if (type == 16) { /* drum set: u8 low, u8 high, then 12-byte entries */
        if (sz - off < 2) return 0;
        int lo = b[off], hi = b[off + 1];
        if (key < lo || key > hi) return 0;
        uint32_t e = off + 2 + (uint32_t)(key - lo) * 12;
        if (e > sz || sz - e < 12) return 0;
        return read_def(b, sz, e + 2, b[e], d);
    }
    if (type == 17) { /* key split: 8 upper-key bytes, then 12-byte entries per region */
        if (sz - off < 8) return 0;
        for (int i = 0; i < 8; i++) {
            uint8_t top = b[off + i];
            if (top == 0) break;
            if (key <= top) {
                uint32_t e = off + 8 + (uint32_t)i * 12;
                if (e > sz || sz - e < 12) return 0;
                return read_def(b, sz, e + 2, b[e], d);
            }
        }
    }
    return 0;
}

static void collect_waves(SseqPlayer *p, const uint8_t *b, uint32_t sz, uint32_t off, uint8_t type, uint8_t *used[]) {
    NoteDef d;
    if (!read_def(b, sz, off, type, &d) || d.type != 1) return;
    if (d.swar < SDAT_MAX_WAVEARCS && d.swav < p->nwaves[d.swar]) used[d.swar][d.swav] = 1;
}

/* Decode only the samples the bank actually references. */
static int load_waves(SseqPlayer *p, const SdatSeqBundle *bun) {
    uint8_t *used[SDAT_MAX_WAVEARCS] = {0};
    int ok = 0;

    for (int k = 0; k < SDAT_MAX_WAVEARCS; k++) {
        p->nwaves[k] = bun->swar[k] ? swar_count(bun->swar[k], bun->swar_size[k]) : 0;
        p->waves[k] = (Wave *)calloc(p->nwaves[k] ? p->nwaves[k] : 1, sizeof(Wave));
        used[k] = (uint8_t *)calloc(p->nwaves[k] ? p->nwaves[k] : 1, 1);
        if (!p->waves[k] || !used[k]) goto done;
    }

    const uint8_t *b = p->bank;
    uint32_t sz = p->bank_size;
    uint32_t n = sz >= 0x3C ? rd32(b + 0x38) : 0;
    for (uint32_t i = 0; i < n && 0x3C + i * 4 + 4 <= sz; i++) {
        const uint8_t *rec = b + 0x3C + i * 4;
        uint8_t type = rec[0];
        uint32_t off = rd16(rec + 1);
        if (type >= 1 && type < 16) {
            collect_waves(p, b, sz, off, type, used);
        } else if (type == 16 && off + 2 <= sz) {
            int lo = b[off], hi = b[off + 1];
            for (int k = lo; k <= hi; k++) {
                uint32_t e = off + 2 + (uint32_t)(k - lo) * 12;
                if (e + 12 > sz) break;
                collect_waves(p, b, sz, e + 2, b[e], used);
            }
        } else if (type == 17 && off + 8 <= sz) {
            for (int k = 0; k < 8 && b[off + k]; k++) {
                uint32_t e = off + 8 + (uint32_t)k * 12;
                if (e + 12 > sz) break;
                collect_waves(p, b, sz, e + 2, b[e], used);
            }
        }
    }

    for (int k = 0; k < SDAT_MAX_WAVEARCS; k++)
        for (uint32_t i = 0; i < p->nwaves[k]; i++)
            if (used[k][i]) swar_decode(bun->swar[k], bun->swar_size[k], i, &p->waves[k][i]); /* failures stay silent */
    ok = 1;

done:
    for (int k = 0; k < SDAT_MAX_WAVEARCS; k++) free(used[k]);
    return ok;
}

/* ---- channels ---------------------------------------------------------------------- */

static void chan_release(Chan *c) {
    if (!c->active) return;
    c->state = ST_RELEASE;
    c->prio = 1;
}

static int alloc_chan(SseqPlayer *p, int kind, int prio) {
    static const uint8_t pcm_order[NUM_CH] = {0, 1, 2, 3, 4, 5, 6, 7, 14, 15, 8, 9, 10, 11, 12, 13};
    static const uint8_t psg_order[6] = {8, 9, 10, 11, 12, 13};
    static const uint8_t noise_order[2] = {14, 15};
    const uint8_t *order;
    int n;
    switch (kind) {
    case K_PSG: order = psg_order; n = 6; break;
    case K_NOISE: order = noise_order; n = 2; break;
    default: order = pcm_order; n = NUM_CH; break;
    }

    int best = -1;
    for (int i = 0; i < n; i++) {
        int c = order[i];
        if (!(p->chan_mask & (1u << c))) continue;
        if (!p->ch[c].active) return c;
        if (best < 0 || p->ch[c].prio < p->ch[best].prio ||
            (p->ch[c].prio == p->ch[best].prio && p->ch[c].ampl < p->ch[best].ampl))
            best = c;
    }
    if (best >= 0 && p->ch[best].prio > prio) return -1;
    return best;
}

static void setup_sweep(Track *t, Chan *c, int key, int32_t dur) {
    int32_t sp = t->sweep_pitch;
    if (t->porta) sp += (t->porta_key - key) * 64;
    c->sweep_pitch = sp;
    c->sweep_cnt = 0;
    if (t->porta_time == 0)
        c->sweep_len = dur;
    else
        c->sweep_len = (int32_t)(((int64_t)t->porta_time * t->porta_time * (sp < 0 ? -sp : sp)) >> 11);
    t->porta_key = (uint8_t)key;
}

static void note_on(SseqPlayer *p, int ti, int key, int vel, int32_t dur) {
    Track *t = &p->trk[ti];
    key += t->transpose;
    if (key < 0) key = 0;
    if (key > 127) key = 127;

    /* Tie mode: glide the existing voice to the new key without retriggering. */
    if (t->tie && t->tie_ch >= 0) {
        Chan *c = &p->ch[t->tie_ch];
        if (c->active && c->track == ti && c->state != ST_RELEASE) {
            setup_sweep(t, c, key, dur);
            c->key = (uint8_t)key;
            c->vel = (uint8_t)vel;
            return;
        }
    }

    NoteDef d;
    if (!bank_lookup(p->bank, p->bank_size, t->program, key, &d)) return;
    int kind = d.type == 2 ? K_PSG : d.type == 3 ? K_NOISE : K_PCM;
    const Wave *w = NULL;
    if (kind == K_PCM) {
        if (d.swar >= SDAT_MAX_WAVEARCS || d.swav >= p->nwaves[d.swar]) return;
        w = &p->waves[d.swar][d.swav];
        if (!w->data) return;
    }

    int ci = alloc_chan(p, kind, t->prio);
    if (ci < 0) return;
    Chan *c = &p->ch[ci];
    memset(c, 0, sizeof *c);
    c->active = 1;
    c->kind = (uint8_t)kind;
    c->track = (uint8_t)ti;
    c->key = (uint8_t)key;
    c->base_key = d.key;
    c->vel = (uint8_t)vel;
    c->prio = t->prio;
    c->inst_pan = d.pan;
    c->wave = w;
    c->base_rate = w ? w->rate : PSG_BASE_RATE;
    c->duty = (uint8_t)(d.swav & 7);
    c->lfsr = 0x7FFF;
    c->noise_out = 0x7FFF;

    c->attack_lvl = cnv_attack(t->att != 0xFF ? t->att : d.a);
    c->decay_rate = cnv_fall(t->dec != 0xFF ? t->dec : d.d);
    c->sustain_lvl = scale(t->sus != 0xFF ? t->sus : d.s) * 128;
    c->release_rate = cnv_fall(t->rel != 0xFF ? t->rel : d.r);
    c->ampl = AMPL_MIN;
    c->state = ST_ATTACK;
    c->length = t->tie ? 0 : dur;

    setup_sweep(t, c, key, dur);
    if (t->tie) t->tie_ch = ci;
}

/* Per-frame: envelope, LFO, pitch, volume and pan. */
static void chan_update(SseqPlayer *p, Chan *c) {
    switch (c->state) {
    case ST_ATTACK:
        c->ampl = (c->attack_lvl * c->ampl) / 255;
        if (c->ampl == 0) c->state = ST_DECAY;
        break;
    case ST_DECAY:
        c->ampl -= c->decay_rate;
        if (c->ampl <= c->sustain_lvl) {
            c->ampl = c->sustain_lvl;
            c->state = ST_SUSTAIN;
        }
        break;
    case ST_SUSTAIN: break;
    case ST_RELEASE:
        c->ampl -= c->release_rate;
        if (c->ampl <= AMPL_MIN) {
            c->active = 0;
            return;
        }
        break;
    }

    const Track *t = &p->trk[c->track];
    int32_t mod = 0;
    if (t->mod_depth) {
        if (c->mod_delay_cnt < t->mod_delay) {
            c->mod_delay_cnt++;
        } else {
            c->mod_phase = (uint16_t)(c->mod_phase + (t->mod_speed << 7));
            mod = sine(c->mod_phase) * t->mod_depth * t->mod_range; /* scaled per target below */
        }
    }

    int32_t pitch = (c->key - c->base_key) * 64 + ((t->bend * t->bend_range * 64) >> 7);
    if (c->sweep_len > 0 && c->sweep_cnt < c->sweep_len)
        pitch += (int32_t)((int64_t)c->sweep_pitch * (c->sweep_len - c->sweep_cnt) / c->sweep_len);
    if (t->mod_type == 0) pitch += mod >> 8;
    float freq = c->base_rate * exp2f((float)pitch / 768.0f);
    float stepf = freq / (float)p->out_rate;
    if (stepf > 64.0f) stepf = 64.0f;
    c->step = (uint64_t)(stepf * 4294967296.0f);

    int32_t vol = scale(p->seq_vol) + scale(p->master_vol) + scale(t->vol) + scale(t->expr) + scale(c->vel) +
                  (c->ampl >> 7);
    if (t->mod_type == 1) vol += (int32_t)(((int64_t)mod * 60) >> 14);
    if (vol < -723) vol = -723;
    if (vol > 0) vol = 0;
    int32_t amp = amp_tbl[vol + 723];

    int32_t pan = t->pan + c->inst_pan - 64;
    if (t->mod_type == 2) pan += mod >> 8;
    if (pan < 0) pan = 0;
    if (pan > 127) pan = 127;
    c->gain_l = (amp * (127 - pan)) >> 7;
    c->gain_r = (amp * pan) >> 7;
}

/* ---- tracks ------------------------------------------------------------------------ */

static void track_init(Track *t, uint32_t pos) {
    memset(t, 0, sizeof *t);
    t->active = 1;
    t->pos = pos;
    t->notewait = 1;
    t->tie_ch = -1;
    t->vol = 127;
    t->expr = 127;
    t->pan = 64;
    t->bend_range = 2;
    t->prio = 64;
    t->porta_key = 60;
    t->att = t->dec = t->sus = t->rel = 0xFF;
    t->mod_speed = 16;
    t->mod_range = 1;
}

static void track_end(SseqPlayer *p, Track *t) {
    t->active = 0;
    if (t->tie_ch >= 0) chan_release(&p->ch[t->tie_ch]);
    t->tie_ch = -1;
}

static int rd8(SseqPlayer *p, Track *t) {
    if (t->pos >= p->seq_size) return -1;
    return p->seq[t->pos++];
}

static int32_t rd_n(SseqPlayer *p, Track *t, int n) {
    if (p->seq_size - t->pos < (uint32_t)n || t->pos > p->seq_size) {
        t->pos = p->seq_size;
        return 0;
    }
    int32_t v = 0;
    for (int i = 0; i < n; i++) v |= (int32_t)p->seq[t->pos++] << (8 * i);
    return v;
}

static int32_t rd_vl(SseqPlayer *p, Track *t) {
    int32_t v = 0;
    for (int i = 0; i < 4; i++) {
        int b = rd8(p, t);
        if (b < 0) return v;
        v = (v << 7) | (b & 0x7F);
        if (!(b & 0x80)) break;
    }
    return v;
}

static int32_t rnd(SseqPlayer *p) {
    p->rng = p->rng * 1664525u + 1013904223u;
    return (int32_t)(p->rng >> 16); /* 0..65535 */
}

/* Reads a command's final argument, honouring a random/variable prefix. */
static int32_t read_arg(SseqPlayer *p, Track *t, int type) {
    int ovr = t->ovr;
    t->ovr = OVR_NONE;
    if (ovr == OVR_RANDOM) {
        int32_t lo = (int16_t)rd_n(p, t, 2);
        int32_t hi = (int16_t)rd_n(p, t, 2);
        if (hi < lo) {
            int32_t x = lo;
            lo = hi;
            hi = x;
        }
        return lo + rnd(p) % (hi - lo + 1);
    }
    if (ovr == OVR_VAR) {
        int v = rd8(p, t);
        return (v >= 0 && v < 32) ? p->vars[v] : 0;
    }
    switch (type) {
    case ARG_U8: return rd_n(p, t, 1);
    case ARG_S8: return (int8_t)rd_n(p, t, 1);
    case ARG_U16: return rd_n(p, t, 2);
    case ARG_S16: return (int16_t)rd_n(p, t, 2);
    default: return rd_vl(p, t);
    }
}

static uint8_t clamp_u7(int32_t v) { return (uint8_t)(v < 0 ? 0 : v > 127 ? 127 : v); }
static int8_t clamp_s8(int32_t v) { return (int8_t)(v < -128 ? -128 : v > 127 ? 127 : v); }

static void var_op(SseqPlayer *p, Track *t, int op, int idx, int32_t val) {
    if (idx < 0 || idx >= 32) return;
    int16_t *v = &p->vars[idx];
    switch (op) {
    case 0xB0: *v = (int16_t)val; break;
    case 0xB1: *v = (int16_t)(*v + val); break;
    case 0xB2: *v = (int16_t)(*v - val); break;
    case 0xB3: *v = (int16_t)(*v * val); break;
    case 0xB4: if (val) *v = (int16_t)(*v / val); break;
    case 0xB5: *v = (int16_t)(val >= 0 ? *v << (val & 15) : *v >> ((-val) & 15)); break;
    case 0xB6: {
        int32_t r = val ? rnd(p) % ((val < 0 ? -val : val) + 1) : 0;
        *v = (int16_t)(val < 0 ? -r : r);
        break;
    }
    case 0xB8: t->cmp = *v == val; break;
    case 0xB9: t->cmp = *v >= val; break;
    case 0xBA: t->cmp = *v > val; break;
    case 0xBB: t->cmp = *v <= val; break;
    case 0xBC: t->cmp = *v < val; break;
    case 0xBD: t->cmp = *v != val; break;
    }
}

static void exec_cmd(SseqPlayer *p, int ti) {
    Track *t = &p->trk[ti];
    uint32_t cmd_pos = t->pos;
    int cmd = rd8(p, t);
    int skip = 0;

    for (;;) { /* prefixes */
        if (cmd == 0xA0) t->ovr = OVR_RANDOM;
        else if (cmd == 0xA1) t->ovr = OVR_VAR;
        else if (cmd == 0xA2) t->if_pending = 1;
        else break;
        cmd = rd8(p, t);
    }
    if (t->if_pending) {
        skip = !t->cmp;
        t->if_pending = 0;
    }
    if (cmd < 0) {
        track_end(p, t);
        return;
    }

    if (cmd < 0x80) {
        int vel = rd8(p, t);
        int32_t dur = read_arg(p, t, ARG_VL);
        if (dur < 0) dur = 0;
        if (skip) return;
        note_on(p, ti, cmd, vel < 0 ? 0 : vel & 0x7F, dur);
        if (t->notewait) t->wait = dur;
        return;
    }

    int32_t a, b;
    switch (cmd) {
    case 0x80: a = read_arg(p, t, ARG_VL); if (!skip) t->wait = a < 0 ? 0 : a; break;
    case 0x81: a = read_arg(p, t, ARG_VL); if (!skip) t->program = (uint16_t)a; break;
    case 0x93:
        a = rd_n(p, t, 1);
        b = rd_n(p, t, 3);
        if (!skip && a > 0 && a < NUM_TRK && !p->trk[a].active) track_init(&p->trk[a], p->data_off + (uint32_t)b);
        break;
    case 0x94:
        a = rd_n(p, t, 3);
        if (skip) break;
        if (p->data_off + (uint32_t)a <= cmd_pos) {
            t->loops++;
            if (t->loops > p->loops) p->loops = t->loops;
        }
        t->pos = p->data_off + (uint32_t)a;
        break;
    case 0x95:
        a = rd_n(p, t, 3);
        if (skip || t->sp >= STACK_DEPTH) break;
        t->stack[t->sp].pos = t->pos;
        t->stack[t->sp].type = STK_CALL;
        t->sp++;
        t->pos = p->data_off + (uint32_t)a;
        break;
    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6:
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD:
        a = rd_n(p, t, 1);
        b = read_arg(p, t, ARG_S16);
        if (!skip) var_op(p, t, cmd, a, b);
        break;

    case 0xC3: a = read_arg(p, t, ARG_S8); if (!skip) t->transpose = clamp_s8(a); break;
    case 0xC4: a = read_arg(p, t, ARG_S8); if (!skip) t->bend = clamp_s8(a); break;
    case 0xC0: case 0xC1: case 0xC2: case 0xC5: case 0xC6: case 0xC7: case 0xC8: case 0xC9:
    case 0xCA: case 0xCB: case 0xCC: case 0xCD: case 0xCE: case 0xCF:
    case 0xD0: case 0xD1: case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6:
        a = read_arg(p, t, ARG_U8);
        if (skip) break;
        switch (cmd) {
        case 0xC0: t->pan = clamp_u7(a); break;
        case 0xC1: t->vol = clamp_u7(a); break;
        case 0xC2: p->master_vol = clamp_u7(a); break;
        case 0xC5: t->bend_range = (uint8_t)a; break;
        case 0xC6: t->prio = (uint8_t)a; break;
        case 0xC7: t->notewait = a != 0; break;
        case 0xC8:
            t->tie = a != 0;
            if (t->tie_ch >= 0) chan_release(&p->ch[t->tie_ch]);
            t->tie_ch = -1;
            break;
        case 0xC9: t->porta_key = clamp_u7(a + t->transpose); t->porta = 1; break;
        case 0xCA: t->mod_depth = (uint8_t)a; break;
        case 0xCB: t->mod_speed = (uint8_t)a; break;
        case 0xCC: t->mod_type = (uint8_t)a; break;
        case 0xCD: t->mod_range = (uint8_t)a; break;
        case 0xCE: t->porta = a != 0; break;
        case 0xCF: t->porta_time = (uint8_t)a; break;
        case 0xD0: t->att = (uint8_t)a; break;
        case 0xD1: t->dec = (uint8_t)a; break;
        case 0xD2: t->sus = (uint8_t)a; break;
        case 0xD3: t->rel = (uint8_t)a; break;
        case 0xD4:
            if (t->sp < STACK_DEPTH) {
                t->stack[t->sp].pos = t->pos;
                t->stack[t->sp].count = (uint8_t)a;
                t->stack[t->sp].type = STK_LOOP;
                t->sp++;
            }
            break;
        case 0xD5: t->expr = clamp_u7(a); break;
        case 0xD6: break; /* print variable (debug) */
        }
        break;

    case 0xE0: a = read_arg(p, t, ARG_U16); if (!skip) t->mod_delay = (uint16_t)a; break;
    case 0xE1: a = read_arg(p, t, ARG_U16); if (!skip) p->tempo = (uint16_t)a; break;
    case 0xE3: a = read_arg(p, t, ARG_S16); if (!skip) t->sweep_pitch = (int16_t)a; break;

    case 0xFC: /* loop end */
        if (skip || t->sp == 0 || t->stack[t->sp - 1].type != STK_LOOP) break;
        {
            StackEnt *e = &t->stack[t->sp - 1];
            if (e->count == 0) { /* infinite loop */
                t->loops++;
                if (t->loops > p->loops) p->loops = t->loops;
                t->pos = e->pos;
            } else if (--e->count > 0) {
                t->pos = e->pos;
            } else {
                t->sp--;
            }
        }
        break;
    case 0xFD: /* return */
        if (skip) break;
        while (t->sp > 0 && t->stack[t->sp - 1].type != STK_CALL) t->sp--;
        if (t->sp > 0) t->pos = t->stack[--t->sp].pos;
        else track_end(p, t);
        break;
    case 0xFE: rd_n(p, t, 2); break; /* track allocation mask: tracks are opened on demand */
    case 0xFF: if (!skip) track_end(p, t); break;
    default: track_end(p, t); break; /* unknown command: argument size unknown, stop safely */
    }
}

static void seq_tick(SseqPlayer *p) {
    for (int i = 0; i < NUM_CH; i++) {
        Chan *c = &p->ch[i];
        if (!c->active) continue;
        if (c->length > 0 && --c->length == 0) chan_release(c);
        if (c->sweep_cnt < c->sweep_len) c->sweep_cnt++;
    }
    for (int ti = 0; ti < NUM_TRK; ti++) {
        Track *t = &p->trk[ti];
        if (!t->active) continue;
        if (t->wait > 0 && --t->wait > 0) continue;
        int guard = 0;
        while (t->active && t->wait == 0) {
            if (++guard > MAX_CMDS_PER_TICK) {
                track_end(p, t);
                break;
            }
            exec_cmd(p, ti);
        }
    }
}

static void seq_frame(SseqPlayer *p) {
    p->tempo_acc += p->tempo;
    while (p->tempo_acc >= 240) {
        p->tempo_acc -= 240;
        seq_tick(p);
    }
    for (int i = 0; i < NUM_CH; i++)
        if (p->ch[i].active) chan_update(p, &p->ch[i]);
}

/* ---- mixing ------------------------------------------------------------------------ */

static void mix_chan(Chan *c, int32_t *mix, uint32_t n) {
    int32_t gl = c->gain_l, gr = c->gain_r;
    if (c->kind == K_PCM) {
        const Wave *w = c->wave;
        uint64_t end = (uint64_t)w->len << 32;
        uint64_t loop_len = (uint64_t)(w->len - w->loop_start) << 32;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t idx = (uint32_t)(c->pos >> 32);
            /* 15-bit fraction: |s1 - s0| * frac must fit in int32 even for full-scale jumps. */
            int32_t frac = (int32_t)((c->pos >> 17) & 0x7FFF);
            int32_t s0 = w->data[idx], s1 = w->data[idx + 1];
            int32_t s = s0 + (((s1 - s0) * frac) >> 15);
            mix[i * 2] += (s * gl) >> 15;
            mix[i * 2 + 1] += (s * gr) >> 15;
            c->pos += c->step;
            if (c->pos >= end) {
                if (!w->loops || loop_len == 0) {
                    c->active = 0;
                    return;
                }
                while (c->pos >= end) c->pos -= loop_len;
            }
        }
    } else if (c->kind == K_PSG) {
        if (c->duty == 7) return; /* 0% duty: silent */
        /* Average the square wave over each output sample (box filter) so high notes
         * don't alias into a harsh buzz. Positions are in duty steps, 8 per cycle. */
        const uint64_t period = 8ull << 32;
        float high = (float)(c->duty + 1);
        float step = (float)c->step * (1.0f / 4294967296.0f);
        float inv_step = 1.0f / step;
        for (uint32_t i = 0; i < n; i++) {
            float x0 = (float)c->pos * (1.0f / 4294967296.0f);
            float x1 = x0 + step;
            float cycles = floorf(x1 * 0.125f);
            float r1 = x1 - cycles * 8.0f;
            float hi = cycles * high + (r1 < high ? r1 : high) - (x0 < high ? x0 : high);
            int32_t s = (int32_t)((2.0f * hi * inv_step - 1.0f) * 32767.0f);
            mix[i * 2] += (s * gl) >> 15;
            mix[i * 2 + 1] += (s * gr) >> 15;
            c->pos = (c->pos + c->step) & (period - 1); /* period is 2^35 */
        }
    } else {
        for (uint32_t i = 0; i < n; i++) {
            uint64_t next = c->pos + c->step;
            uint32_t ticks = (uint32_t)((next >> 32) - (c->pos >> 32));
            while (ticks--) {
                if (c->lfsr & 1) {
                    c->lfsr = (uint16_t)((c->lfsr >> 1) ^ 0x6000);
                    c->noise_out = -0x7FFF;
                } else {
                    c->lfsr >>= 1;
                    c->noise_out = 0x7FFF;
                }
            }
            c->pos = next;
            mix[i * 2] += (c->noise_out * gl) >> 15;
            mix[i * 2 + 1] += (c->noise_out * gr) >> 15;
        }
    }
}

static inline int16_t soft_clip(int32_t s) {
    const int32_t room = 32767 - SOFT_KNEE;
    if (s > SOFT_KNEE) {
        int32_t x = s - SOFT_KNEE;
        s = SOFT_KNEE + (int32_t)((int64_t)room * x / (x + room));
    } else if (s < -SOFT_KNEE) {
        int32_t x = -s - SOFT_KNEE;
        s = -SOFT_KNEE - (int32_t)((int64_t)room * x / (x + room));
    }
    return (int16_t)s;
}

void sseq_render(SseqPlayer *p, int16_t *out, uint32_t frames) {
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
        for (int i = 0; i < NUM_CH; i++)
            if (p->ch[i].active) mix_chan(&p->ch[i], p->mix, n);
        for (uint32_t i = 0; i < n * 2; i++) {
            out[i] = soft_clip(p->mix[i]);
        }
        out += n * 2;
        frames -= n;
        p->clk_acc += (uint64_t)n * ARM7_CLOCK;
    }
}

/* ---- lifecycle --------------------------------------------------------------------- */

SseqPlayer *sseq_create(const SdatSeqBundle *b, uint32_t out_rate) {
    if (!b->sseq || b->sseq_size < 0x1C || !b->sbnk || out_rate == 0) return NULL;
    uint32_t data_off = rd32(b->sseq + 0x18);
    if (data_off >= b->sseq_size) return NULL;

    init_tables();
    SseqPlayer *p = (SseqPlayer *)calloc(1, sizeof *p);
    if (!p) return NULL;
    p->seq = b->sseq;
    p->seq_size = b->sseq_size;
    p->data_off = data_off;
    p->bank = b->sbnk;
    p->bank_size = b->sbnk_size;
    p->out_rate = out_rate;
    p->clk_per_frame = (uint64_t)FRAME_CYCLES * out_rate;
    p->clk_acc = p->clk_per_frame; /* run the first frame immediately */
    p->mix_cap = 1024;
    p->mix = (int32_t *)malloc(p->mix_cap * 2 * sizeof(int32_t));
    if (!p->mix || !load_waves(p, b)) {
        sseq_destroy(p);
        return NULL;
    }

    p->tempo = 120;
    p->master_vol = 127;
    p->seq_vol = b->info.vol;
    p->chan_mask = (b->player.present && b->player.channel_mask) ? b->player.channel_mask : 0xFFFF;
    p->rng = 0x12345678u;
    for (int i = 0; i < 32; i++) p->vars[i] = -1;
    track_init(&p->trk[0], data_off);
    return p;
}

void sseq_destroy(SseqPlayer *p) {
    if (!p) return;
    for (int k = 0; k < SDAT_MAX_WAVEARCS; k++) {
        if (p->waves[k])
            for (uint32_t i = 0; i < p->nwaves[k]; i++) wave_free(&p->waves[k][i]);
        free(p->waves[k]);
    }
    free(p->mix);
    free(p);
}

int sseq_finished(const SseqPlayer *p) {
    for (int i = 0; i < NUM_TRK; i++)
        if (p->trk[i].active) return 0;
    for (int i = 0; i < NUM_CH; i++)
        if (p->ch[i].active) return 0;
    return 1;
}

int sseq_loops(const SseqPlayer *p) { return p->loops; }

uint16_t sseq_voice_mask(const SseqPlayer *p) {
    uint16_t m = 0;
    for (int i = 0; i < NUM_CH; i++)
        if (p->ch[i].active && p->ch[i].ampl > AMPL_MIN / 2) m |= (uint16_t)(1u << i);
    return m;
}
