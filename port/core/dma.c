// Software DMA and the BIOS memory-copy calls.
//
// Immediate DMA transfers run at once. VBlank- and HBlank-timed transfers are
// remembered and replayed by the frame loop; pmd-red uses an HBlank DMA to
// stream per-scanline window positions. Sound FIFO DMA ("special" timing) is
// ignored here: the audio backend reads the mixer's buffer directly.

#include <string.h>
#include "port.h"

typedef unsigned int u32;

enum {
    DMA_DEST_INC = 0, DMA_DEST_DEC = 1, DMA_DEST_FIXED = 2, DMA_DEST_RELOAD = 3,
    DMA_SRC_INC = 0, DMA_SRC_DEC = 1, DMA_SRC_FIXED = 2,
    DMA_NOW = 0, DMA_AT_VBLANK = 1, DMA_AT_HBLANK = 2, DMA_SPECIAL = 3,
};

typedef struct {
    int active;
    uintptr_t src, dest, destStart;
    u32 control;
} DmaChannel;

static DmaChannel sDma[4];

static void Mirror(int n, uintptr_t src, uintptr_t dest, u32 control)
{
    unsigned base = 0xB0 + (unsigned)n * 12;
    for (int i = 0; i < 4; i++) {
        gPortRegs[base + i] = (uint8_t)(src >> (i * 8));
        gPortRegs[base + 4 + i] = (uint8_t)(dest >> (i * 8));
        gPortRegs[base + 8 + i] = (uint8_t)(control >> (i * 8));
    }
}

static void Transfer(DmaChannel *ch, int n)
{
    u32 c = ch->control;
    u32 count = c & 0xFFFF;
    if (count == 0)
        count = (n == 3) ? 0x10000 : 0x4000;
    int wide = (c >> 26) & 1;
    int unit = wide ? 4 : 2;
    int destCtl = (c >> 21) & 3, srcCtl = (c >> 23) & 3;
    intptr_t dStep = destCtl == DMA_DEST_DEC ? -unit : destCtl == DMA_DEST_FIXED ? 0 : unit;
    intptr_t sStep = srcCtl == DMA_SRC_DEC ? -unit : srcCtl == DMA_SRC_FIXED ? 0 : unit;

    uintptr_t s = ch->src & ~(uintptr_t)(unit - 1);
    uintptr_t d = ch->dest & ~(uintptr_t)(unit - 1);
    if (dStep == unit && sStep == unit && (s + (uintptr_t)count * unit <= d || d + (uintptr_t)count * unit <= s)) {
        memcpy((void *)d, (const void *)s, (size_t)count * unit);
        s += (uintptr_t)count * unit;
        d += (uintptr_t)count * unit;
    }
    else {
        for (u32 i = 0; i < count; i++, s += sStep, d += dStep) {
            if (wide) *(volatile uint32_t *)d = *(const volatile uint32_t *)s;
            else *(volatile uint16_t *)d = *(const volatile uint16_t *)s;
        }
    }
    ch->src = s;
    ch->dest = (destCtl == DMA_DEST_RELOAD) ? ch->destStart : d;
}

void Port_DmaSet(int n, const void *src, void *dest, u32 control)
{
    DmaChannel *ch = &sDma[n & 3];
    Mirror(n, (uintptr_t)src, (uintptr_t)dest, control);
    ch->active = 0;
    if (!(control & 0x80000000u))
        return;

    ch->src = (uintptr_t)src;
    ch->dest = ch->destStart = (uintptr_t)dest;
    ch->control = control;

    switch ((control >> 28) & 3) {
    case DMA_NOW:
        Transfer(ch, n);
        Mirror(n, (uintptr_t)src, (uintptr_t)dest, control & ~0x80000000u);
        break;
    case DMA_AT_VBLANK:
    case DMA_AT_HBLANK:
        ch->active = 1;
        break;
    case DMA_SPECIAL:
        break; // sound FIFO: handled by the audio path
    }
}

void Port_DmaStop(int n)
{
    sDma[n & 3].active = 0;
    unsigned off = 0xB0 + (unsigned)(n & 3) * 12 + 11;
    gPortRegs[off] &= (uint8_t)~0x80; // clear enable bit
}

static void Fire(int timing)
{
    for (int n = 0; n < 4; n++) {
        DmaChannel *ch = &sDma[n];
        if (!ch->active || (int)((ch->control >> 28) & 3) != timing)
            continue;
        Transfer(ch, n);
        if (!((ch->control >> 25) & 1)) { // not repeating: one shot
            ch->active = 0;
            gPortRegs[0xB0 + n * 12 + 11] &= (uint8_t)~0x80;
        }
    }
}

void Dma_HBlank(void) { Fire(DMA_AT_HBLANK); }
void Dma_VBlank(void) { Fire(DMA_AT_VBLANK); }
void Dma_Reset(void) { memset(sDma, 0, sizeof(sDma)); }

// --- BIOS CpuSet / CpuFastSet ---------------------------------------------

void CpuSet(const void *src, void *dest, u32 control)
{
    u32 count = control & 0x1FFFFF;
    int fixed = (control >> 24) & 1;
    if (control & (1u << 26)) {
        const uint32_t *s = (const uint32_t *)((uintptr_t)src & ~3u);
        uint32_t *d = (uint32_t *)((uintptr_t)dest & ~3u);
        if (fixed) { uint32_t v = *s; for (u32 i = 0; i < count; i++) d[i] = v; }
        else memmove(d, s, (size_t)count * 4);
    }
    else {
        const uint16_t *s = (const uint16_t *)((uintptr_t)src & ~1u);
        uint16_t *d = (uint16_t *)((uintptr_t)dest & ~1u);
        if (fixed) { uint16_t v = *s; for (u32 i = 0; i < count; i++) d[i] = v; }
        else memmove(d, s, (size_t)count * 2);
    }
}

void CpuFastSet(const void *src, void *dest, u32 control)
{
    // Like CpuSet in 32-bit mode, but the count is rounded up to 8 words.
    u32 count = (control & 0x1FFFFF);
    count = (count + 7) & ~7u;
    CpuSet(src, dest, (control & (1u << 24)) | (1u << 26) | count);
}
