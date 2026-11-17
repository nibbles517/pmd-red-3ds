/*
 * From the NDS Music Player (ndsmusicplayer, source/core), for the port's Music
 * Options. Files are read through the backend (Backend_File*) instead of stdio.
 */
/*
 * dse_set.h - the DSE songs of one game: SOUND/BGM (bgmNNNN.smd/.swd + bgm.swd sample bank)
 * and SOUND/ME jingles, found either in a ROM or in a 2SF library's game image.
 */
#ifndef NDSMUSIC_DSE_SET_H
#define NDSMUSIC_DSE_SET_H

#include "datasource.h"
#include "dse.h"

enum { DSE_GROUP_BGM = 0, DSE_GROUP_ME = 1, DSE_GROUPS };

typedef struct {
    uint16_t number; /* NNNN from bgmNNNN.smd */
    uint8_t group;   /* DSE_GROUP_* */
    uint32_t smd_off, smd_len;
    uint32_t swd_off, swd_len;
    char label[17]; /* internal name from the SMDL header */
} DseSong;

typedef struct {
    uint32_t number, off, len;
    uint8_t group, is_smd;
} DseCandidate;

typedef struct {
    DataSource src;
    int num;
    DseSong *songs;
    uint32_t bank_off[DSE_GROUPS], bank_len[DSE_GROUPS];
    uint8_t has_bank[DSE_GROUPS];
    DseBank *bank[DSE_GROUPS]; /* loaded on first use */
    DseCandidate *cand;
    int ncand, cand_cap;
} DseSet;

void dse_set_init(DseSet *s, const DataSource *src);
/* Feed every file of the game's file system; non-DSE files are ignored. Returns 1 if kept. */
int dse_set_add_file(DseSet *s, const char *path, uint32_t off, uint32_t size);
/* Pairs songs with their banks and reads names. Returns the number of songs. */
int dse_set_finish(DseSet *s);
void dse_set_free(DseSet *s);

/* Index of song `number` in `group`, or -1. */
int dse_set_find(const DseSet *s, int group, uint32_t number);
/* Creates a player for song idx (loads the shared sample bank the first time). */
DsePlayer *dse_set_open(DseSet *s, int idx, uint32_t out_rate);

#endif
