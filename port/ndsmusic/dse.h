/*
 * From the NDS Music Player (ndsmusicplayer, source/core), for the port's Music
 * Options. Files are read through the backend (Backend_File*) instead of stdio.
 */
/*
 * dse.h - player for Procyon Studio's "Digital Sound Elements" (DSE) music, as used by
 * Pokémon Mystery Dungeon: Explorers of Time/Darkness/Sky.
 *
 *   SMDL (.smd)  a song: tracks of note/rest/control events
 *   SWDL (.swd)  per-song instrument bank (programs -> key splits -> sample ids)
 *   bgm.swd      the shared sample bank (wave info + PCM/ADPCM data) every song draws from
 *
 * Built from community format documentation (psy_commando's ppmdu, VG Music Studio) and
 * checked against the game's own engine running in an emulator.
 */
#ifndef NDSMUSIC_DSE_H
#define NDSMUSIC_DSE_H

#include <stdint.h>
#if NDSMUSIC_DSE_TRACE
#include <stdio.h>
#endif

typedef struct DseBank DseBank;
typedef struct DsePlayer DsePlayer;

/* Shared sample bank. Takes ownership of a malloc'd buffer (freed by dse_bank_close). */
DseBank *dse_bank_open(uint8_t *swd, uint32_t size);
void dse_bank_close(DseBank *b);

/* A song. smd/swd are copied as needed; the bank must outlive the player. */
DsePlayer *dse_create(const DseBank *bank, const uint8_t *smd, uint32_t smd_size, const uint8_t *swd,
                      uint32_t swd_size, uint32_t out_rate);
void dse_destroy(DsePlayer *p);

void dse_render(DsePlayer *p, int16_t *out, uint32_t frames);
int dse_finished(const DsePlayer *p);
int dse_loops(const DsePlayer *p);
uint16_t dse_voice_mask(const DsePlayer *p);
void dse_set_gain(DsePlayer *p, int32_t gain); /* 8.8 fixed point, 256 = unity */

/* Internal song name from the SMDL header (e.g. "B_SYS_P3_OPENIN"); empty if none. */
void dse_smd_label(const uint8_t *smd, uint32_t size, char out[17]);
/* Quick format check. */
int dse_is_smdl(const uint8_t *data, uint32_t size);

#if NDSMUSIC_DSE_TRACE
/* Debug: per-voice log for comparing against the real engine (NULL = off). */
void dse_set_trace(DsePlayer *p, FILE *f);
#endif

#endif
