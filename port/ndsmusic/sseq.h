/*
 * From the NDS Music Player (ndsmusicplayer, source/core), for the port's Music
 * Options. Files are read through the backend (Backend_File*) instead of stdio.
 */
/*
 * sseq.h - Nitro sequence (SSEQ) player: sequencer, instruments, ADSR, LFO and a
 * 16-voice software mixer modelled on the DS sound hardware.
 *
 * Feed it an SdatSeqBundle (from an NDS ROM or an NCSF) and pull stereo PCM out.
 */
#ifndef NDSMUSIC_SSEQ_H
#define NDSMUSIC_SSEQ_H

#include <stdint.h>

#include "sdat.h"

typedef struct SseqPlayer SseqPlayer;

/* The bundle must outlive the player. Returns NULL on bad data / out of memory. */
SseqPlayer *sseq_create(const SdatSeqBundle *b, uint32_t out_rate);
void sseq_destroy(SseqPlayer *p);

/* Writes `frames` interleaved stereo samples. */
void sseq_render(SseqPlayer *p, int16_t *out, uint32_t frames);

int sseq_finished(const SseqPlayer *p);      /* all tracks ended and all voices silent */
int sseq_loops(const SseqPlayer *p);         /* times the song has looped back */
uint16_t sseq_voice_mask(const SseqPlayer *p); /* bit n = hardware channel n is sounding */

#endif
