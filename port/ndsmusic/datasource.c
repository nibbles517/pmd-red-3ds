/*
 * From the NDS Music Player (ndsmusicplayer, source/core), for the port's Music
 * Options. Files are read through the backend (Backend_File*) instead of stdio.
 */
#include "datasource.h"

#include <stdlib.h>

#include "../core/port.h"
#include <string.h>

DataSource ds_mem(const void *data, uint32_t size) {
    DataSource ds;
    memset(&ds, 0, sizeof ds);
    ds.kind = DS_MEM;
    ds.mem = (const uint8_t *)data;
    ds.size = data ? size : 0;
    return ds;
}

DataSource ds_file(void *fp) {
    DataSource ds;
    memset(&ds, 0, sizeof ds);
    ds.kind = DS_FILE;
    ds.fp = fp;
    if (!fp) return ds;
    ds.size = Backend_FileSize(fp);
    return ds;
}

DataSource ds_sub(const DataSource *parent, uint32_t off, uint32_t size) {
    DataSource ds = *parent;
    if (off > parent->size) {
        ds.size = 0;
        return ds;
    }
    if (size > parent->size - off) size = parent->size - off;
    ds.base = parent->base + off;
    ds.size = size;
    return ds;
}

int ds_read(const DataSource *ds, uint32_t off, void *dst, uint32_t len) {
    if (off > ds->size || len > ds->size - off) return -1;
    if (len == 0) return 0;
    if (ds->kind == DS_MEM) {
        memcpy(dst, ds->mem + ds->base + off, len);
        return 0;
    }
    return Backend_FileRead(ds->fp, ds->base + off, dst, len) ? 0 : -1;
}

uint8_t *ds_load(const DataSource *ds, uint32_t off, uint32_t len) {
    uint8_t *buf = (uint8_t *)malloc(len ? len : 1);
    if (!buf) return NULL;
    if (ds_read(ds, off, buf, len) != 0) {
        free(buf);
        return NULL;
    }
    return buf;
}
