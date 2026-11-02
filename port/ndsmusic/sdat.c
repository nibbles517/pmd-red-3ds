/*
 * From the NDS Music Player (ndsmusicplayer, source/core), for the port's Music
 * Options. Files are read through the backend (Backend_File*) instead of stdio.
 */
#include "sdat.h"

#include <stdlib.h>
#include <string.h>

enum { REC_SEQ = 0, REC_SEQARC, REC_BANK, REC_WAVEARC, REC_PLAYER, REC_GROUP, REC_PLAYER2, REC_STRM, REC_COUNT };

/* ---- INFO / SYMB record helpers ------------------------------------------------ */

/* Both blocks: "MAGI" u32 size, then REC_COUNT u32 offsets (relative to block start),
 * each pointing at: u32 count, count * u32 entry offsets (0 = empty slot). */
static int get_record(const uint8_t *blk, uint32_t sz, int type, uint32_t *count, uint32_t *table) {
    *count = 0;
    *table = 0;
    uint32_t rec = rd32(blk + 8 + type * 4);
    if (rec == 0) return 0;
    if (rec > sz - 4) return -1;
    uint32_t n = rd32(blk + rec);
    if (n > (sz - rec - 4) / 4) return -1;
    *count = n;
    *table = rec + 4;
    return 0;
}

static const uint8_t *get_entry(const uint8_t *blk, uint32_t sz, uint32_t table, uint32_t i, uint32_t need) {
    uint32_t o = rd32(blk + table + i * 4);
    if (o == 0 || o > sz || need > sz - o) return NULL;
    return blk + o;
}

static const char *get_string(const uint8_t *blk, uint32_t sz, uint32_t table, uint32_t i) {
    uint32_t o = rd32(blk + table + i * 4);
    if (o == 0 || o >= sz) return NULL;
    if (!memchr(blk + o, 0, sz - o)) return NULL; /* must be NUL-terminated inside the block */
    return (const char *)blk + o;
}

static void *xcalloc(uint32_t n, size_t sz) { return calloc(n ? n : 1, sz); }

static int parse_info(Sdat *s, const uint8_t *in, uint32_t sz) {
    uint32_t n, t;

    if (get_record(in, sz, REC_SEQ, &n, &t)) return -1;
    s->num_seq = n;
    if (!(s->seq = (SdatSeqInfo *)xcalloc(n, sizeof *s->seq))) return -1;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = get_entry(in, sz, t, i, 12);
        if (!e) continue;
        SdatSeqInfo *q = &s->seq[i];
        q->present = 1;
        q->file_id = rd16(e);
        q->bank = rd16(e + 4);
        q->vol = e[6];
        q->cpr = e[7];
        q->ppr = e[8];
        q->ply = e[9];
    }

    if (get_record(in, sz, REC_BANK, &n, &t)) return -1;
    s->num_bank = n;
    if (!(s->bank = (SdatBankInfo *)xcalloc(n, sizeof *s->bank))) return -1;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = get_entry(in, sz, t, i, 12);
        if (!e) continue;
        SdatBankInfo *b = &s->bank[i];
        b->present = 1;
        b->file_id = rd16(e);
        for (int k = 0; k < SDAT_MAX_WAVEARCS; k++) b->wavearc[k] = rd16(e + 4 + k * 2);
    }

    if (get_record(in, sz, REC_WAVEARC, &n, &t)) return -1;
    s->num_wavearc = n;
    if (!(s->wavearc = (SdatWaveArcInfo *)xcalloc(n, sizeof *s->wavearc))) return -1;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = get_entry(in, sz, t, i, 4);
        if (!e) continue;
        s->wavearc[i].present = 1;
        s->wavearc[i].file_id = rd16(e);
    }

    if (get_record(in, sz, REC_PLAYER, &n, &t)) return -1;
    s->num_player = n;
    if (!(s->player = (SdatPlayerInfo *)xcalloc(n, sizeof *s->player))) return -1;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = get_entry(in, sz, t, i, 8);
        if (!e) continue;
        SdatPlayerInfo *p = &s->player[i];
        p->present = 1;
        p->max_seqs = e[0];
        p->channel_mask = rd16(e + 2);
        p->heap_size = rd32(e + 4);
    }

    if (get_record(in, sz, REC_STRM, &n, &t)) return -1;
    s->num_strm = n;
    if (!(s->strm = (SdatStrmInfo *)xcalloc(n, sizeof *s->strm))) return -1;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = get_entry(in, sz, t, i, 12);
        if (!e) continue;
        SdatStrmInfo *q = &s->strm[i];
        q->present = 1;
        q->file_id = rd16(e);
        q->vol = e[4];
        q->pri = e[5];
        q->ply = e[6];
    }
    return 0;
}

/* Names are optional: any problem here just leaves them NULL. */
static void parse_symb(Sdat *s, const uint8_t *sy, uint32_t sz) {
    uint32_t n, t;

    s->seq_name = (const char **)xcalloc(s->num_seq, sizeof *s->seq_name);
    s->strm_name = (const char **)xcalloc(s->num_strm, sizeof *s->strm_name);
    if (!s->seq_name || !s->strm_name) return;

    if (get_record(sy, sz, REC_SEQ, &n, &t) == 0)
        for (uint32_t i = 0; i < n && i < s->num_seq; i++) s->seq_name[i] = get_string(sy, sz, t, i);

    if (get_record(sy, sz, REC_STRM, &n, &t) == 0)
        for (uint32_t i = 0; i < n && i < s->num_strm; i++) s->strm_name[i] = get_string(sy, sz, t, i);
}

