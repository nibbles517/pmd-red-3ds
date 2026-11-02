/*
 * From the NDS Music Player (ndsmusicplayer, source/core), for the port's Music
 * Options. Files are read through the backend (Backend_File*) instead of stdio.
 */
#include "nds_banner.h"

#include <string.h>

#define BANNER_V1_SIZE 0x840
#define BANNER_V2_SIZE 0x940
#define BANNER_V3_SIZE 0xA40
#define BANNER_DSI_SIZE 0x23C0

/* 4x4 tiles of 8x8 pixels, two pixels per byte, low nibble first. */
static void decode_bitmap(const uint8_t *src, uint8_t *dst) {
    for (int ty = 0; ty < 4; ty++)
        for (int tx = 0; tx < 4; tx++) {
            const uint8_t *tile = src + (ty * 4 + tx) * 32;
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x += 2) {
                    uint8_t b = tile[y * 4 + x / 2];
                    uint8_t *row = dst + (ty * 8 + y) * BANNER_ICON_W + tx * 8 + x;
                    row[0] = b & 0x0F;
                    row[1] = b >> 4;
                }
        }
}

static void decode_palette(const uint8_t *src, uint16_t *dst) {
    for (int i = 0; i < 16; i++) dst[i] = rd16(src + i * 2) & 0x7FFF;
}

/* Latin-1 letters -> unaccented ASCII (the 3DS console font is ASCII-only here). */
static const char latin1_ascii[64 + 1] =
    "AAAAAAACEEEEIIII"  /* C0-CF */
    "DNOOOOOxOUUUUYTs"  /* D0-DF */
    "aaaaaaaceeeeiiii"  /* E0-EF */
    "dnooooo/ouuuuyty"; /* F0-FF */

static void decode_title(const uint8_t *src, char *dst) {
    int n = 0;
    for (int i = 0; i < 128 && n < BANNER_TITLE_MAX - 1; i++) {
        uint16_t c = rd16(src + i * 2);
        if (c == 0) break;
        char out;
        if (c == '\n') out = '\n';
        else if (c == '\r') continue;
        else if (c >= 0x20 && c < 0x7F) out = (char)c;
        else if (c >= 0xC0 && c <= 0xFF) out = latin1_ascii[c - 0xC0];
        else if (c == 0x2019 || c == 0x2018) out = '\'';
        else if (c == 0x3000) out = ' '; /* ideographic space */
        else if (c == 0x00A9 || c == 0x00AE || c == 0x2122) continue; /* (c) (R) TM */
        else out = '?';
        dst[n++] = out;
    }
    /* trim trailing whitespace/newlines */
    while (n > 0 && (dst[n - 1] == '\n' || dst[n - 1] == ' ')) n--;
    dst[n] = 0;
}

int nds_read_banner(const DataSource *rom, NdsBanner *b) {
    memset(b, 0, sizeof *b);
    uint8_t hdr[4];
    if (ds_read(rom, 0x68, hdr, 4) != 0) return -1;
    uint32_t off = rd32(hdr);
    if (off == 0 || off >= rom->size) return -1;

    static uint8_t buf[BANNER_DSI_SIZE]; /* not reentrant; banners are read from one thread */
    uint32_t avail = rom->size - off;
    if (avail < BANNER_V1_SIZE) return -1;
    uint32_t len = avail < BANNER_DSI_SIZE ? avail : BANNER_DSI_SIZE;
    if (ds_read(rom, off, buf, len) != 0) return -1;

    uint16_t version = rd16(buf);
    if (version != 0x0001 && version != 0x0002 && version != 0x0003 && version != 0x0103) return -1;

    decode_bitmap(buf + 0x20, b->pixels[0]);
    decode_palette(buf + 0x220, b->palette[0]);
    b->num_bitmaps = b->num_palettes = 1;
    b->num_frames = 1;

    int langs = 6;
    if (version >= 0x0002 && len >= BANNER_V2_SIZE) langs = 7;
    if (version >= 0x0003 && len >= BANNER_V3_SIZE) langs = 8;
    for (int i = 0; i < langs; i++) decode_title(buf + 0x240 + i * 0x100, b->title[i]);

    if (version == 0x0103 && len >= BANNER_DSI_SIZE) {
        int nf = 0;
        uint8_t max_bmp = 0, max_pal = 0;
        for (int i = 0; i < BANNER_MAX_FRAMES; i++) {
            uint16_t e = rd16(buf + 0x2340 + i * 2);
            uint8_t dur = e & 0xFF;
            if (dur == 0) break; /* end of sequence: loop back to frame 0 */
            BannerFrame *f = &b->frame[nf++];
            f->duration = dur;
            f->bitmap = (e >> 8) & 7;
            f->palette = (e >> 11) & 7;
            f->flip_h = (e >> 14) & 1;
            f->flip_v = (e >> 15) & 1;
            if (f->bitmap > max_bmp) max_bmp = f->bitmap;
            if (f->palette > max_pal) max_pal = f->palette;
        }
        if (nf > 0) {
            for (int i = 0; i <= max_bmp; i++) decode_bitmap(buf + 0x1240 + i * 0x200, b->pixels[i]);
            for (int i = 0; i <= max_pal; i++) decode_palette(buf + 0x2240 + i * 0x20, b->palette[i]);
            b->num_bitmaps = (uint8_t)(max_bmp + 1);
            b->num_palettes = (uint8_t)(max_pal + 1);
            b->num_frames = nf;
            b->animated = nf > 1;
        } else {
            memset(b->frame, 0, sizeof b->frame); /* no sequence: keep the static icon */
        }
    }
    b->valid = 1;
    return 0;
}

const char *nds_banner_title(const NdsBanner *b, int lang) {
    if (lang >= 0 && lang < BANNER_LANGS && b->title[lang][0]) {
        /* A title we can only print as "???" is worse than the English one. */
        int bad = 0, total = 0;
        for (const char *p = b->title[lang]; *p; p++, total++) bad += *p == '?';
        if (bad * 2 < total) return b->title[lang];
    }
    if (b->title[BANNER_EN][0]) return b->title[BANNER_EN];
    return b->title[BANNER_JP];
}

int nds_banner_pixel(const NdsBanner *b, int f, int x, int y) {
    if (!b->valid || f < 0 || f >= b->num_frames) return -1;
    const BannerFrame *fr = &b->frame[f];
    if (fr->flip_h) x = BANNER_ICON_W - 1 - x;
    if (fr->flip_v) y = BANNER_ICON_W - 1 - y;
    uint8_t idx = b->pixels[fr->bitmap][y * BANNER_ICON_W + x];
    return idx == 0 ? -1 : b->palette[fr->palette][idx];
}
