// Shadow memory, interrupts and the frame loop.
//
// pmd-red runs its main loop and calls the BIOS VBlankIntrWait() once per
// frame. That call is where the port does a frame's worth of "hardware":
// it raises VBlank, steps through all 228 scanlines firing VCount interrupts
// and HBlank DMA, renders the 160 visible lines, presents, and returns to the
// game for the next frame.

#include <string.h>
#include "port.h"

unsigned char gPortRegs[0x400] __attribute__((aligned(4)));
unsigned char gPortVram[0x18000] __attribute__((aligned(4)));
unsigned char gPortPltt[0x400] __attribute__((aligned(4)));
unsigned char gPortOam[0x400] __attribute__((aligned(4)));
struct SoundInfo *gPortSoundInfoPtr;
unsigned short gPortIntrCheck;
void *gPortIntrVector;

// pmd-red keeps its interrupt handlers in this table (reg_control.c). Indices
// follow its IntrMain: 0 serial/timer3 (link cable), 1 VBlank, 2 VCount,
// 3 timer2, 4 timer3, 5 gamepak.
typedef void (*IntrCallback)(void);
extern IntrCallback gIntrTable[6];

enum {
    REG_DISPSTAT = 0x04, REG_VCOUNT = 0x06, REG_KEYINPUT = 0x130,
    REG_IE = 0x200, REG_IF = 0x202, REG_IME = 0x208,
    INTR_VBLANK = 1 << 0, INTR_VCOUNT = 1 << 2,
    STAT_VBLANK = 1 << 0, STAT_HBLANK = 1 << 1, STAT_VCOUNT = 1 << 2,
    STAT_VBLANK_IE = 1 << 3, STAT_VCOUNT_IE = 1 << 5,
    LINES = 228,
};

// Optional per-scanline hook (test tooling: the harness uses it to capture
// frames for renderer regression tests). NULL in normal builds.
void (*gPortLineHook)(int line);

static uint16_t sFrame[GBA_W * GBA_H];
static uint16_t sShown[GBA_W * GBA_H];
static uint64_t sFrames;
static int sInFrame;     // inside the scanline loop: VCOUNT is the real line
static int sVirtualLine; // outside it: advances on every read so busy-waits end
static int sInHandler;

static inline uint16_t R16(unsigned off) { return (uint16_t)(gPortRegs[off] | (gPortRegs[off + 1] << 8)); }
static inline void W16(unsigned off, uint16_t v) { gPortRegs[off] = (uint8_t)v; gPortRegs[off + 1] = (uint8_t)(v >> 8); }

static void Raise(unsigned bit, int index)
{
    if (sInHandler)
        return; // the GBA masks IRQs inside a handler; pmd-red never re-enables them
    W16(REG_IF, (uint16_t)(R16(REG_IF) | bit));
    if (!(R16(REG_IME) & 1) || !(R16(REG_IE) & bit))
        return;
    IntrCallback cb = gIntrTable[index];
    if (cb == NULL)
        return;
    sInHandler = 1;
    cb();
    sInHandler = 0;
    W16(REG_IF, (uint16_t)(R16(REG_IF) & ~bit));
}

static void SetLine(int line)
{
    uint16_t stat = R16(REG_DISPSTAT);
    W16(REG_VCOUNT, (uint16_t)line);
    stat &= (uint16_t)~(STAT_VBLANK | STAT_VCOUNT | STAT_HBLANK);
    if (line >= 160 && line < 227)
        stat |= STAT_VBLANK;
    if ((stat >> 8) == line) {
        stat |= STAT_VCOUNT;
        W16(REG_DISPSTAT, stat);
        if (stat & STAT_VCOUNT_IE)
            Raise(INTR_VCOUNT, 2);
        return;
    }
    W16(REG_DISPSTAT, stat);
}

void Port_Init(void)
{
    if (Backend_Version4_UpdateRepoIfMissing != 4)
        Backend_Fatal("backend version mismatch");
    memset(gPortRegs, 0, sizeof(gPortRegs));
    memset(gPortVram, 0, sizeof(gPortVram));
    memset(gPortPltt, 0, sizeof(gPortPltt));
    memset(gPortOam, 0, sizeof(gPortOam));
    W16(REG_KEYINPUT, 0x3FF);
    W16(REG_DISPSTAT, 0);
    Dma_Reset();
    sFrames = 0;
    sVirtualLine = 0;
}

