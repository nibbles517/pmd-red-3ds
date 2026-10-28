// Nintendo 3DS backend (libctru).
//
// The game's picture goes on the bottom screen by default, as on the DS, or
// on the top screen (Others > Screen/Layout Options > Main Screen). In GBA
// size it is the 240x160 picture (1:1, 4/3x to 320x213 on the bottom screen,
// or 1.5x to 360x240 on the top); in DS size (Main Screen Settings > Size),
// 256x192 with a wider view of dungeons and ground maps (1:1, or 1.25x to
// 320x240). Y toggles the scaling. The other screen shows the
// DS-style second screen at 256x192 (team panel in dungeons, world map
// elsewhere), or a short status text when there is none (title screen).
// Sound goes through the DSP (needs sdmc:/3ds/dspfirm.cdc, which most custom
// firmware setups already have).
// The save is sdmc:/3ds/pmd-red-3ds/pmd-red.sav, a standard 128 KiB GBA save.

#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "../core/port.h"

#define DATA_DIR "sdmc:/3ds/pmd-red-3ds"
#define SAVE_PATH DATA_DIR "/pmd-red.sav"
#define LOG_PATH DATA_DIR "/port.log"
#define CONFIG_PATH DATA_DIR "/config.bin"
#define DEBUG_FLAG DATA_DIR "/debug.txt"

void AgbMain(void);

const int Backend_Version9_UpdateRepoIfMissing = 9;

static uint16_t sLut[0x8000]; // GBA BGR555 -> RGB565 (fades start from it)
static uint16_t sDimLut[0x8000]; // the same, darkened during a screen change fade
static const uint16_t *sPresentLut; // NULL: full brightness (ToRgb565)

// GBA BGR555 to RGB565, as sLut holds it. Worked out per pixel rather than
// looked up: the 64 KiB table does not stay in the CPU's cache.
static inline uint16_t ToRgb565(uint16_t c)
{
    return (uint16_t)(((c & 0x1F) << 11) | ((c & 0x3E0) << 1) | ((c >> 4) & 0x20) | ((c >> 10) & 0x1F));
}
#define PIX(c) (lut ? lut[(c) & 0x7FFF] : ToRgb565(c))
static int sScaled;
static int sBottom; // picture on the bottom screen
static FILE *sLog;
static bool sLate; // the last frame missed its VBlank (see Pacing)
static PrintConsole sConsole;
static int sAudioOn;

// --- Video -------------------------------------------------------------------

static void BuildLut(void)
{
    for (unsigned c = 0; c < 0x8000; c++) {
        unsigned r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
        unsigned g6 = (g << 1) | (g >> 4);
        sLut[c] = (uint16_t)((r << 11) | (g6 << 5) | b);
    }
}

// Switching screens fades the picture to black, moves it, and fades it back
// in, like the game's own fades.
#define FADE_FRAMES 16
static int sFadeLevel = FADE_FRAMES; // FADE_FRAMES = full brightness
static int sFadeDir;                 // -1 fading out, +1 fading in

static void SetFadeLevel(int level)
{
    sFadeLevel = level;
    if (level >= FADE_FRAMES) {
        sPresentLut = NULL;
        return;
    }
    for (unsigned c = 0; c < 0x8000; c++) {
        unsigned r = (c & 31) * level / FADE_FRAMES;
        unsigned g = ((c >> 5) & 31) * level / FADE_FRAMES;
        unsigned b = ((c >> 10) & 31) * level / FADE_FRAMES;
        sDimLut[c] = sLut[r | (g << 5) | (b << 10)];
    }
    sPresentLut = sDimLut;
}

static void ClearGameScreen(void);

// Copies (and converts) a picture to a framebuffer: output pixels x0..x1-1,
// y0..y1-1 of an area at (ox, oy), each from source pixel (xmap[x], ymap[y]).
//
// Framebuffers are 240 pixels per column, columns left to right, each column
// stored bottom to top. Going down a source column one pixel at a time would
// read every 512 bytes in a DS-size picture (256 pixels a row), which the
// Old 3DS's cache cannot hold on to (they all fall in the same few cache
// sets); writing across columns one pixel at a time is slow too. So it goes
// in strips of 16 columns: the strip is read row by row into a small buffer
// that stays in the cache, turned on its side, then each framebuffer column
// is written in one go.
static uint16_t sIdentity[400];
static uint16_t sMapX[400], sMapY[240];

