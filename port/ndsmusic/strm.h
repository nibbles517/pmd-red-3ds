/*
 * From the NDS Music Player (ndsmusicplayer, source/core), for the port's Music
 * Options. Files are read through the backend (Backend_File*) instead of stdio.
 */
/*
 * strm.h - Nitro stream (STRM) player. Decodes one block at a time straight from the
 * DataSource, so even multi-megabyte streams use only a few KB of RAM.
 */
#ifndef NDSMUSIC_STRM_H
#define NDSMUSIC_STRM_H

#include "datasource.h"

typedef struct StrmPlayer StrmPlayer;

/* src must stay readable (FILE* open) until strm_destroy. */
StrmPlayer *strm_create(const DataSource *src, uint32_t out_rate);
void strm_destroy(StrmPlayer *s);
void strm_render(StrmPlayer *s, int16_t *out, uint32_t frames);
int strm_finished(const StrmPlayer *s);
int strm_loops(const StrmPlayer *s);

#endif
