// Shadow memory, interrupts and the frame loop.
//
// pmd-red runs its main loop and calls the BIOS VBlankIntrWait() once per
// frame. That call is where the port does a frame's worth of "hardware":
// it raises VBlank, steps through all 228 scanlines firing VCount interrupts
// and HBlank DMA, renders the 160 visible lines, presents, and returns to the
// game for the next frame.

#include <string.h>
#include <stdio.h>
#include "port.h"
#include "bgcache.h"
#include "gpulist.h"

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

// Two pictures: the one being drawn, and the last one finished (shown);
// they change places when a frame is drawn (rather than copying it).
static uint16_t sPictures[2][SCREEN_W_MAX * SCREEN_H_MAX];
static uint16_t *sFrame = sPictures[0], *sShown = sPictures[1];
static uint16_t sLine[SCREEN_W_MAX];

// --- Drawing list (gpulist.h) -------------------------------------------------------
//
// With Backend_GpuListMode on, each frame's lines are described for the
// drawing list (Ppu_ListLine) as they go by, with a copy of the registers
// each line had; the frame is then drawn from the list, or, if a line used
// what the list can't express, from those copies the usual way.

static uint64_t sFrames; // (below)
#define REG_COPY 0x60 // the registers the renderer reads (DISPCNT .. BLDY)
static uint8_t sLineRegs[SCREEN_H_MAX][REG_COPY] __attribute__((aligned(4)));
static uint8_t sStartRegs[REG_COPY] __attribute__((aligned(4)));
static struct {
    uint32_t frames, listed, bad, why[GPU_WHY_COUNT];
    uint32_t quads, strips;
    uint32_t pal[3]; // listed frames by GPU_PAL_*
} sListStats;

// The frame drawn the usual way, from the registers' copies
static void DrawFromCopies(int lines, uint16_t *dst, int stride)
{
    Ppu_SetLineRegs(sStartRegs);
    Ppu_BeginFrame();
    for (int y = 0; y < lines; y++) {
        Ppu_SetLineRegs(sLineRegs[y]);
        Ppu_RenderLine(y, &dst[y * stride]);
    }
    Ppu_SetLineRegs(NULL);
}

// For the 3DS's meter: frames listed (drawn from the list), and why the
// others weren't (GPU_WHY_*), since the start
void Port_GpuListCounts(uint32_t *listed, uint32_t why[GPU_WHY_COUNT])
{
    *listed = sListStats.listed;
    memcpy(why, sListStats.why, sizeof(sListStats.why));
}

static void ListFrameDone(int listMode, int listOk, int lines, uint16_t *dst, int stride)
{
    static uint16_t check[SCREEN_W_MAX * SCREEN_H_MAX];
    GpuList *l = &gGpuList;
    sListStats.frames++;
    if (listOk) {
        GpuList_Palette();
        sListStats.listed++;
        sListStats.quads += l->quadCount;
        sListStats.strips += l->stripRows;
        sListStats.pal[l->palette]++;
        if (listMode == 3)
            return; // (the backend draws it: Backend_PresentList)
        GpuList_Draw(l, dst, stride);
        if (listMode == 2) { // checked against the renderer
            DrawFromCopies(lines, check, l->w);
            int bad = 0, fx = -1, fy = -1;
            for (int y = 0; y < lines; y++)
                for (int x = 0; x < l->w; x++)
                    if (dst[y * stride + x] != check[y * l->w + x]) {
                        if (!bad++) { fx = x; fy = y; }
                    }
            if (bad) {
                sListStats.bad++;
                if (sListStats.bad <= 40)
                    Backend_Log("gpu-list: frame %lu: %d pixels differ, first at %d,%d (list %04x, renderer %04x); %d quads",
                                (unsigned long)sFrames, bad, fx, fy, dst[fy * stride + fx], check[fy * l->w + fx], l->quadCount);
            }
        }
    }
    else {
        sListStats.why[l->fallback]++;
        DrawFromCopies(lines, dst, stride);
    }
    if (listMode != 3 && sListStats.frames % 300 == 0) { // (the 3DS: in its own meter's lines)
        char why[160];
        int n = 0;
        why[0] = 0;
        for (int i = 1; i < GPU_WHY_COUNT && n < (int)sizeof(why) - 24; i++)
            if (sListStats.why[i])
                n += snprintf(why + n, sizeof(why) - n, " %s %lu", gGpuWhy[i], (unsigned long)sListStats.why[i]);
        Backend_Log("gpu-list: %lu frames, %lu listed (%lu differ), %lu quads and %lu strip rows a frame; palettes: fade %lu other %lu; others:%s",
                    (unsigned long)sListStats.frames, (unsigned long)sListStats.listed, (unsigned long)sListStats.bad,
                    (unsigned long)(sListStats.listed ? sListStats.quads / sListStats.listed : 0),
                    (unsigned long)(sListStats.listed ? sListStats.strips / sListStats.listed : 0),
                    (unsigned long)sListStats.pal[GPU_PAL_FADE], (unsigned long)sListStats.pal[GPU_PAL_OTHER], why);
    }
}
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
    if (Backend_Version18_UpdateRepoIfMissing != 18)
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
extern uint8_t gPortRawXY;
uint8_t gPortFieldControl; // set by the game each frame the player walks freely (no menu)