static void Blit(const uint16_t *frame, int srcW, uint16_t *fb, int ox, int oy,
                 int x0, int y0, int x1, int y1, const uint16_t *xmap, const uint16_t *ymap,
                 const uint16_t *lut)
{
    enum { STRIP = 16 };
    static uint16_t strip[STRIP][240] __attribute__((aligned(8))); // [column][y]
    for (int xs = x0; xs < x1; xs += STRIP) {
        int n = xs + STRIP < x1 ? STRIP : x1 - xs;
        for (int y = y0; y < y1; y++) {
            const uint16_t *row = frame + ymap[y] * srcW;
            for (int i = 0; i < n; i++)
                strip[i][y] = PIX(row[xmap[xs + i]]);
        }
        for (int i = 0; i < n; i++) {
            uint16_t *col = fb + (ox + xs + i) * 240 + (239 - oy); // y = 0 here, y grows downwards in memory
            const uint16_t *src = strip[i];
            for (int y = y0; y < y1; y++)
                col[-y] = src[y];
        }
    }
}

static void BuildMaps(void)
{
    for (int i = 0; i < 400; i++)
        sIdentity[i] = (uint16_t)i;
}

// The scaling maps (out x -> source x, out y -> source y) for a ratio num/den.
static void ScaleMaps(int outW, int outH, int num, int den)
{
    static int sNum, sDen, sW, sH;
    if (num == sNum && den == sDen && outW == sW && outH == sH)
        return;
    sNum = num; sDen = den; sW = outW; sH = outH;
    for (int x = 0; x < outW; x++)
        sMapX[x] = (uint16_t)(x * den / num);
    for (int y = 0; y < outH; y++)
        sMapY[y] = (uint16_t)(y * den / num);
}

// GBA size: the 240x160 picture, 1:1 or scaled (4/3x to 320x213 on the
// bottom screen, 1.5x to 360x240 on the top).
static void PresentGba(const uint16_t *frame, uint16_t *fb, int width, const uint16_t *lut)
{
    if (!sScaled) {
        const int ox = (width - GBA_W) / 2, oy = (240 - GBA_H) / 2;
        Blit(frame, GBA_W, fb, ox, oy, 0, 0, GBA_W, GBA_H, sIdentity, sIdentity, lut);
        return;
    }
    if (sBottom) {
        // 4/3x to 320x213: every source pixel becomes 1 or 2 screen pixels.
        ScaleMaps(320, 213, 4, 3);
        Blit(frame, GBA_W, fb, 0, (240 - 213) / 2, 0, 0, 320, 213, sMapX, sMapY, lut);
        return;
    }
    // 1.5x: every source pixel becomes 1 or 2 screen pixels in each direction.
    ScaleMaps(360, 240, 3, 2);
    Blit(frame, GBA_W, fb, (400 - 360) / 2, 0, 0, 0, 360, 240, sMapX, sMapY, lut);
}

// DS size: the 256x192 picture, 1:1 or 1.25x to 320x240.
static void PresentDs(const uint16_t *frame, uint16_t *fb, int width, const uint16_t *lut)
{
    if (!sScaled) {
        const int ox = (width - SCREEN_W) / 2, oy = (240 - SCREEN_H) / 2;
        Blit(frame, SCREEN_W, fb, ox, oy, 0, 0, SCREEN_W, SCREEN_H, sIdentity, sIdentity, lut);
        return;
    }
    // 1.25x: every source pixel becomes 1 or 2 screen pixels.
    ScaleMaps(320, 240, 5, 4);
    Blit(frame, SCREEN_W, fb, (width - 320) / 2, 0, 0, 0, 320, 240, sMapX, sMapY, lut);
}

static void PresentMain(const uint16_t *frame, int frameW)
{
    // Framebuffers are 240 pixels per column, columns left to right, each
    // column stored bottom to top. Top: 400 columns; bottom: 320.
    static int sLastW;
    if (frameW != sLastW) { // size changed: clear what the old picture left
        sLastW = frameW;
        ClearGameScreen();
    }
    gfxScreen_t screen = sBottom ? GFX_BOTTOM : GFX_TOP;
    const int width = sBottom ? 320 : 400;
    uint16_t *fb = (uint16_t *)gfxGetFramebuffer(screen, GFX_LEFT, NULL, NULL);
    if (frameW == GBA_W)
        PresentGba(frame, fb, width, sPresentLut);
    else
        PresentDs(frame, fb, width, sPresentLut);
}

