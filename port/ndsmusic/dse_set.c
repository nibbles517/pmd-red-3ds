/*
 * From the NDS Music Player (ndsmusicplayer, source/core), for the port's Music
 * Options. Files are read through the backend (Backend_File*) instead of stdio.
 */
#include "dse_set.h"

/* (no <ctype.h>: its tables belong to the C library the 3DS build links) */
#define isdigit(c) ((c) >= '0' && (c) <= '9')
#define tolower(c) ((c) >= 'A' && (c) <= 'Z' ? (c) + 32 : (c))
#include <stdlib.h>
#include <string.h>

void dse_set_init(DseSet *s, const DataSource *src) {
    memset(s, 0, sizeof *s);
    s->src = *src;
}

static int ieq(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0;
    return 1;
}

/* "/SOUND/BGM/bgm0012.smd" -> group, number (or -1 for the bank "bgm.swd"), is_smd */
static int classify(const char *path, int *group, int *number, int *is_smd) {
    static const char *dirs[DSE_GROUPS] = {"/SOUND/BGM/", "/SOUND/ME/"};
    static const char *prefix[DSE_GROUPS] = {"bgm", "me"};
    for (int g = 0; g < DSE_GROUPS; g++) {
        size_t dl = strlen(dirs[g]), pl = strlen(prefix[g]);
        if (strlen(path) < dl || !ieq(path, dirs[g], dl)) continue;
        const char *name = path + dl;
        size_t nl = strlen(name);
        if (strchr(name, '/') || nl < pl + 4 || !ieq(name, prefix[g], pl)) return 0;
        const char *ext = name + nl - 4;
        if (ieq(ext, ".smd", 4)) *is_smd = 1;
        else if (ieq(ext, ".swd", 4)) *is_smd = 0;
        else return 0;
        const char *digits = name + pl;
        int len = (int)(ext - digits);
        if (len == 0) {
            if (*is_smd) return 0;
            *number = -1; /* shared bank */
        } else {
            if (len > 5) return 0;
            int v = 0;
            for (int i = 0; i < len; i++) {
                if (!isdigit((unsigned char)digits[i])) return 0;
                v = v * 10 + (digits[i] - '0');
            }
            *number = v;
        }
        *group = g;
        return 1;
    }
    return 0;
}

int dse_set_add_file(DseSet *s, const char *path, uint32_t off, uint32_t size) {
    int group, number, is_smd;
    if (!classify(path, &group, &number, &is_smd) || size == 0) return 0;
    if (number < 0) {
        s->bank_off[group] = off;
        s->bank_len[group] = size;
        s->has_bank[group] = 1;
        return 1;
    }
    if (s->ncand == s->cand_cap) {
        int cap = s->cand_cap ? s->cand_cap * 2 : 256;
        DseCandidate *n = (DseCandidate *)realloc(s->cand, (size_t)cap * sizeof *n);
        if (!n) return 0;
        s->cand = n;
        s->cand_cap = cap;
    }
    DseCandidate *c = &s->cand[s->ncand++];
    c->number = (uint32_t)number;
    c->off = off;
    c->len = size;
    c->group = (uint8_t)group;
    c->is_smd = (uint8_t)is_smd;
    return 1;
}

static int song_cmp(const void *a, const void *b) {
    const DseSong *x = (const DseSong *)a, *y = (const DseSong *)b;
    if (x->group != y->group) return x->group - y->group;
    return (int)x->number - (int)y->number;
}

int dse_set_finish(DseSet *s) {
    int nsmd = 0;
    for (int i = 0; i < s->ncand; i++) nsmd += s->cand[i].is_smd;
    s->songs = (DseSong *)calloc(nsmd ? (size_t)nsmd : 1, sizeof(DseSong));
    if (!s->songs) return 0;
    for (int i = 0; i < s->ncand; i++) {
        const DseCandidate *c = &s->cand[i];
        if (!c->is_smd) continue;
        uint8_t hdr[0x80];
        if (c->len < sizeof hdr || ds_read(&s->src, c->off, hdr, sizeof hdr) != 0 || !dse_is_smdl(hdr, sizeof hdr))
            continue; /* blanked out (common in 2SF rips) or not a song */
        DseSong *g = &s->songs[s->num];
        memset(g, 0, sizeof *g);
        g->number = (uint16_t)c->number;
        g->group = c->group;
        g->smd_off = c->off;
        g->smd_len = c->len;
        for (int k = 0; k < s->ncand; k++) {
            const DseCandidate *w = &s->cand[k];
            if (!w->is_smd && w->group == c->group && w->number == c->number) {
                g->swd_off = w->off;
                g->swd_len = w->len;
            }
        }
        if (!g->swd_len && s->has_bank[c->group]) { /* jingles share one bank for programs too */
            g->swd_off = s->bank_off[c->group];
            g->swd_len = s->bank_len[c->group];
        }
        if (!g->swd_len) continue;
        dse_smd_label(hdr, sizeof hdr, g->label);
        s->num++;
    }
    qsort(s->songs, (size_t)s->num, sizeof(DseSong), song_cmp);
    free(s->cand);
    s->cand = NULL;
    s->ncand = s->cand_cap = 0;
    return s->num;
}

void dse_set_free(DseSet *s) {
    for (int g = 0; g < DSE_GROUPS; g++) dse_bank_close(s->bank[g]);
    free(s->songs);
    free(s->cand);
    memset(s, 0, sizeof *s);
}

int dse_set_find(const DseSet *s, int group, uint32_t number) {
    for (int i = 0; i < s->num; i++)
        if (s->songs[i].group == group && s->songs[i].number == number) return i;
    return -1;
}

DsePlayer *dse_set_open(DseSet *s, int idx, uint32_t out_rate) {
    if (idx < 0 || idx >= s->num) return NULL;
    const DseSong *g = &s->songs[idx];
    if (!s->bank[g->group] && s->has_bank[g->group]) {
        uint8_t *b = ds_load(&s->src, s->bank_off[g->group], s->bank_len[g->group]);
        if (b) s->bank[g->group] = dse_bank_open(b, s->bank_len[g->group]); /* takes ownership */
    }
    uint8_t *smd = ds_load(&s->src, g->smd_off, g->smd_len);
    uint8_t *swd = ds_load(&s->src, g->swd_off, g->swd_len);
    DsePlayer *p = (smd && swd) ? dse_create(s->bank[g->group], smd, g->smd_len, swd, g->swd_len, out_rate) : NULL;
    free(smd);
    free(swd);
    return p;
}