/* ---- open / close ---------------------------------------------------------------- */

int sdat_open(Sdat *s, const DataSource *src) {
    uint8_t h[0x40];
    uint8_t *info = NULL, *fat = NULL;
    memset(s, 0, sizeof *s);
    s->src = *src;

    if (ds_read(src, 0, h, sizeof h) != 0 || memcmp(h, "SDAT", 4) != 0) return -1;

    uint32_t symb_off = rd32(h + 0x10), symb_size = rd32(h + 0x14);
    uint32_t info_off = rd32(h + 0x18), info_size = rd32(h + 0x1C);
    uint32_t fat_off = rd32(h + 0x20), fat_size = rd32(h + 0x24);

    /* INFO */
    if (info_size < 8 + REC_COUNT * 4) goto fail;
    if (!(info = ds_load(src, info_off, info_size)) || memcmp(info, "INFO", 4) != 0) goto fail;
    if (parse_info(s, info, info_size) != 0) goto fail;

    /* FAT */
    if (fat_size < 12) goto fail;
    if (!(fat = ds_load(src, fat_off, fat_size)) || memcmp(fat, "FAT ", 4) != 0) goto fail;
    s->num_fat = rd32(fat + 8);
    if (s->num_fat > (fat_size - 12) / 16) goto fail;
    if (!(s->fat = (SdatFatEntry *)xcalloc(s->num_fat, sizeof *s->fat))) goto fail;
    for (uint32_t i = 0; i < s->num_fat; i++) {
        s->fat[i].offset = rd32(fat + 12 + i * 16);
        s->fat[i].size = rd32(fat + 12 + i * 16 + 4);
    }

    /* SYMB (optional) */
    if (symb_off && symb_size >= 8 + REC_COUNT * 4) {
        s->symb_buf = ds_load(src, symb_off, symb_size);
        if (s->symb_buf && memcmp(s->symb_buf, "SYMB", 4) == 0) {
            parse_symb(s, s->symb_buf, symb_size);
        } else {
            free(s->symb_buf);
            s->symb_buf = NULL;
        }
    }

    free(info);
    free(fat);
    return 0;

fail:
    free(info);
    free(fat);
    sdat_close(s);
    return -1;
}

void sdat_close(Sdat *s) {
    free(s->seq);
    free(s->bank);
    free(s->wavearc);
    free(s->player);
    free(s->strm);
    free(s->fat);
    free(s->symb_buf);
    free((void *)s->seq_name);
    free((void *)s->strm_name);
    memset(s, 0, sizeof *s);
}

const char *sdat_seq_name(const Sdat *s, uint32_t i) {
    return (s->seq_name && i < s->num_seq) ? s->seq_name[i] : NULL;
}

const char *sdat_strm_name(const Sdat *s, uint32_t i) {
    return (s->strm_name && i < s->num_strm) ? s->strm_name[i] : NULL;
}

/* ---- loading ----------------------------------------------------------------------- */

static uint8_t *load_file(const Sdat *s, uint16_t file_id, const char *magic, uint32_t *size_out) {
    if (file_id >= s->num_fat) return NULL;
    const SdatFatEntry *e = &s->fat[file_id];
    if (e->size < 16) return NULL;
    uint8_t *buf = ds_load(&s->src, e->offset, e->size);
    if (buf && memcmp(buf, magic, 4) != 0) {
        free(buf);
        return NULL;
    }
    if (buf) *size_out = e->size;
    return buf;
}

void sdat_free_seq(SdatSeqBundle *b) {
    free(b->sseq);
    free(b->sbnk);
    for (int k = 0; k < SDAT_MAX_WAVEARCS; k++) free(b->swar[k]);
    memset(b, 0, sizeof *b);
}

int sdat_load_seq(const Sdat *s, uint32_t seq_index, SdatSeqBundle *out) {
    memset(out, 0, sizeof *out);
    if (seq_index >= s->num_seq || !s->seq[seq_index].present) return -1;

    const SdatSeqInfo *q = &s->seq[seq_index];
    out->seq_index = seq_index;
    out->info = *q;
    if (q->ply < s->num_player) out->player = s->player[q->ply];

    if (q->bank >= s->num_bank || !s->bank[q->bank].present) goto fail;
    const SdatBankInfo *b = &s->bank[q->bank];

    if (!(out->sseq = load_file(s, q->file_id, "SSEQ", &out->sseq_size))) goto fail;
    if (!(out->sbnk = load_file(s, b->file_id, "SBNK", &out->sbnk_size))) goto fail;

    for (int k = 0; k < SDAT_MAX_WAVEARCS; k++) {
        uint16_t wa = b->wavearc[k];
        if (wa == SDAT_NO_WAVEARC) continue;
        if (wa >= s->num_wavearc || !s->wavearc[wa].present) goto fail;
        if (!(out->swar[k] = load_file(s, s->wavearc[wa].file_id, "SWAR", &out->swar_size[k]))) goto fail;
    }
    return 0;

fail:
    sdat_free_seq(out);
    return -1;
}

DataSource sdat_strm_source(const Sdat *s, uint32_t i) {
    DataSource none = s->src;
    none.size = 0;
    if (i >= s->num_strm || !s->strm[i].present) return none;
    uint16_t fid = s->strm[i].file_id;
    if (fid >= s->num_fat) return none;
    const SdatFatEntry *e = &s->fat[fid];
    if (e->offset > s->src.size || e->size > s->src.size - e->offset) return none;
    return ds_sub(&s->src, e->offset, e->size);
}
