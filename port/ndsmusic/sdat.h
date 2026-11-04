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
    uint8_t present;
    uint16_t file_id;
} SdatSeqArcInfo;

/* One sound effect (or jingle) inside a sequence archive (SSAR). */
typedef struct {
    uint8_t valid;   /* 0 for empty slots */
    uint32_t offset; /* into the archive's shared sequence data */
    uint16_t bank;
    uint8_t vol, cpr, ppr, ply;
} SdatSsarEntry;

typedef struct {
    uint32_t offset; /* relative to SDAT start */
    uint32_t size;
} SdatFatEntry;

typedef struct {
    DataSource src;

    uint32_t num_seq, num_seqarc, num_bank, num_wavearc, num_player, num_strm;
    SdatSeqInfo *seq;
    SdatSeqArcInfo *seqarc;
    SdatBankInfo *bank;
    SdatWaveArcInfo *wavearc;
    SdatPlayerInfo *player;
    SdatStrmInfo *strm;

    uint32_t num_fat;
    SdatFatEntry *fat;

    /* Symbol names (NULL entries if stripped). Point into symb_buf. */
    uint8_t *symb_buf;
    uint32_t symb_size;
    const char **seq_name;
    const char **strm_name;
    const char **seqarc_name;
    uint32_t *seqarc_sub; /* SYMB offset of each archive's entry-name table (0 = none) */
} Sdat;

/* Everything the sequence player needs for one song. Buffers are malloc'd. */
typedef struct {
    uint32_t seq_index;
    uint32_t data_off;  /* where sequence data starts in sseq (offsets in commands are relative to it) */
    uint32_t start_off; /* where track 0 starts in sseq */
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

/* ---- the file cache (sdat.c): files songs load are kept, with their decoded samples ---- */
#include "wave.h"
/* A cached SWAR's decoded samples (n of them, decoded on demand: empty until
 * then), or NULL if `swar` isn't a cached file. Shared by every song using it. */
Wave *sdat_swar_waves(const uint8_t *swar, uint32_t n);
/* Counts `bytes` more against the cache's budget for that file (samples decoded into it). */
void sdat_cache_account(const uint8_t *data, uint32_t bytes);

/* ---- sequence archives (SSAR: mostly sound effects) ---- */
const char *sdat_ssar_name(const Sdat *s, uint32_t arc);                 /* may be NULL */
const char *sdat_ssar_entry_name(const Sdat *s, uint32_t arc, uint32_t idx); /* may be NULL */
/* Reads an archive's entry table. Returns the entry count (0 if none/invalid) and a
 * malloc'd array in *out (free it). Empty slots have valid = 0. */
uint32_t sdat_ssar_entries(const Sdat *s, uint32_t arc, SdatSsarEntry **out);
/* Loads one archive entry the same way sdat_load_seq loads a sequence. */
int sdat_load_ssar_seq(const Sdat *s, uint32_t arc, uint32_t idx, SdatSeqBundle *out);

#endif
