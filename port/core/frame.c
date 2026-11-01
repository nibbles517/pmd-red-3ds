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

static uint16_t sFrame[SCREEN_W_MAX * SCREEN_H_MAX];
static uint16_t sShown[SCREEN_W_MAX * SCREEN_H_MAX];
static uint16_t sLine[SCREEN_W_MAX];
enum { VIEW_GBA, VIEW_FRAMED, VIEW_WIDE };
static int sViewMode = -1; // view of the last frame
static int sViewW;         // ... and its width
uint8_t gPortWideView;
int16_t gPortOamY[128];
struct PortWinX gPortWin1X[SCREEN_H_MAX];
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
    if (Backend_Version11_UpdateRepoIfMissing != 11)
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
    Port_LoadConfig();
}

uint64_t Port_FrameCount(void) { return sFrames; }
const uint16_t *Port_Framebuffer(void);

static uint16_t sKeysHeld;
uint8_t gPortFieldControl; // set by the game each frame the player walks freely (no menu)

// DS and 3DS buttons: X is A in menus, and B where the player walks freely
// (it opens the menu there), whichever it was when pressed until released.
static void UpdateKeys(void)
{
    static uint16_t sXAs;
    uint16_t keys = Backend_PollKeys();
    if (!(keys & KEY_BIT_X) || !Port_DsButtons())
        sXAs = 0;
    else if (sXAs == 0)
        sXAs = gPortFieldControl ? KEY_BIT_B : KEY_BIT_A;
    if (gPortFieldControl)
        gPortFieldControl--;
    keys |= sXAs;
    sKeysHeld = keys;
    W16(REG_KEYINPUT, (uint16_t)(~keys & 0x3FF));
}

// Y (DS and 3DS controls: held for Rotate Mode), as of the last frame
uint8_t Port_YHeld(void)
{
    return (sKeysHeld & KEY_BIT_Y) != 0;
}

// --- Dual Core ------------------------------------------------------------
//
// Graphics Options > Dual Core: On. While the frame's lines go by (and the
// VCount and HBlank events change the registers), the registers each line
// has are kept; then the picture is drawn by two renderers at once, the main
// CPU core drawing the top lines and the second core the rest. VRAM, the
// palettes and OAM do not change until VBlank, so both read them as they are.
// The split moves a little each frame towards the core that finished first.

#define REG_COPY 0x60 // the registers the renderer reads (DISPCNT .. BLDY)
static uint8_t sLineRegs[SCREEN_H_MAX][REG_COPY] __attribute__((aligned(4)));
static uint8_t sStartRegs[REG_COPY] __attribute__((aligned(4)));
static int sSplit = 96;    // the first line the second core draws
static int sDualViewWide = -1, sDualViewW;

typedef struct {
    int first, lines, mode, wide;
} Part;

static void DrawPartB(void *arg)
{
    const Part *p = arg;
    static uint16_t line[SCREEN_W_MAX];
    if (p->wide != sDualViewWide || SCREEN_W != sDualViewW) {
        PpuB_SetView(p->wide);
        sDualViewWide = p->wide;
        sDualViewW = SCREEN_W;
    }
    PpuB_SetLineRegs(sStartRegs);
    PpuB_BeginFrame();
    for (int y = 0; y < p->lines; y++) {
        PpuB_SetLineRegs(sLineRegs[y]);
        if (y < p->first)
            PpuB_SkipLine(y);
        else if (p->mode == VIEW_GBA)
            PpuB_RenderLine(y, &sFrame[y * GBA_W]);
        else if (p->wide)
            PpuB_RenderLine(y, &sFrame[y * SCREEN_W]);
        else {
            PpuB_RenderLine(y, line);
            memcpy(&sFrame[(y + NARROW_OY) * SCREEN_W + NARROW_OX], line, GBA_W * sizeof(uint16_t));
        }
    }
    PpuB_SetLineRegs(NULL);
}

static void DrawDual(int mode, int wide, int lines)
{
    static Part part;
    int split = sSplit;
    if (split > lines - 8) split = lines - 8;
    if (split < 8) split = 8;
    part.first = split;
    part.lines = lines;
    part.mode = mode;
    part.wide = wide;
    Backend_RunAsync(DrawPartB, &part);

    Ppu_SetLineRegs(sStartRegs);
    Ppu_BeginFrame();
    for (int y = 0; y < split; y++) {
        Ppu_SetLineRegs(sLineRegs[y]);
        if (mode == VIEW_GBA)
            Ppu_RenderLine(y, &sFrame[y * GBA_W]);
        else if (wide)
            Ppu_RenderLine(y, &sFrame[y * SCREEN_W]);
        else {
            Ppu_RenderLine(y, sLine);
            memcpy(&sFrame[(y + NARROW_OY) * SCREEN_W + NARROW_OX], sLine, GBA_W * sizeof(uint16_t));
        }
    }
    Ppu_SetLineRegs(NULL);

    // Whichever core finished first takes a couple more lines next frame.
    if (Backend_AsyncDone())
        sSplit = split - 2;
    else
        sSplit = split + 2;
    Backend_WaitAsync();
}