// --- Performance meter ---------------------------------------------------------
//
// With sdmc:/3ds/pmd-red-3ds/debug.txt present, port.log gets a line every 5
// seconds: frames per second, and the average time per frame of each part:
//   game       the game's own code (between frames)
//   draw       the main screen's picture (port/core/ppu.c)
//   out        converting it to the 3DS screen
//   sub        drawing the second screen; subout: converting it
//   vblank     the game's VBlank work and the sound
//   flush      flushing the screens to memory (and writing the save)
//   wait       waiting for the screens' refresh (spare time)
enum { T_GAME, T_DRAW, T_OUT, T_SUB, T_SUBOUT, T_VBLANK, T_FLUSH, T_WAIT, T_COUNT };
static const char *const sPartNames[T_COUNT] = { "game", "draw", "out", "sub", "subout", "vblank", "flush", "wait" };
static u64 sPartTicks[T_COUNT];
static u64 sMarkTick, sMeterStart, sMeterBoot;
static int sMeterFrames, sMeterSkipped, sMeterLate;

static void Part(int part)
{
    u64 now = svcGetSystemTick();
    if (sMarkTick != 0)
        sPartTicks[part] += now - sMarkTick;
    sMarkTick = now;
}

// --- Second screen -------------------------------------------------------------
//
// The screen without the game shows the DS-style second screen (team panel,
// world map) when the game has one to show, else the status text.

enum { SUB_UNSET, SUB_TEXT, SUB_PICTURE };
static int sSubMode;
static uint16_t sSubFrame[SUBSCREEN_W * SUBSCREEN_H];

static void PrintStatus(void);

static void PresentSub(void)
{
    gfxScreen_t screen = sBottom ? GFX_TOP : GFX_BOTTOM;
    const int width = sBottom ? 400 : 320;

    bool shown = Port_RenderSubScreen(sSubFrame);
    Part(T_SUB);
    if (!shown) {
        if (sSubMode != SUB_TEXT) {
            consoleInit(screen, &sConsole);
            PrintStatus();
            sSubMode = SUB_TEXT;
        }
        return;
    }
    // The second screen is double-buffered: what changed is drawn into this
    // frame's buffer, and again into the other one next frame (it missed it).
    // While nothing changes, neither is touched.
    static int sSubFade = -1;
    static int sPend[4]; // the area the other buffer still lacks (x0, y0, x1, y1)
    int area[4] = { 0, 0, 0, 0 };
    int full = sFadeLevel != sSubFade;
    sSubFade = sFadeLevel;
    if (sSubMode != SUB_PICTURE) {
        full = 1;
        gfxSetScreenFormat(screen, GSP_RGB565_OES);
        gfxSetDoubleBuffering(screen, true);
        for (int i = 0; i < 2; i++) { // both buffers, so the border stays black
            memset(gfxGetFramebuffer(screen, GFX_LEFT, NULL, NULL), 0, width * 240 * 2);
            gfxScreenSwapBuffers(screen, false);
        }
        sSubMode = SUB_PICTURE;
    }
    if (full) {
        area[2] = SUBSCREEN_W; area[3] = SUBSCREEN_H;
    } else if (Port_SubScreenChanged()) {
        Port_SubScreenChangedArea(&area[0], &area[1], &area[2], &area[3]);
    }
    int draw[4] = { area[0], area[1], area[2], area[3] };
    if (sPend[0] < sPend[2]) { // add what the other buffer lacks
        if (draw[0] >= draw[2]) {
            memcpy(draw, sPend, sizeof(draw));
        } else {
            if (sPend[0] < draw[0]) draw[0] = sPend[0];
            if (sPend[1] < draw[1]) draw[1] = sPend[1];
            if (sPend[2] > draw[2]) draw[2] = sPend[2];
            if (sPend[3] > draw[3]) draw[3] = sPend[3];
        }
    }
    memcpy(sPend, area, sizeof(sPend));
    if (draw[0] >= draw[2] || draw[1] >= draw[3])
        return;

    const uint16_t *lut = sPresentLut;
    uint16_t *fb = (uint16_t *)gfxGetFramebuffer(screen, GFX_LEFT, NULL, NULL);
    const int ox = (width - SUBSCREEN_W) / 2, oy = (240 - SUBSCREEN_H) / 2;
    Blit(sSubFrame, SUBSCREEN_W, fb, ox, oy, draw[0], draw[1], draw[2], draw[3], sIdentity, sIdentity, lut);
    Part(T_SUBOUT);
}

