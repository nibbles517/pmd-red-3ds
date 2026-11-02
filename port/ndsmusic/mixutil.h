/*
 * From the NDS Music Player (ndsmusicplayer, source/core), for the port's Music
 * Options. Files are read through the backend (Backend_File*) instead of stdio.
 */
/*
 * mixutil.h - output level helpers shared by the sequence and stream players.
 */
#ifndef NDSMUSIC_MIXUTIL_H
#define NDSMUSIC_MIXUTIL_H

#include <stdint.h>

/* Output above this level is softly compressed instead of hard-clipped. */
#define SOFT_KNEE 24576

#define GAIN_UNITY 256 /* gains are 8.8 fixed point */
#define GAIN_MAX (GAIN_UNITY * 4)

static inline int16_t soft_clip(int32_t s) {
    const int32_t room = 32767 - SOFT_KNEE;
    if (s > SOFT_KNEE) {
        int32_t x = s - SOFT_KNEE;
        s = SOFT_KNEE + (int32_t)((int64_t)room * x / (x + room));
    } else if (s < -SOFT_KNEE) {
        int32_t x = -s - SOFT_KNEE;
        s = -SOFT_KNEE - (int32_t)((int64_t)room * x / (x + room));
    }
    return (int16_t)s;
}

#endif
