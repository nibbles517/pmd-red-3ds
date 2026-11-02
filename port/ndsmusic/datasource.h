/*
 * From the NDS Music Player (ndsmusicplayer, source/core), for the port's Music
 * Options. Files are read through the backend (Backend_File*) instead of stdio.
 */
/*
 * datasource.h - random-access byte source backed by memory or a FILE*.
 *
 * The SDAT parser only ever talks to a DataSource, so the same code path works for:
 *   - NCSF files (SDAT already decompressed into RAM -> ds_mem)
 *   - .nds ROMs   (SDAT read lazily off the SD card -> ds_file + ds_sub)
 */
#ifndef NDSMUSIC_DATASOURCE_H
#define NDSMUSIC_DATASOURCE_H

#include <stdint.h>
#include <stdio.h>

typedef enum { DS_MEM, DS_FILE } DsKind;

typedef struct DataSource {
    DsKind kind;
    const uint8_t *mem; /* DS_MEM */
    void *fp;           /* DS_FILE: a Backend_FileOpen handle */
    uint32_t base;      /* absolute offset of byte 0 of this view */
    uint32_t size;      /* bytes visible through this view */
} DataSource;

DataSource ds_mem(const void *data, uint32_t size);
/* Wraps an already-open file. Returns size 0 on failure. Caller keeps ownership of fp. */
DataSource ds_file(void *fp);
/* Sub-view [off, off+size) of parent, clamped to the parent's bounds. */
DataSource ds_sub(const DataSource *parent, uint32_t off, uint32_t size);

/* 0 on success, -1 on out-of-range or I/O error. */
int ds_read(const DataSource *ds, uint32_t off, void *dst, uint32_t len);
/* malloc + read; returns NULL on failure. Free with free(). */
uint8_t *ds_load(const DataSource *ds, uint32_t off, uint32_t len);

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

#endif