static int sMainPresented; // this frame drew the main screen (else it keeps its buffers)

void Backend_Mark(int mark, int skipped)
{
    switch (mark) {
        case PORT_MARK_FRAME_START: Part(T_GAME); break;
        case PORT_MARK_DRAWN: Part(T_DRAW); sMeterSkipped += skipped; break;
        case PORT_MARK_PRESENTED: sMarkTick = svcGetSystemTick(); break; // (timed in Backend_Present)
        case PORT_MARK_VBLANK_DONE: Part(T_VBLANK); break;
    }
}

static void MeterEndFrame(void)
{
    u64 now = svcGetSystemTick();
    if (sMeterStart == 0) {
        sMeterStart = sMeterBoot = now;
        return;
    }
    sMeterFrames++;
    sMeterLate += sLate;
    if (now - sMeterStart < 5 * (u64)SYSCLOCK_ARM11)
        return;
    if (sLog != NULL) {
        char line[512];
        double ms = 1000.0 / SYSCLOCK_ARM11, secs = (now - sMeterStart) / (double)SYSCLOCK_ARM11;
        bool isNew = false;
        APT_CheckNew3DS(&isNew);
        int n = snprintf(line, sizeof(line), "perf t=%us %s %s fps=%.1f drawn=%d/%d late=%d %s |",
            (unsigned)((now - sMeterBoot) / SYSCLOCK_ARM11), isNew ? "New3DS" : "Old3DS", Port_SceneName(),
            sMeterFrames / secs, sMeterFrames - sMeterSkipped, sMeterFrames, sMeterLate, Port_SettingsSummary());
        for (int i = 0; i < T_COUNT && n < (int)sizeof(line); i++)
            n += snprintf(line + n, sizeof(line) - n, " %s %.1f", sPartNames[i], sPartTicks[i] * ms / sMeterFrames);
        if (n < (int)sizeof(line))
            snprintf(line + n, sizeof(line) - n, " ms/frame");
        fprintf(sLog, "%s\n", line);
        fflush(sLog);
    }
    memset(sPartTicks, 0, sizeof(sPartTicks));
    sMeterFrames = sMeterSkipped = sMeterLate = 0;
    sMeterStart = now;
}

void Backend_Present(const uint16_t *frame, int width, int height)
{
    (void)height;
    // NULL: a frame Frame Skip did not draw. During the screen change fade
    // the last picture is shown again (the fade changes its colours).
    if (frame == NULL && sFadeDir != 0)
        frame = Port_Framebuffer();
    sMainPresented = frame != NULL;
    sMarkTick = svcGetSystemTick();
    if (frame != NULL)
        PresentMain(frame, width);
    Part(T_OUT);
    PresentSub(); // (times T_SUB and T_SUBOUT)
}

// --- Pacing ------------------------------------------------------------------
//
// Each frame should be done by the next VBlank (60 a second). One that is
// not is "late": the next frame then starts at once, without waiting for a
// VBlank, and Frame Skip: Auto leaves out its drawing, so that the game
// catches up (port/core/frame.c).

#define FRAME_TICKS ((u64)SYSCLOCK_ARM11 * 1000 / 59831) // the screens' refresh: 59.831 Hz
static u64 sDeadline;

bool Backend_FrameLate(void)
{
    return sLate;
}

static void WaitForFrame(void)
{
    u64 now = svcGetSystemTick();
    if (sDeadline == 0)
        sDeadline = now + FRAME_TICKS;
    sLate = now > sDeadline;
    if (sLate && now - sDeadline > 2 * FRAME_TICKS)
        sDeadline = now; // too far behind to catch up: carry on from now
    if (sLate) {
        sDeadline += FRAME_TICKS;
        return;
    }
    gspWaitForVBlank();
    sDeadline = svcGetSystemTick() + FRAME_TICKS;
}

static void ClearGameScreen(void)
{
    gfxScreen_t screen = sBottom ? GFX_BOTTOM : GFX_TOP;
    size_t size = (sBottom ? 320 : 400) * 240 * 2;
    for (int i = 0; i < 2; i++) {
        memset(gfxGetFramebuffer(screen, GFX_LEFT, NULL, NULL), 0, size);
        gfxFlushBuffers();
        gfxSwapBuffers();
    }
}

