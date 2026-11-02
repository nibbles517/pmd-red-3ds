/*
 * From the NDS Music Player (ndsmusicplayer, source/core), for the port's Music
 * Options. Files are read through the backend (Backend_File*) instead of stdio.
 */
/*
 * song.h - one playable track (an SSEQ or a STRM) plus loop/length/fade handling.
 * This is the only playback API the front end (3DS app or PC tools) needs.
 */
#ifndef NDSMUSIC_SONG_H
#define NDSMUSIC_SONG_H

#include "sdat.h"

typedef struct Song Song;

/* The Sdat (and, for ROMs, its FILE*) must stay open while a STRM song plays.
 * SSEQ songs copy what they need and don't depend on the Sdat afterwards. */
Song *song_open_seq(const Sdat *s, uint32_t seq_index, uint32_t out_rate);
Song *song_open_strm(const Sdat *s, uint32_t strm_index, uint32_t out_rate);
void song_close(Song *g);

/* Fixed timing (from NCSF tags): play length_ms, then fade over fade_ms. */
void song_set_length(Song *g, uint32_t length_ms, uint32_t fade_ms);
/* Loop-based timing (default 2 loops, 8 s fade). loops = 0 repeats forever. Clears any length. */
void song_set_loops(Song *g, int loops, uint32_t fade_ms);

void song_render(Song *g, int16_t *out, uint32_t frames);
int song_done(const Song *g);
uint32_t song_pos_ms(const Song *g);
int song_loops(const Song *g);
uint16_t song_voices(const Song *g); /* active hardware channels (SSEQ only) */

#endif
