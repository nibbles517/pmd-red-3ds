/*
 * From the NDS Music Player (ndsmusicplayer, source/core), for the port's Music
 * Options. Files are read through the backend (Backend_File*) instead of stdio.
 */
/*
 * sdat.h - Nitro SoundData (SDAT) archive parser.
 *
 * Shared by both loaders:
 *   NCSF: decompress program section -> ds_mem(sdat_bytes) -> sdat_open -> sdat_load_seq(reserved seq#)
 *   NDS:  nds_find_sdats -> ds_sub(rom, off, size)        -> sdat_open -> sdat_load_seq(user's pick)
 * Either way the player core receives an SdatSeqBundle.
 */
#ifndef NDSMUSIC_SDAT_H
#define NDSMUSIC_SDAT_H

#include "datasource.h"

#define SDAT_MAX_WAVEARCS 4
#define SDAT_NO_WAVEARC 0xFFFF

typedef struct {
    uint8_t present;
    uint16_t file_id;
    uint16_t bank;
    uint8_t vol;      /* sequence volume, 0-127 */
    uint8_t cpr, ppr; /* channel / player priority */
    uint8_t ply;      /* player index */
} SdatSeqInfo;

typedef struct {
    uint8_t present;
    uint16_t file_id;
    uint16_t wavearc[SDAT_MAX_WAVEARCS]; /* SDAT_NO_WAVEARC = unused slot */
} SdatBankInfo;

typedef struct {
    uint8_t present;
    uint16_t file_id;
} SdatWaveArcInfo;

typedef struct {
    uint8_t present;
    uint8_t max_seqs;
    uint16_t channel_mask; /* 0 means "any channel" in practice */
    uint32_t heap_size;
} SdatPlayerInfo;

typedef struct {
    uint8_t present;
    uint16_t file_id;
    uint8_t vol, pri, ply;
} SdatStrmInfo;

typedef struct {
    uint32_t offset; /* relative to SDAT start */
    uint32_t size;
} SdatFatEntry;

typedef struct {
    DataSource src;

    uint32_t num_seq, num_bank, num_wavearc, num_player, num_strm;
    SdatSeqInfo *seq;
    SdatBankInfo *bank;
    SdatWaveArcInfo *wavearc;
    SdatPlayerInfo *player;
    SdatStrmInfo *strm;

    uint32_t num_fat;
    SdatFatEntry *fat;

    /* Symbol names (NULL entries if stripped). Point into symb_buf. */
    uint8_t *symb_buf;
    const char **seq_name;
    const char **strm_name;
} Sdat;

/* Everything the sequence player needs for one song. Buffers are malloc'd. */
typedef struct {
    uint32_t seq_index;
    SdatSeqInfo info;
    SdatPlayerInfo player; /* present==0 if the SDAT has no player entry */
    uint8_t *sseq;
    uint32_t sseq_size;
    uint8_t *sbnk;
    uint32_t sbnk_size;
    uint8_t *swar[SDAT_MAX_WAVEARCS]; /* NULL for unused slots */
    uint32_t swar_size[SDAT_MAX_WAVEARCS];
} SdatSeqBundle;

/* Keeps a copy of *src; for DS_FILE the FILE* must stay open until sdat_close. */
int sdat_open(Sdat *s, const DataSource *src);
void sdat_close(Sdat *s);

const char *sdat_seq_name(const Sdat *s, uint32_t seq_index);   /* may be NULL */
const char *sdat_strm_name(const Sdat *s, uint32_t strm_index); /* may be NULL */

/* 0 on success. On failure *out is zeroed and nothing needs freeing. */
int sdat_load_seq(const Sdat *s, uint32_t seq_index, SdatSeqBundle *out);
void sdat_free_seq(SdatSeqBundle *b);

/* View of a STRM file for chunked streaming (size 0 if invalid). */
DataSource sdat_strm_source(const Sdat *s, uint32_t strm_index);

#endif