static void PrintStatus(void)
{
    consoleClear();
    printf("\x1b[1mPokemon Mystery Dungeon: Red Rescue Team\x1b[0m\n");
    printf("pmd-red-3ds (early test build)\n\n");
    printf("Y      : 1:1 / larger picture\n");
    printf("HOME   : exit (save in-game first)\n\n");
    printf("Main Screen: Others > Game Options\n\n");
    printf("Save: %s\n", SAVE_PATH);
    if (!sAudioOn)
        printf("\n\x1b[33mNo sound: sdmc:/3ds/dspfirm.cdc missing.\x1b[0m\n");
}

// Puts the game's picture on one screen; the other gets the second screen.
static void SetGameScreen(int bottom)
{
    sBottom = bottom;
    gfxScreen_t game = bottom ? GFX_BOTTOM : GFX_TOP;
    gfxSetScreenFormat(game, GSP_RGB565_OES);
    gfxSetDoubleBuffering(game, true);
    ClearGameScreen();
    sSubMode = SUB_UNSET; // the other screen is set up again on the next frame
}

// --- Input -------------------------------------------------------------------

uint16_t Backend_PollKeys(void)
{
    hidScanInput();
    u32 h = hidKeysHeld();
    uint16_t k = 0;
    if (h & KEY_A) k |= KEY_BIT_A;
    if (h & KEY_B) k |= KEY_BIT_B;
    if (h & KEY_SELECT) k |= KEY_BIT_SELECT;
    if (h & KEY_START) k |= KEY_BIT_START;
    if (h & KEY_RIGHT) k |= KEY_BIT_RIGHT; // KEY_RIGHT covers D-pad and Circle Pad
    if (h & KEY_LEFT) k |= KEY_BIT_LEFT;
    if (h & KEY_UP) k |= KEY_BIT_UP;
    if (h & KEY_DOWN) k |= KEY_BIT_DOWN;
    if (h & KEY_R) k |= KEY_BIT_R;
    if (h & KEY_L) k |= KEY_BIT_L;
    return k;
}

// --- Audio -------------------------------------------------------------------

#define AUDIO_BUFS 8
#define AUDIO_MAX_SAMPLES 1024

static int sAudioRate;
static ndspWaveBuf sWave[AUDIO_BUFS];
static int16_t *sWaveData;

static void AudioInit(void)
{
    if (R_FAILED(ndspInit()))
        return; // no dspfirm.cdc: run silently
    sWaveData = linearAlloc(AUDIO_BUFS * AUDIO_MAX_SAMPLES * 2 * sizeof(int16_t));
    if (!sWaveData) { ndspExit(); return; }
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(0);
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
    float mix[12] = { 1.0f, 1.0f };
    ndspChnSetMix(0, mix);
    memset(sWave, 0, sizeof(sWave));
    for (int i = 0; i < AUDIO_BUFS; i++) {
        sWave[i].data_pcm16 = sWaveData + i * AUDIO_MAX_SAMPLES * 2;
        sWave[i].status = NDSP_WBUF_DONE;
    }
    sAudioOn = 1;
}

void Backend_QueueAudio(const int16_t *stereo, int count, int rate)
{
    if (!sAudioOn || count <= 0)
        return;
    if (count > AUDIO_MAX_SAMPLES)
        count = AUDIO_MAX_SAMPLES;
    if (rate != sAudioRate) {
        sAudioRate = rate;
        ndspChnSetRate(0, (float)rate);
    }
    for (int i = 0; i < AUDIO_BUFS; i++) {
        ndspWaveBuf *wb = &sWave[i];
        if (wb->status != NDSP_WBUF_DONE && wb->status != NDSP_WBUF_FREE)
            continue;
        memcpy(wb->data_pcm16, stereo, (size_t)count * 4);
        wb->nsamples = (u32)count;
        DSP_FlushDataCache(wb->data_pcm16, (u32)count * 4);
        ndspChnWaveBufAdd(0, wb);
        return;
    }
    // Every buffer is still queued: the game is running ahead, drop this chunk.
}

// --- Saves -------------------------------------------------------------------

static const uint8_t *sSaveData;
static uint32_t sSaveSize;
static int sSaveDirty;

bool Backend_LoadSave(uint8_t *data, uint32_t size)
{
    FILE *f = fopen(SAVE_PATH, "rb");
    if (!f)
        return false;
    size_t n = fread(data, 1, size, f);
    fclose(f);
    Backend_Log("save: loaded %u bytes", (unsigned)n);
    return n == size;
}