uint64_t Port_FrameCount(void) { return sFrames; }
const uint16_t *Port_Framebuffer(void) { return sShown; }

static void UpdateKeys(void)
{
    W16(REG_KEYINPUT, (uint16_t)(~Backend_PollKeys() & 0x3FF));
}

// One full frame of hardware: the visible lines, then VBlank.
//
// On the GBA, VBlankIntrWait returns at the start of VBlank and the game's
// main loop then runs while the next frame is drawn, mostly before the
// scanlines reach the screen areas it changes. So what the main loop writes
// (tilemaps copied from buffers, scroll values) shows in the very next frame.
// Drawing the visible lines here, when the game next waits, keeps that
// ordering; drawing them before returning would delay those writes by a frame
// while palettes and OAM (copied in the VBlank handler) stay on time.
static void RunFrame(void)
{
    sInFrame = 1;

    // Lines 0-159: visible.
    Ppu_BeginFrame();
    for (int line = 0; line < GBA_H; line++) {
        SetLine(line);
        if (gPortLineHook)
            gPortLineHook(line);
        Ppu_RenderLine(line, &sFrame[line * GBA_W]);
        Dma_HBlank();
    }
    memcpy(sShown, sFrame, sizeof(sShown));
    Backend_Present(sShown);

    // Lines 160-227: VBlank.
    SetLine(160);
    if (R16(REG_DISPSTAT) & STAT_VBLANK_IE)
        Raise(INTR_VBLANK, 1);
    Dma_VBlank();
    for (int line = 161; line < LINES; line++)
        SetLine(line);

    sInFrame = 0;
    sVirtualLine = 160; // the game resumes at the start of the next VBlank
    Sound_EndFrame();
    Backend_EndFrame();
    UpdateKeys();
    sFrames++;
}

// --- BIOS calls ----------------------------------------------------------

void VBlankIntrWait(void)
{
    RunFrame();
}

void Port_Halt(void)
{
    RunFrame();
}

void Port_Stop(void)
{
    // GBA sleep mode (pmd-red's L+R+SELECT sleep). The 3DS has its own sleep,
    // so this returns at once.
}

// --- Reads with side effects (see io_reg.h) ------------------------------

unsigned short Port_ReadVCount(void)
{
    if (sInFrame)
        return R16(REG_VCOUNT);
    // Busy-waits like `while (REG_VCOUNT < 160)` must terminate; walk a virtual
    // scanline counter forward on each read.
    sVirtualLine = (sVirtualLine + 1) % LINES;
    return (unsigned short)sVirtualLine;
}

unsigned short Port_ReadKeyInput(void)
{
    if (!sInFrame)
        UpdateKeys();
    return R16(REG_KEYINPUT);
}

// --- Link cable ----------------------------------------------------------

// pmd-red's link-cable code (arm_funcs.s) is copied to RAM and run from there.
// The port has no link cable: these replacements keep its state consistent
// with "no partner connected".
extern struct { uint8_t raw[0x80]; } gUnknown_202DCF8;

unsigned int sub_80001E8(void)
{
    // Swap the two halves of the receive buffer and return the pending flag,
    // as the original does with interrupts masked.
    uint32_t *base = (uint32_t *)gUnknown_202DCF8.raw;
    uint32_t tmp[4];
    memcpy(tmp, &base[16], 16);
    memcpy(&base[16], &base[20], 16);
    memcpy(&base[20], tmp, 16);
    uint32_t pending = base[1];
    base[1] = 0;
    return pending;
}

void sub_8000228(void)
{
    // Serial interrupt handler. Never raised: there is no link hardware.
}

// The GBA-side interrupt dispatcher. pmd-red copies it into RAM at startup
// but the port dispatches interrupts itself, so this is never executed.
void IntrMain(void) {}

// Work RAM that pmd-red's linker script places at 0x3004000 (see
// code_80001DC.c); used as a scratch buffer.
uint32_t gUnknown_3004000[0x3F00 / 4] __attribute__((aligned(4)));
