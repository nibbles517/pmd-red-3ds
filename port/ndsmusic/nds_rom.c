/*
 * From the NDS Music Player (ndsmusicplayer, source/core), for the port's Music
 * Options. Files are read through the backend (Backend_File*) instead of stdio.
 */
#include "nds_rom.h"

/* (no <ctype.h>: its tables belong to the C library the 3DS build links) */
#define isprint(c) ((c) >= 0x20 && (c) < 0x7F)
#define tolower(c) ((c) >= 'A' && (c) <= 'Z' ? (c) + 32 : (c))
#include <stdlib.h>
#include <string.h>

#define NDS_MAX_DIR_DEPTH 32

int nds_read_header(const DataSource *rom, NdsHeader *h) {
    uint8_t b[0x50];
    memset(h, 0, sizeof *h);
    if (ds_read(rom, 0, b, sizeof b) != 0) return -1;

    memcpy(h->title, b, 12);
    memcpy(h->gamecode, b + 0x0C, 4);
    for (int i = 0; i < 12; i++)
        if (h->title[i] && !isprint((unsigned char)h->title[i])) h->title[i] = '?';

    h->fnt_off = rd32(b + 0x40);
    h->fnt_size = rd32(b + 0x44);
    h->fat_off = rd32(b + 0x48);
    h->fat_size = rd32(b + 0x4C);

    if (h->fnt_size < 8 || h->fnt_off > rom->size || h->fnt_size > rom->size - h->fnt_off) return -1;
    if (h->fat_off > rom->size || h->fat_size > rom->size - h->fat_off) return -1;
    return 0;
}

typedef struct {
    const DataSource *rom;
    const uint8_t *fnt;
    uint32_t fnt_size;
    const uint8_t *fat;
    uint32_t fat_count;
    uint16_t num_dirs;
    nds_visit_fn fn;
    void *user;
    NdsFile cur;
} WalkCtx;

static int walk_dir(WalkCtx *w, uint16_t dir_index, size_t plen, int depth) {
    if (depth > NDS_MAX_DIR_DEPTH || dir_index >= w->num_dirs) return -1;

    const uint8_t *e = w->fnt + (uint32_t)dir_index * 8;
    uint32_t p = rd32(e);
    uint16_t file_id = rd16(e + 4);
    char *path = w->cur.path;

    for (;;) {
        if (p >= w->fnt_size) return -1;
        uint8_t t = w->fnt[p++];
        if (t == 0x00) break; /* end of this directory */
        if (t == 0x80) return -1; /* reserved */

        uint32_t len = t & 0x7F;
        if (len > w->fnt_size - p) return -1;
        const char *name = (const char *)w->fnt + p;
        p += len;

        if (plen + 1 + len >= NDS_PATH_MAX) return -1;
        path[plen] = '/';
        memcpy(path + plen + 1, name, len);
        path[plen + 1 + len] = '\0';

        if (t & 0x80) { /* subdirectory */
            if (p + 2 > w->fnt_size) return -1;
            uint16_t sub = rd16(w->fnt + p) & 0x0FFF;
            p += 2;
            int r = walk_dir(w, sub, plen + 1 + len, depth + 1);
            if (r) return r;
        } else { /* file */
            if (file_id < w->fat_count) {
                uint32_t start = rd32(w->fat + (uint32_t)file_id * 8);
                uint32_t end = rd32(w->fat + (uint32_t)file_id * 8 + 4);
                if (end >= start && end <= w->rom->size) {
                    w->cur.file_id = file_id;
                    w->cur.offset = start;
                    w->cur.size = end - start;
                    int r = w->fn(&w->cur, w->user);
                    if (r) return r;
                }
            }
            file_id++;
        }
        path[plen] = '\0';
    }
    return 0;
}

int nds_walk_files(const DataSource *rom, const NdsHeader *h, nds_visit_fn fn, void *user) {
    uint8_t *fnt = ds_load(rom, h->fnt_off, h->fnt_size);
    uint8_t *fat = ds_load(rom, h->fat_off, h->fat_size);
    int r = -1;

    if (fnt && fat) {
        WalkCtx w;
        memset(&w, 0, sizeof w);
        w.rom = rom;
        w.fnt = fnt;
        w.fnt_size = h->fnt_size;
        w.fat = fat;
        w.fat_count = h->fat_size / 8;
        w.num_dirs = rd16(fnt + 6); /* root entry's "parent" field = total dir count */
        w.fn = fn;
        w.user = user;
        if (w.num_dirs >= 1 && (uint32_t)w.num_dirs * 8 <= h->fnt_size)
            r = walk_dir(&w, 0, 0, 0);
    }
    free(fnt);
    free(fat);
    return r;
}

typedef struct {
    const DataSource *rom;
    NdsFile *out;
    int max, count;
    int by_magic;
} FindCtx;

static int ends_with_sdat(const char *s) {
    size_t n = strlen(s);
    if (n < 5) return 0;
    const char *ext = s + n - 5;
    return ext[0] == '.' && tolower((unsigned char)ext[1]) == 's' && tolower((unsigned char)ext[2]) == 'd' &&
           tolower((unsigned char)ext[3]) == 'a' && tolower((unsigned char)ext[4]) == 't';
}

static int find_cb(const NdsFile *f, void *user) {
    FindCtx *c = (FindCtx *)user;
    int match;
    if (c->by_magic) {
        uint8_t m[4];
        match = f->size >= 0x40 && ds_read(c->rom, f->offset, m, 4) == 0 && memcmp(m, "SDAT", 4) == 0;
    } else {
        match = ends_with_sdat(f->path);
    }
    if (match && c->count < c->max) c->out[c->count++] = *f;
    return 0;
}

int nds_find_sdats(const DataSource *rom, const NdsHeader *h, NdsFile *out, int max) {
    FindCtx c = {rom, out, max, 0, 0};
    int r = nds_walk_files(rom, h, find_cb, &c);
    if (r < 0) return r;
    if (c.count == 0) { /* renamed or extension-less SDAT: fall back to magic scan */
        c.by_magic = 1;
        r = nds_walk_files(rom, h, find_cb, &c);
        if (r < 0) return r;
    }
    return c.count;
}