// DS and 3DS buttons: X is A in menus, and B where the player walks freely
// (it opens the menu there), whichever it was when pressed until released.
static void UpdateKeys(void)
{
    static uint16_t sXAs;
    uint16_t keys = Backend_PollKeys();
    if (!(keys & KEY_BIT_X) || !Port_DsButtons() || gPortRawXY)
        sXAs = 0;
    else if (sXAs == 0)
        sXAs = gPortFieldControl ? KEY_BIT_B : KEY_BIT_A;
    if (gPortFieldControl)
        gPortFieldControl--;
    keys |= sXAs;
    if (!gPortRawXY) { // (Music Options' player: the volume instead)
        if (keys & KEY_BIT_CPAD_UP) keys |= KEY_BIT_UP;
        if (keys & KEY_BIT_CPAD_DOWN) keys |= KEY_BIT_DOWN;
    }
    sKeysHeld = keys;
    W16(REG_KEYINPUT, (uint16_t)(~keys & 0x3FF));
}

// Y (DS and 3DS controls: held for Rotate Mode), as of the last frame
uint8_t gPortRawXY; // Music Options: X and Y are its own (stop, pause)

unsigned Port_KeysHeld(void)
{
    return sKeysHeld;
}

uint8_t Port_YHeld(void)
{
    return (sKeysHeld & KEY_BIT_Y) != 0;
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

// The picture is black: every palette entry the last listed frame showed
// is (the end of a fade out: the game leaves a few entries it doesn't fade,
// the text box's, unshown), the backdrop too, and no brightening (blending
// black with black stays black); not forced blank. What the next frames
// show is not known before they are described, so at most 3 frames in a row
// are taken as black this way.
// Every palette entry of the banks `used` (bits 0-15 BG, 16-31 OBJ) is
// black, the backdrop too (colour 0 of the other banks: clear, not shown)
static int BanksBlack(uint32_t used)
{
    const uint16_t *p = (const uint16_t *)gPortPltt;
    used |= 1; // (the backdrop: entry 0)
    for (int i = 0; i < 512; i++)
        if ((p[i] & 0x7FFF) && ((used >> (i >> 4)) & 1) && ((i & 15) || i == 0))
            return 0;
    return 1;
}

static int sLastListed, sBlackRun;
static int sShowingBlack, sHeld; // the screen shows black; frames held black since (RunFrame)
static int PictureBlack(void)
{
    if (!sLastListed || sBlackRun >= 3)
        return 0;
    if (R16(0x00) & 0x80)
        return 0;
    if (((R16(0x50) >> 6) & 3) == 2)
        return 0;
    return BanksBlack(gGpuList.usedBanks);
}

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
    // A black picture (the end of a fade out: every palette entry black,
    // no brightening): with the GPU drawing, an empty list faded to black,
    // drawn at once (Backend_PresentList), instead of describing a frame
    // nothing of which shows (the next map's tiles, loaded while the screen
    // is black, are decoded when it fades in)
    int black = !skip && mode == sViewMode && SCREEN_W == sViewW && PictureBlack() && Backend_GpuListMode() == 3;
    sBlackRun = black ? sBlackRun + 1 : 0;
    if (black)
        skip = 1; // (not drawn: below)

    sInFrame = 1;
    if (mode != sViewMode || SCREEN_W != sViewW) {
        sViewMode = mode;
        sViewW = SCREEN_W;
        Ppu_SetView(wide);
        memset(sPictures, 0, sizeof(sPictures)); // the framed view's border is black (both pictures)
    }
    int lines = wide ? SCREEN_H : GBA_H;
    if (!skip)
        BgCache_Prepare(gPortRegs, wide); // (before the first line)
    if (!skip)
        Ppu_BeginFrame();
    int listMode = skip ? 0 : Backend_GpuListMode(), listOk = 0;
    if (listMode) {
        memcpy(sStartRegs, gPortRegs, REG_COPY);
        GpuList_Begin(wide ? SCREEN_W : GBA_W, lines);
        listOk = 1;
    }

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
        else if (listMode) { // described (or drawn below, from the copy)
            memcpy(sLineRegs[line], gPortRegs, REG_COPY);
            if (listOk && line == 0)
                listOk = Ppu_ListObjects(lines);
            if (listOk)
                listOk = Ppu_ListLine(line);
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
    if (listMode)
        sLastListed = listMode == 3 && listOk;
    if (listMode) {
        if (mode == VIEW_GBA)
            ListFrameDone(listMode, listOk, lines, sFrame, GBA_W);
        else if (wide)
            ListFrameDone(listMode, listOk, lines, sFrame, SCREEN_W);
        else
            ListFrameDone(listMode, listOk, lines, &sFrame[NARROW_OY * SCREEN_W + NARROW_OX], SCREEN_W);
    }
    Backend_Mark(PORT_MARK_DRAWN, skip && !black);
    LoadWide(&live); // the game's again
    sLatched = live;
    sLatchValid = 1;

    if (!skip)
    {
        uint16_t *t = sShown; // (the new picture is the shown one; the old one is drawn over next)
        sShown = sFrame;
        sFrame = t;
    }
    // Coming out of black (a fade in, a new map: its tiles decoded and sent
    // to the GPU on its first frames, 20-30 ms each on an Old 3DS), the
    // first 2 frames are described and their textures sent, but the screen
    // stays black: the fade then starts from frames that are ready instead
    // of on late ones.
    int hold = 0;
    if (black) {
        static GpuList sBlack; // (an empty list faded to black: no rectangles, the backdrop)
        sBlack.w = wide ? SCREEN_W : GBA_W;
        sBlack.h = lines;
        sBlack.backdrop = 0;
        sBlack.bdSecond = 0;
        sBlack.eva = 16;
        sBlack.evb = 0;
        memcpy(sBlack.base, gGpuList.base, sizeof(sBlack.base)); // (the textures' colours stay)
        sBlack.fadeOn = 1;
        sBlack.fade.mul = 0;
        memset(sBlack.fade.add, 0, sizeof(sBlack.fade.add));
        sBlack.palette = GPU_PAL_STEADY;
        sBlack.objBanks = 0;
        sBlack.quadCount = 0;
        sBlack.stripRows = 0;
        sBlack.frame = gGpuList.frame;
        sBlack.full = sBlack.fallback = 0;
        sShowingBlack = 1;
        sHeld = 0;
        if (mode == VIEW_GBA)
            Backend_PresentList(&sBlack, GBA_W, GBA_H, 0, 0);
        else if (wide)
            Backend_PresentList(&sBlack, SCREEN_W, SCREEN_H, 0, 0);
        else
            Backend_PresentList(&sBlack, SCREEN_W, SCREEN_H, NARROW_OX, NARROW_OY);
    }
    else if (listMode == 3 && listOk) {
        const GpuList *l = &gGpuList;
        // (black: faded to it, or the palettes it shows are black as they are)
        int nowBlack = (l->fadeOn && l->fade.mul == 0 && !(l->fade.add[0] | l->fade.add[1] | l->fade.add[2]))
                       || (((R16(0x50) >> 6) & 3) != 2 && BanksBlack(l->usedBanks)); // (not brightened)
        if (nowBlack)
            sHeld = 0;
        hold = sShowingBlack && !nowBlack && sHeld < 2;
        if (hold) {
            sHeld++;
            Backend_UploadList(l);
        }
        else
            sShowingBlack = nowBlack;
    }
    else if (!skip) {
        sShowingBlack = 0;
    }
    if (black) {
        // (shown above)
    }
    else if (hold) {
        Backend_Present(NULL, mode == VIEW_GBA ? GBA_W : SCREEN_W, mode == VIEW_GBA ? GBA_H : SCREEN_H); // (the screen stays black)
    }
    else if (listMode == 3 && listOk) { // the backend draws the list (on the GPU)
        if (mode == VIEW_GBA)
            Backend_PresentList(&gGpuList, GBA_W, GBA_H, 0, 0);
        else if (wide)
            Backend_PresentList(&gGpuList, SCREEN_W, SCREEN_H, 0, 0);
        else
            Backend_PresentList(&gGpuList, SCREEN_W, SCREEN_H, NARROW_OX, NARROW_OY);
    }
    else if (mode == VIEW_GBA)
        Backend_Present(skip ? NULL : sShown, GBA_W, GBA_H);
    else
        Backend_Present(skip ? NULL : sShown, SCREEN_W, SCREEN_H);
    Backend_Mark(PORT_MARK_PRESENTED, skip && !black);

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
    Backend_Mark(PORT_MARK_VBLANK_DONE, skip && !black);
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
