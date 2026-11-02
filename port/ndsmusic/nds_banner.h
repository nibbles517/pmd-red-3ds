/*
 * From the NDS Music Player (ndsmusicplayer, source/core), for the port's Music
 * Options. Files are read through the backend (Backend_File*) instead of stdio.
 */
/*
 * nds_banner.h - the icon and titles shown on the DS Menu.
 *
 * Header offset 0x68 points at the banner:
 *   +0x000 u16 version (1, 2 = +Chinese, 3 = +Korean, 0x0103 = DSi animated icon)
 *   +0x020 32x32 4bpp tiled bitmap, +0x220 16-colour BGR555 palette (colour 0 = transparent)
 *   +0x240 titles, 0x100 bytes of UTF-16 each: JP EN FR DE IT ES [ZH] [KO]
 *   +0x1240 (v0x0103) 8 bitmaps, +0x2240 8 palettes, +0x2340 64-entry animation sequence
 */
#ifndef NDSMUSIC_NDS_BANNER_H
#define NDSMUSIC_NDS_BANNER_H

#include "datasource.h"

#define BANNER_ICON_W 32
#define BANNER_MAX_FRAMES 64
#define BANNER_TITLE_MAX 160

enum { BANNER_JP, BANNER_EN, BANNER_FR, BANNER_DE, BANNER_IT, BANNER_ES, BANNER_ZH, BANNER_KO, BANNER_LANGS };

typedef struct {
    uint8_t bitmap, palette;
    uint8_t flip_h, flip_v;
    uint8_t duration; /* in 60 Hz frames; 0 = hold forever (static icon) */
} BannerFrame;

typedef struct {
    uint8_t valid;
    uint8_t animated;
    uint8_t num_bitmaps, num_palettes;
    uint8_t pixels[8][BANNER_ICON_W * BANNER_ICON_W]; /* palette indices, row-major */
    uint16_t palette[8][16];                         /* BGR555 */
    int num_frames;
    BannerFrame frame[BANNER_MAX_FRAMES];
    /* Titles converted to printable ASCII ('\n' separates lines). Empty if missing. */
    char title[BANNER_LANGS][BANNER_TITLE_MAX];
} NdsBanner;

/* 0 on success. A ROM without a banner returns -1 with out->valid = 0. */
int nds_read_banner(const DataSource *rom, NdsBanner *out);

/* Title for a BANNER_* language, falling back to English, then Japanese. */
const char *nds_banner_title(const NdsBanner *b, int lang);

/* Colour of icon pixel (x, y) for animation frame f, as BGR555; returns -1 for transparent. */
int nds_banner_pixel(const NdsBanner *b, int f, int x, int y);

#endif
