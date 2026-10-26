// Reference runner: plays the original ROM in mGBA (headless) so port output
// can be compared against real GBA behaviour. Dev tool, not part of the build.
//
// usage: gba_ref ROM SAV OUTDIR FRAMES [options]
//   -p F:KEYS:DUR   hold KEYS (A+B+...) from frame F for DUR frames
//   -s F            dump frame F as OUTDIR/frame_F.ppm
//   -e N            dump every Nth frame
//   -l STATE / -w STATE   load / write an mGBA savestate (frame count restarts at -f)
//   -f F            starting frame number (when loading a state)
//   -a WAV          write the audio (stereo 16-bit, 65536 Hz)
//   -d ADDR:LEN:FILE  dump bus memory after the last frame
//   -m MASK         audio channels to keep: bits 0-3 PSG, 4-5 Direct Sound A/B
#include <mgba/core/core.h>
#include <mgba/core/serialize.h>
#include <mgba-util/audio-buffer.h>
#include <mgba-util/vfs.h>
#include <mgba/core/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *kNames[] = { "A", "B", "SELECT", "START", "RIGHT", "LEFT", "UP", "DOWN", "R", "L" };
struct Press { int start, end; unsigned mask; } presses[256];
int npress;

static unsigned ParseKeys(char *s)
{
    unsigned m = 0;
    for (char *t = strtok(s, "+"); t; t = strtok(NULL, "+"))
        for (int i = 0; i < 10; i++)
            if (!strcmp(t, kNames[i])) m |= 1u << i;
    return m;
}

static void NoLog(struct mLogger *l, int c, enum mLogLevel lv, const char *f, va_list a) {}
static struct mLogger sQuiet = { .log = NoLog };

int main(int argc, char **argv)
{
    mLogSetDefaultLogger(&sQuiet);
    if (argc < 5) { fprintf(stderr, "usage\n"); return 1; }
    const char *rom = argv[1], *sav = argv[2], *out = argv[3];
    int frames = atoi(argv[4]), every = 0, first = 0;
    int shots[256], nshot = 0;
    const char *dumpspec = NULL; // ADDR:LEN:FILE, dumped after the last frame
    int chmask = 0x3F; // audio channels: bits 0-3 PSG, 4-5 FIFO A/B
    const char *loadst = NULL, *writest = NULL, *wav = NULL;
    for (int i = 5; i < argc; i++) {
        if (!strcmp(argv[i], "-p")) {
            char *a = argv[++i];
            int f = atoi(strtok(a, ":"));
            char *k = strtok(NULL, ":");
            int d = atoi(strtok(NULL, ":"));
            char kb[64]; snprintf(kb, sizeof kb, "%s", k);
            presses[npress++] = (struct Press){ f, f + d, ParseKeys(kb) };
        } else if (!strcmp(argv[i], "-s")) shots[nshot++] = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-e")) every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-l")) loadst = argv[++i];
        else if (!strcmp(argv[i], "-w")) writest = argv[++i];
        else if (!strcmp(argv[i], "-f")) first = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-a")) wav = argv[++i];
        else if (!strcmp(argv[i], "-d")) dumpspec = argv[++i];
        else if (!strcmp(argv[i], "-m")) chmask = strtol(argv[++i], NULL, 0);
    }

    struct mCore *core = mCoreFind(rom);
    core->init(core);
    mCoreInitConfig(core, NULL);
    static mColor buf[256 * 160];
    core->setVideoBuffer(core, buf, 256);
    core->setAudioBufferSize(core, 4096);
    mCoreLoadFile(core, rom);
    mCoreLoadSaveFile(core, sav, true); // temporary: never writes the .sav back
    core->reset(core);
    for (int c = 0; c < 6; c++)
        core->enableAudioChannel(core, c, (chmask >> c) & 1);
    if (loadst) {
        size_t n = core->stateSize(core);
        void *st = malloc(n);
        FILE *sf = fopen(loadst, "rb");
        if (!sf || fread(st, 1, n, sf) != n || !core->loadState(core, st)) { fprintf(stderr, "load state failed\n"); return 1; }
        fclose(sf);
        free(st);
    }

    FILE *aw = NULL;
    long asamples = 0;
    if (wav) { aw = fopen(wav, "wb"); fseek(aw, 44, SEEK_SET); }
    int16_t abuf[8192];

    for (int f = first; f < first + frames; f++) {
        unsigned keys = 0;
        for (int i = 0; i < npress; i++)
            if (f >= presses[i].start && f < presses[i].end) keys |= presses[i].mask;
        core->setKeys(core, keys);
        core->runFrame(core);
        if (aw) {
            struct mAudioBuffer *ab = core->getAudioBuffer(core);
            size_t n;
            while ((n = mAudioBufferRead(ab, abuf, 4096)) > 0) { fwrite(abuf, 4, n, aw); asamples += n; }
        }
        int dump = every && f % every == 0;
        for (int i = 0; i < nshot; i++) dump |= shots[i] == f;
        if (dump) {
            char path[512]; snprintf(path, sizeof path, "%s/frame_%05d.ppm", out, f);
            FILE *p = fopen(path, "wb");
            fprintf(p, "P6 240 160 255\n");
            for (int y = 0; y < 160; y++)
                for (int x = 0; x < 240; x++) {
                    uint32_t c = buf[y * 256 + x];
                    unsigned char rgb[3] = { c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF };
                    fwrite(rgb, 1, 3, p);
                }
            fclose(p);
        }
    }
    if (writest) {
        size_t n = core->stateSize(core);
        void *st = calloc(1, n);
        core->saveState(core, st);
        FILE *sf = fopen(writest, "wb");
        fwrite(st, 1, n, sf);
        fclose(sf);
        free(st);
    }
    if (dumpspec) {
        char spec[512]; snprintf(spec, sizeof spec, "%s", dumpspec);
        uint32_t addr = strtoul(strtok(spec, ":"), NULL, 0), len = strtoul(strtok(NULL, ":"), NULL, 0);
        FILE *df = fopen(strtok(NULL, ":"), "wb");
        for (uint32_t i = 0; i < len; i++) fputc(core->busRead8(core, addr + i), df);
        fclose(df);
    }
    if (aw) {
        unsigned rate = 65536, bytes = asamples * 4;
        unsigned char h[44] = "RIFF";
        memcpy(h + 8, "WAVEfmt ", 8);
        uint32_t v; uint16_t s;
        v = 36 + bytes; memcpy(h + 4, &v, 4);
        v = 16; memcpy(h + 16, &v, 4);
        s = 1; memcpy(h + 20, &s, 2); s = 2; memcpy(h + 22, &s, 2);
        memcpy(h + 24, &rate, 4); v = rate * 4; memcpy(h + 28, &v, 4);
        s = 4; memcpy(h + 32, &s, 2); s = 16; memcpy(h + 34, &s, 2);
        memcpy(h + 36, "data", 4); memcpy(h + 40, &bytes, 4);
        fseek(aw, 0, SEEK_SET); fwrite(h, 1, 44, aw); fclose(aw);
    }
    core->deinit(core);
    return 0;
}
