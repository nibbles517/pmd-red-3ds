// The game's data from the player's own ROM.
//
// A shared build (the cloud's .3dsx) has none of the ROM's data in it:
// tools/rom_scrub.py zeroed each stretch of the game's objects that is the
// ROM's own bytes and listed them in gPortRomData (where it goes, where it is
// in the ROM, how long), sorted by the ROM offset. At start they are copied
// back from pmdred.gba (Pokemon Mystery Dungeon: Red Rescue Team, USA),
// found next to the .3dsx or in the data folder.
//
// A personal build (make prelink, with the ROM's data in it) has no table:
// these weak ones say so, and nothing is read.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "port.h"

// (romdata_table.s from rom_scrub.py; else romdata_none.c's weak empty ones,
// in a file of their own so the compiler can't take their values here).
// gPortRomData: per section, its address and (ULEB128) number of runs, then
// each run as ULEB128 bytes on from the last run's end, SLEB128 ROM offset
// from the last run's ROM end (the previous section's for its first), and
// ULEB128 length; each section's padded to 4 bytes.
extern const uint32_t gPortRomDataCount;
extern const uint32_t gPortRomDataSum;
extern const uint32_t gPortRomDataGroups;
extern const uint32_t gPortRomData[];

typedef struct {
    uint8_t *dest;
    uint32_t rom, len;
} RomRun;

static int ByRom(const void *a, const void *b)
{
    uint32_t x = ((const RomRun *)a)->rom, y = ((const RomRun *)b)->rom;
    return x < y ? -1 : x > y;
}

static uint32_t Uleb(const uint8_t **p)
{
    uint32_t v = 0;
    int shift = 0;
    uint8_t b;
    do {
        b = *(*p)++;
        v |= (uint32_t)(b & 0x7F) << shift;
        shift += 7;
    } while (b & 0x80);
    return v;
}

static int32_t Sleb(const uint8_t **p)
{
    uint32_t v = 0;
    int shift = 0;
    uint8_t b;
    do {
        b = *(*p)++;
        v |= (uint32_t)(b & 0x7F) << shift;
        shift += 7;
    } while (b & 0x80);
    if (shift < 32 && (b & 0x40))
        v |= ~0u << shift;
    return (int32_t)v;
}

#define ROM_SIZE 0x2000000
#define WINDOW (256 * 1024)

static const char *const sRomNames[] = {
    "pmdred.gba", "pmd-red.gba", "pmd_red.gba", "PMDRed.gba", "PMD Red.gba",
};

static void *OpenRom(char *path, size_t pathSize)
{
    const char *appDir = Backend_AppDir();
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < sizeof(sRomNames) / sizeof(sRomNames[0]); i++) {
            if (pass == 0) {
                if (appDir == NULL)
                    continue;
                snprintf(path, pathSize, "%s/%s", appDir, sRomNames[i]);
            } else {
                snprintf(path, pathSize, "%s", sRomNames[i]);
            }
            void *f = Backend_FileOpen(path);
            if (f != NULL)
                return f;
        }
    }
    return NULL;
}

bool Port_RomDataNeeded(void)
{
    return gPortRomDataCount != 0;
}

void Port_LoadRomData(void)
{
    if (!Port_RomDataNeeded())
        return;
    char path[320];
    void *f = OpenRom(path, sizeof(path));
    if (f == NULL)
        Backend_Fatal("pmdred.gba not found.\n\n"
                      "This port needs your own ROM of\n"
                      "Pokemon Mystery Dungeon:\n"
                      "Red Rescue Team (USA), named\n"
                      "pmdred.gba, in the same folder as\n"
                      "the .3dsx or in\n"
                      "sdmc:/3ds/pmd-red-3ds/");
    uint8_t header[0x10];
    if (Backend_FileSize(f) < ROM_SIZE || !Backend_FileRead(f, 0xA0, header, sizeof(header))
        || memcmp(header, "POKE DUNGEONB24E", 16) != 0)
        Backend_Fatal("pmdred.gba is not the right ROM.\n\n"
                      "Please use Pokemon Mystery\n"
                      "Dungeon: Red Rescue Team (USA).\n"
                      "Europe's ROM, a hack or a\n"
                      "trimmed ROM will not work.");

    // The table's runs, in the order of where they are in the ROM, so it
    // is read once from start to end (in the table's own order, its
    // sections' runs jump about, and it was read many times over)
    uint32_t count = gPortRomDataCount, rom = 0, n = 0;
    RomRun *list = malloc(count * sizeof(RomRun));
    uint8_t *win = malloc(WINDOW);
    if (list == NULL || win == NULL)
        Backend_Fatal("Out of memory loading pmdred.gba");
    const uint32_t *g = gPortRomData;
    for (uint32_t gi = 0; gi < gPortRomDataGroups; gi++) {
        uint8_t *out = (uint8_t *)(uintptr_t)g[0];
        const uint8_t *p = (const uint8_t *)(g + 1);
        uint32_t runs = Uleb(&p);
        for (uint32_t i = 0; i < runs; i++) {
            out += Uleb(&p);
            rom += (uint32_t)Sleb(&p);
            uint32_t len = Uleb(&p);
            if (n == count || rom > ROM_SIZE || len > ROM_SIZE - rom)
                Backend_Fatal("romdata: bad table");
            list[n++] = (RomRun){out, rom, len};
            out += len;
            rom += len;
        }
        g = (const uint32_t *)(((uintptr_t)p + 3) & ~(uintptr_t)3);
    }
    if (n != count)
        Backend_Fatal("romdata: bad table");
    qsort(list, count, sizeof(RomRun), ByRom);

    uint32_t winOff = 0, winLen = 0, sum = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t *out = list[i].dest;
        uint32_t pos = list[i].rom, left = list[i].len, h = 2166136261u ^ pos;
        while (left > 0) {
            if (pos < winOff || pos >= winOff + winLen) {
                winOff = pos;
                winLen = ROM_SIZE - pos < WINDOW ? ROM_SIZE - pos : WINDOW;
                if (!Backend_FileRead(f, winOff, win, winLen))
                    Backend_Fatal("Could not read pmdred.gba");
            }
            uint32_t k = winOff + winLen - pos;
            if (k > left)
                k = left;
            const uint8_t *src = win + (pos - winOff);
            for (uint32_t b = 0; b < k; b++)
                h = (h ^ src[b]) * 16777619u;
            memcpy(out, src, k);
            out += k;
            pos += k;
            left -= k;
        }
        sum += h;
    }
    free(list);
    free(win);
    Backend_FileClose(f);
    if (sum != gPortRomDataSum)
        Backend_Fatal("pmdred.gba is not the right ROM.\n\n"
                      "Its data is not what this port\n"
                      "expects. Please use an unmodified\n"
                      "Pokemon Mystery Dungeon:\n"
                      "Red Rescue Team (USA) ROM.");
    Backend_Log("romdata: %u runs from %s", (unsigned)gPortRomDataCount, path);
}
