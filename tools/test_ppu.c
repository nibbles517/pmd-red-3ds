// Renderer regression test: replays frames captured by the harness
// (tools/harness.py --capture) through the optimized renderer (port/core/ppu.c)
// and the reference one (port/test/ppu_ref.c), and requires byte-identical
// output. Also times both, natively.
//
// build: cc -O2 -o build/test_ppu tools/test_ppu.c port/core/ppu.c port/core/bgcache.c port/test/ppu_ref.c -Iport/core
// usage: build/test_ppu build/captures/capture_*.bin

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "port.h"
#include "bgcache.h"

unsigned char gPortRegs[0x400] __attribute__((aligned(4)));
unsigned char gPortVram[0x18000] __attribute__((aligned(4)));
unsigned char gPortPltt[0x400] __attribute__((aligned(4)));
unsigned char gPortOam[0x400] __attribute__((aligned(4)));
int16_t gPortOamY[128];
int gPortScreenW = 256, gPortScreenH = 192;
struct PortWinX gPortWin1X[SCREEN_H_MAX];

void Ref_BeginFrame(void);
void Ref_RenderLine(int line, uint16_t *dst);

typedef struct {
    uint8_t regs[GBA_H][0x400];
    uint8_t vram[0x18000];
    uint8_t pltt[0x400];
    uint8_t oam[0x400];
} Capture;

static void Render(const Capture *c, uint16_t *out, int ref)
{
    memcpy(gPortVram, c->vram, sizeof(gPortVram));
    memcpy(gPortPltt, c->pltt, sizeof(gPortPltt));
    memcpy(gPortOam, c->oam, sizeof(gPortOam));
    memcpy(gPortRegs, c->regs[0], sizeof(gPortRegs));
    if (ref) Ref_BeginFrame(); else { BgCache_Prepare(gPortRegs, 0); Ppu_BeginFrame(); }
    for (int line = 0; line < GBA_H; line++) {
        memcpy(gPortRegs, c->regs[line], sizeof(gPortRegs));
        if (ref) Ref_RenderLine(line, &out[line * GBA_W]);
        else Ppu_RenderLine(line, &out[line * GBA_W]);
    }
}

static double Seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(int argc, char **argv)
{
    static Capture cap;
    static uint16_t a[GBA_W * GBA_H], b[GBA_W * GBA_H], shown[GBA_W * GBA_H];
    int failures = 0;
    double tRef = 0, tNew = 0;

    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f || fread(&cap, 1, sizeof(cap), f) != sizeof(cap)) { fprintf(stderr, "cannot read %s\n", argv[i]); return 2; }
        fclose(f);

        double t0 = Seconds();
        for (int k = 0; k < 20; k++) Render(&cap, a, 1);
        double t1 = Seconds();
        for (int k = 0; k < 20; k++) Render(&cap, b, 0);
        double t2 = Seconds();
        tRef += t1 - t0;
        tNew += t2 - t1;

        int diff = 0, first = -1;
        for (int p = 0; p < GBA_W * GBA_H; p++)
            if (a[p] != b[p]) { if (first < 0) first = p; diff++; }

        // The frame the game actually showed (written next to the capture).
        char path[1024];
        snprintf(path, sizeof(path), "%.*s.frame", (int)(strlen(argv[i]) - 4), argv[i]);
        int fidelity = -1;
        f = fopen(path, "rb");
        if (f && fread(shown, 1, sizeof(shown), f) == sizeof(shown)) {
            fidelity = 0;
            for (int p = 0; p < GBA_W * GBA_H; p++) fidelity += a[p] != shown[p];
        }
        if (f) fclose(f);

        printf("%-40s %s", argv[i], diff ? "MISMATCH" : "ok");
        if (diff) printf(" (%d pixels, first at x=%d y=%d: ref %04x new %04x)", diff, first % GBA_W, first / GBA_W, a[first], b[first]);
        if (fidelity > 0) printf("  [capture replay differs from shown frame in %d px]", fidelity);
        printf("\n");
        failures += diff != 0;
    }
    printf("\n%d of %d frames match. Native time per frame: reference %.3f ms, optimized %.3f ms (%.1fx)\n",
           argc - 1 - failures, argc - 1, tRef / (argc - 1) / 20 * 1000, tNew / (argc - 1) / 20 * 1000,
           tNew > 0 ? tRef / tNew : 0);
    return failures ? 1 : 0;
}
