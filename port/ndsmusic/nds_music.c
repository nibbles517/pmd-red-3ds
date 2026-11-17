/*
 * From the NDS Music Player (ndsmusicplayer, source/core), for the port's Music
 * Options. Files are read through the backend (Backend_File*) instead of stdio.
 */
#include "nds_music.h"

#include <stdlib.h>

#include "../core/port.h"
#include <string.h>

static int dse_visit(const NdsFile *f, void *user) {
    dse_set_add_file((DseSet *)user, f->path, f->offset, f->size);
    return 0;
}

NdsMusicResult nds_music_open(NdsMusic *m, const char *path) {
    NdsFile found[NDS_MUSIC_MAX_SDATS];
    memset(m, 0, sizeof *m);

    m->fp = Backend_FileOpen(path); /* (the backend buffers its reads) */
    if (!m->fp) return NDSM_ERR_OPEN;

    m->rom = ds_file(m->fp);
    if (m->rom.size < 0x200 || nds_read_header(&m->rom, &m->hdr) != 0) {
        nds_music_close(m);
        return NDSM_ERR_HEADER;
    }

    nds_read_banner(&m->rom, &m->banner); /* optional */

    int n = nds_find_sdats(&m->rom, &m->hdr, found, NDS_MUSIC_MAX_SDATS);
    if (n < 0) {
        nds_music_close(m);
        return NDSM_ERR_HEADER;
    }
    for (int i = 0; i < n; i++) {
        DataSource sub = ds_sub(&m->rom, found[i].offset, found[i].size);
        if (sdat_open(&m->sdat[m->num_sdats], &sub) == 0) m->sdat_file[m->num_sdats++] = found[i];
    }

    /* Games on Procyon's DSE engine keep their music under SOUND/BGM instead. */
    dse_set_init(&m->dse, &m->rom);
    nds_walk_files(&m->rom, &m->hdr, dse_visit, &m->dse);
    dse_set_finish(&m->dse);

    if (m->num_sdats == 0 && m->dse.num == 0) {
        nds_music_close(m);
        return n > 0 ? NDSM_ERR_SDAT : NDSM_ERR_NO_SDAT;
    }
    return NDSM_OK;
}

void nds_music_close(NdsMusic *m) {
    for (int i = 0; i < m->num_sdats; i++) sdat_close(&m->sdat[i]);
    dse_set_free(&m->dse);
    if (m->fp) Backend_FileClose(m->fp);
    memset(m, 0, sizeof *m);
}

const char *nds_music_strerror(NdsMusicResult r) {
    switch (r) {
    case NDSM_OK: return "OK";
    case NDSM_ERR_OPEN: return "Couldn't open the file";
    case NDSM_ERR_HEADER: return "Not a valid NDS ROM";
    case NDSM_ERR_NO_SDAT: return "No music found (unsupported sound engine)";
    case NDSM_ERR_SDAT: return "SDAT found but couldn't be read";
    }
    return "Unknown error";
}