void Backend_StoreSave(const uint8_t *data, uint32_t size)
{
    // The game writes its save in several pieces; write the file once they
    // are all done, at the end of the frame.
    sSaveData = data;
    sSaveSize = size;
    sSaveDirty = 1;
}

bool Backend_LoadConfig(uint8_t *data, uint32_t size)
{
    FILE *f = fopen(CONFIG_PATH, "rb");
    if (!f)
        return false;
    memset(data, 0, size);
    size_t n = fread(data, 1, size, f); // an older, shorter file: the rest stays 0
    fclose(f);
    return n > 0;
}

void Backend_StoreConfig(const uint8_t *data, uint32_t size)
{
    FILE *f = fopen(CONFIG_PATH, "wb");
    if (!f) { Backend_Log("config: cannot open for writing"); return; }
    fwrite(data, 1, size, f);
    fclose(f);
}

static void FlushSave(void)
{
    if (!sSaveDirty)
        return;
    sSaveDirty = 0;
    FILE *f = fopen(SAVE_PATH ".tmp", "wb");
    if (!f) { Backend_Log("save: cannot open for writing"); return; }
    size_t n = fwrite(sSaveData, 1, sSaveSize, f);
    int ok = (n == sSaveSize) && fflush(f) == 0;
    fclose(f);
    if (!ok) { Backend_Log("save: write failed"); remove(SAVE_PATH ".tmp"); return; }
    remove(SAVE_PATH);
    rename(SAVE_PATH ".tmp", SAVE_PATH);
}

// --- Frame, logging, errors ---------------------------------------------------

static void Shutdown(void)
{
    FlushSave();
    if (sAudioOn) { ndspExit(); linearFree(sWaveData); }
    if (sLog) fclose(sLog);
    gfxExit();
}

void Backend_EndFrame(void)
{
    hidScanInput();
    if (hidKeysDown() & KEY_Y) {
        sScaled = !sScaled;
        ClearGameScreen();
    }
    if (sFadeDir == 0 && Port_WantsBottomScreen() != sBottom)
        sFadeDir = -1;
    if (sFadeDir < 0) {
        SetFadeLevel(sFadeLevel - 1);
        if (sFadeLevel == 0) {
            SetGameScreen(Port_WantsBottomScreen());
            sFadeDir = 1;
        }
    } else if (sFadeDir > 0) {
        SetFadeLevel(sFadeLevel + 1);
        if (sFadeLevel == FADE_FRAMES)
            sFadeDir = 0;
    }
    sMarkTick = svcGetSystemTick();
    FlushSave();
    gfxFlushBuffers();
    // A skipped frame leaves the main screen's buffers as they are.
    if (sMainPresented)
        gfxScreenSwapBuffers(sBottom ? GFX_BOTTOM : GFX_TOP, false);
    gfxScreenSwapBuffers(sBottom ? GFX_TOP : GFX_BOTTOM, false);
    Part(T_FLUSH);
    WaitForFrame();
    Part(T_WAIT);
    MeterEndFrame();
    if (!aptMainLoop()) {
        Shutdown();
        exit(0);
    }
}

void Backend_Log(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    svcOutputDebugString(buf, (s32)strlen(buf));
    if (sLog) { fprintf(sLog, "%s\n", buf); fflush(sLog); }
}

void Backend_Fatal(const char *msg)
{
    Backend_Log("FATAL: %s", msg);
    if (sSubMode != SUB_TEXT)
        consoleInit(sBottom ? GFX_TOP : GFX_BOTTOM, &sConsole);
    consoleClear();
    printf("\x1b[31mpmd-red-3ds stopped:\x1b[0m\n%s\n\nPress START to exit.\n", msg);
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START)
            break;
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    Shutdown();
    exit(1);
}

int main(void)
{
    gfxInitDefault();
    osSetSpeedupEnable(true); // 804 MHz on New 3DS models

    mkdir("sdmc:/3ds", 0777);
    mkdir(DATA_DIR, 0777);
    FILE *flag = fopen(DEBUG_FLAG, "r");
    if (flag) { fclose(flag); sLog = fopen(LOG_PATH, "w"); }

    BuildLut();
    BuildMaps();
    AudioInit();
    SetGameScreen(Port_WantsBottomScreen());

    Port_Init();
    AgbMain();
    Backend_Fatal("AgbMain returned");
}
