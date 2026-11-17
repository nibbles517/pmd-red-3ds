/*
 * From the NDS Music Player (ndsmusicplayer, source/core), for the port's Music
 * Options. Files are read through the backend (Backend_File*) instead of stdio.
 */
/*
 * nds_music.h - opens a .nds ROM straight from the SD card and exposes its SDAT(s), or the
 * DSE music of games that use Procyon's engine instead (Mystery Dungeon: Explorers).
 * Nothing but the tables is loaded up front; song data is read on demand.
 */
#ifndef NDSMUSIC_NDS_MUSIC_H
#define NDSMUSIC_NDS_MUSIC_H

#include "nds_banner.h"
#include "nds_rom.h"
#include "dse_set.h"
#include "sdat.h"

#define NDS_MUSIC_MAX_SDATS 8

typedef enum {
    NDSM_OK = 0,
    NDSM_ERR_OPEN = -1,    /* couldn't open file */
    NDSM_ERR_HEADER = -2,  /* not an NDS ROM / bad FNT or FAT */
    NDSM_ERR_NO_SDAT = -3, /* no SDAT and no DSE music: unsupported sound engine */
    NDSM_ERR_SDAT = -4     /* SDAT(s) found but none parsed */
} NdsMusicResult;

typedef struct {
    void *fp; /* Backend_FileOpen */
    DataSource rom;
    NdsHeader hdr;
    NdsBanner banner; /* banner.valid = 0 if the ROM has none */
    int num_sdats;
    NdsFile sdat_file[NDS_MUSIC_MAX_SDATS];
    Sdat sdat[NDS_MUSIC_MAX_SDATS];
    DseSet dse; /* dse.num = 0 if the game has no DSE music */
} NdsMusic;

/* path: a file in the port's data folder, or /a/path on the SD card (Backend_FileOpen) */
NdsMusicResult nds_music_open(NdsMusic *m, const char *path);
void nds_music_close(NdsMusic *m);
const char *nds_music_strerror(NdsMusicResult r);

#endif