// The wide view's state the game sets (port_view.h), kept a frame (RunFrame)
typedef struct {
    struct PortWideBg bg[4];
    uint8_t tile[4];
    uint8_t hudLines, hudLeft;
} WideState;
static WideState sLatched;
static int sLatchValid;

static void SaveWide(WideState *w)
{
    memcpy(w->bg, gPortWideBg, sizeof(w->bg));
    memcpy(w->tile, gPortTileBg, sizeof(w->tile));
    w->hudLines = gPortHudLines;
    w->hudLeft = gPortHudLeft;
}

static void LoadWide(const WideState *w)
{
    memcpy(gPortWideBg, w->bg, sizeof(w->bg));
    memcpy(gPortTileBg, w->tile, sizeof(w->tile));
    gPortHudLines = w->hudLines;
    gPortHudLeft = w->hudLeft;
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
    Backend_Mark(PORT_MARK_FRAME_START, 0);
    // Main Screen Settings > Size: GBA, the game's 240x160 picture as it is;
    // DS (256x192) or Fit (320x240), the GBA picture in the middle, or the
    // wide view while the game keeps asking for it (it sets gPortWideView
    // every frame it draws a dungeon floor or a ground map, which it does
    // only in those sizes).
    Port_UpdateScreenSize();
    int ds = Port_WideSize();
    int wide = ds && gPortWideView != 0;
    int mode = !ds ? VIEW_GBA : wide ? VIEW_WIDE : VIEW_FRAMED;
    if (gPortWideView)
        gPortWideView--;
    if (!wide) {
        memset(gPortWideBg, 0, sizeof(gPortWideBg));
        memset(gPortTileBg, 0, sizeof(gPortTileBg));
        gPortHudLines = 0;
    }
    // What the game set for the wide view this frame is drawn in the next
    // one: its BG registers and OAM reach the hardware at VBlank, after this
    // frame is drawn, so the wide view's backgrounds wait as long, keeping
    // in step with the sprites.
    WideState live;
    SaveWide(&live);
    if (!sLatchValid)
        sLatched = live;
    LoadWide(&sLatched);

    int dual = Port_DualCore();

    // Graphics Options > Frame Skip: the game runs every frame, but the
    // picture is not drawn on some (the main screen keeps the last one).
    // Always: every other frame. Auto: after a frame that took too long (so
    // the game keeps its speed when drawing is too slow), at most
    // MAX_SKIPPED frames in a row.
    enum { MAX_SKIPPED = 2 };
    static int sSkipped; // frames skipped in a row
    int skip = 0;
    switch (Port_FrameSkipMode()) {
        case 1: skip = Backend_FrameLate() && sSkipped < MAX_SKIPPED; break;
        case 2: skip = sSkipped == 0; break;
    }
    if (mode != sViewMode || SCREEN_W != sViewW)
        skip = 0; // a new view is drawn at once
    sSkipped = skip ? sSkipped + 1 : 0;

    sInFrame = 1;
    if (mode != sViewMode || SCREEN_W != sViewW) {
        sViewMode = mode;
        sViewW = SCREEN_W;
        Ppu_SetView(wide);
        memset(sFrame, 0, sizeof(sFrame)); // the framed view's border is black
    }
    int lines = wide ? SCREEN_H : GBA_H;
    if (!skip && dual)
        memcpy(sStartRegs, gPortRegs, REG_COPY);
    else if (!skip)
        Ppu_BeginFrame();

    // Lines 0-159: visible. The wide view also draws lines below, as if the
    // screen went on (no VCount events there, but the HBlank DMA continues).
    for (int line = 0; line < lines; line++) {
        if (line < GBA_H) {
            SetLine(line);
            if (gPortLineHook)
                gPortLineHook(line);
        }
        if (skip) {
            // not drawn
        }
        else if (dual) {
            memcpy(sLineRegs[line], gPortRegs, REG_COPY); // drawn below
        }
        else if (mode == VIEW_GBA) {
            Ppu_RenderLine(line, &sFrame[line * GBA_W]);
        }
        else if (wide) {
            Ppu_RenderLine(line, &sFrame[line * SCREEN_W]);
        }
        else {
            Ppu_RenderLine(line, sLine);
            memcpy(&sFrame[(line + NARROW_OY) * SCREEN_W + NARROW_OX], sLine, GBA_W * sizeof(uint16_t));
        }
        Dma_HBlank();
    }
    if (!skip && dual)
        DrawDual(mode, wide, lines);
    Backend_Mark(PORT_MARK_DRAWN, skip);
    LoadWide(&live); // the game's again
    sLatched = live;
    sLatchValid = 1;

    if (!skip)
        memcpy(sShown, sFrame, (mode == VIEW_GBA ? GBA_W * GBA_H : SCREEN_W * SCREEN_H) * sizeof(uint16_t));
    if (mode == VIEW_GBA)
        Backend_Present(skip ? NULL : sShown, GBA_W, GBA_H);
    else
        Backend_Present(skip ? NULL : sShown, SCREEN_W, SCREEN_H);
    Backend_Mark(PORT_MARK_PRESENTED, skip);

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
    Backend_Mark(PORT_MARK_VBLANK_DONE, skip);
    Backend_EndFrame();
    UpdateKeys();
    sFrames++;
}

const uint16_t *Port_Framebuffer(void) { return sShown; }

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
