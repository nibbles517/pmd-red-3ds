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
// The GPU (citro3d) puts both pictures on the screens, scaling them; see
// "Video on the GPU". Sound goes through the DSP (needs sdmc:/3ds/dspfirm.cdc,
// which most custom firmware setups already have).
// The save is sdmc:/3ds/pmd-red-3ds/pmd-red.sav, a standard 128 KiB GBA save
// (Save Data 1; Save Data 2 and 3 are pmd-red-2.sav and pmd-red-3.sav).

#include <3ds.h>
#include <citro3d.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "../core/port.h"

#define DATA_DIR "sdmc:/3ds/pmd-red-3ds"
// Save Data 1 (Game Options > Change Current Save Data on the title picks
// another: pmd-red-2.sav, pmd-red-3.sav)
static const char *const sSavePaths[3] = {
    DATA_DIR "/pmd-red.sav", DATA_DIR "/pmd-red-2.sav", DATA_DIR "/pmd-red-3.sav",
};
static const char *sSavePath = DATA_DIR "/pmd-red.sav";
static char sSaveTmpPath[64] = DATA_DIR "/pmd-red.sav.tmp";
#define SAVE_PATH sSavePath
#define LOG_PATH DATA_DIR "/port.log"
#define CONFIG_PATH DATA_DIR "/config.bin"
#define DEBUG_FLAG DATA_DIR "/debug.txt"
#define CPU_VIDEO_FLAG DATA_DIR "/cpu-video.txt" // the pictures copied by the CPU, as before the GPU did it

void AgbMain(void);

const int Backend_Version15_UpdateRepoIfMissing = 15;

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
// sets). So it goes in strips of 16 columns, two rows at a time: the two
// rows' 16 pixels are read together, and each column gets its two pixels
// (one above the other, next to each other in memory) in one 32-bit write.
static uint16_t sIdentity[400];
static uint16_t sMapX[400], sMapY[240];

typedef struct {
    const uint16_t *frame;
    int srcW;
    uint16_t *fb;
    int ox, oy, x0, y0, x1, y1;
    const uint16_t *xmap, *ymap, *lut;
} BlitArgs;

static void Blit(const BlitArgs *b)
{
    enum { STRIP = 16 };
    const uint16_t *lut = b->lut;
    for (int xs = b->x0; xs < b->x1; xs += STRIP) {
        int n = xs + STRIP < b->x1 ? STRIP : b->x1 - xs;
        const uint16_t *xmap = b->xmap + xs;
        uint16_t *top = b->fb + (b->ox + xs) * 240 + (239 - b->oy); // y grows downwards in memory
        int y = b->y0;
        if ((b->oy + y) & 1) { // a lone first row (the pairs must be 32-bit aligned)
            const uint16_t *row = b->frame + b->ymap[y] * b->srcW;
            for (int i = 0; i < n; i++)
                top[i * 240 - y] = PIX(row[xmap[i]]);
            y++;
        }
        for (; y + 1 < b->y1; y += 2) {
            const uint16_t *rowA = b->frame + b->ymap[y] * b->srcW;
            const uint16_t *rowB = b->frame + b->ymap[y + 1] * b->srcW;
            uint16_t *dst = top - y - 1; // row y + 1, then row y
            for (int i = 0; i < n; i++, dst += 240) {
                uint32_t a = PIX(rowA[xmap[i]]), c = PIX(rowB[xmap[i]]);
                *(uint32_t *)dst = c | (a << 16);
            }
        }
        if (y < b->y1) { // a lone last row
            const uint16_t *row = b->frame + b->ymap[y] * b->srcW;
            for (int i = 0; i < n; i++)
                top[i * 240 - y] = PIX(row[xmap[i]]);
        }
    }
}

static void BlitAsync(void *arg) { Blit(arg); }

// The main screen's copy: with Dual Core on, the second core does the right
// half while this one does the left.
static void BlitMain(const uint16_t *frame, int srcW, uint16_t *fb, int ox, int oy, int w, int h,
                     const uint16_t *xmap, const uint16_t *ymap, const uint16_t *lut)
{
    BlitArgs left = { frame, srcW, fb, ox, oy, 0, 0, w, h, xmap, ymap, lut };
    if (!Port_DualCore()) {
        Blit(&left);
        return;
    }
    static BlitArgs right;
    int mid = (w / 2) & ~15;
    right = left;
    right.x0 = mid;
    left.x1 = mid;
    Backend_RunAsync(BlitAsync, &right);
    Blit(&left);
    Backend_WaitAsync();
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
        BlitMain(frame, GBA_W, fb, ox, oy, GBA_W, GBA_H, sIdentity, sIdentity, lut);
        return;
    }
    if (sBottom) {
        // 4/3x to 320x213: every source pixel becomes 1 or 2 screen pixels.
        ScaleMaps(320, 213, 4, 3);
        BlitMain(frame, GBA_W, fb, 0, (240 - 213) / 2, 320, 213, sMapX, sMapY, lut);
        return;
    }
    // 1.5x: every source pixel becomes 1 or 2 screen pixels in each direction.
    ScaleMaps(360, 240, 3, 2);
    BlitMain(frame, GBA_W, fb, (400 - 360) / 2, 0, 360, 240, sMapX, sMapY, lut);
}

// DS size: the 256x192 picture, 1:1 or 1.25x to 320x240. Fit: the 320x240
// picture, 1:1.
static void PresentDs(const uint16_t *frame, int frameW, int frameH, uint16_t *fb, int width, const uint16_t *lut)
{
    if (!sScaled || frameW != 256) {
        const int ox = (width - frameW) / 2, oy = (240 - frameH) / 2;
        BlitMain(frame, frameW, fb, ox, oy, frameW, frameH, sIdentity, sIdentity, lut);
        return;
    }
    // 1.25x: every source pixel becomes 1 or 2 screen pixels.
    ScaleMaps(320, 240, 5, 4);
    BlitMain(frame, frameW, fb, (width - 320) / 2, 0, 320, 240, sMapX, sMapY, lut);
}

// --- Video on the GPU -------------------------------------------------------
//
// The GPU puts the pictures on the screens: each picture is converted to the
// 3DS's colours (RGB565) into a plain buffer, which the GPU's transfer engine
// copies into a texture (in the GPU's tiled layout); a rectangle with that
// texture is then drawn on each screen, scaled as the CPU path would scale it
// (nearest pixel), and dimmed by the screen change fade. The CPU no longer
// writes the framebuffers column by column, nor scales.
//
// Without citro3d (or with sdmc:/3ds/pmd-red-3ds/cpu-video.txt present), the
// CPU copies the pictures as before (Blit).

#include "present_shbin.h"

#define TEX_W 512 // the textures: wide and high enough for 320x240
#define TEX_H 256
#define TEX_TRANSFER_FLAGS                                                                  \
    (GX_TRANSFER_FLIP_VERT(1) | GX_TRANSFER_OUT_TILED(1) | GX_TRANSFER_RAW_COPY(0) |        \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB565) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))
#define SCREEN_TRANSFER_FLAGS                                                               \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |        \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

typedef struct {
    C3D_Tex tex;
    uint16_t *stage; // the converted picture, TEX_W pixels a row (linear memory)
    bool shown;      // drawn on its screen every frame
    int x, y, w, h;  // where on the screen
    int srcW, srcH;  // the picture's size in the texture
} GpuPicture;

static bool sGpu; // the GPU puts the pictures on the screens
static C3D_RenderTarget *sTargetTop, *sTargetBottom;
static C3D_Mtx sProjTop, sProjBottom;
static DVLB_s *sShaderDvlb;
static shaderProgram_s sShader;
static int sProjLoc;
static GpuPicture sGpuMain, sGpuSub;
static bool sSubModeIsPicture(void);

static bool GpuPictureInit(GpuPicture *p)
{
    p->stage = linearAlloc(TEX_W * TEX_H * 2);
    if (p->stage == NULL || !C3D_TexInitVRAM(&p->tex, TEX_W, TEX_H, GPU_RGB565))
        return false;
    memset(p->stage, 0, TEX_W * TEX_H * 2);
    C3D_TexSetFilter(&p->tex, GPU_NEAREST, GPU_NEAREST); // (as the CPU scaled: whole pixels)
    C3D_TexSetWrap(&p->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    return true;
}

static void GpuInit(void)
{
    FILE *flag = fopen(CPU_VIDEO_FLAG, "r");
    if (flag != NULL) {
        fclose(flag);
        Backend_Log("video: CPU (cpu-video.txt)");
        return;
    }
    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE)) {
        Backend_Log("video: CPU (citro3d did not start)");
        return;
    }
    sTargetTop = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, -1);
    sTargetBottom = C3D_RenderTargetCreate(240, 320, GPU_RB_RGBA8, -1);
    sShaderDvlb = DVLB_ParseFile(sPresentShader, PRESENT_SHADER_SIZE);
    if (sTargetTop == NULL || sTargetBottom == NULL || sShaderDvlb == NULL
        || !GpuPictureInit(&sGpuMain) || !GpuPictureInit(&sGpuSub)) {
        Backend_Log("video: CPU (GPU setup failed)");
        C3D_Fini();
        return;
    }
    C3D_RenderTargetSetOutput(sTargetTop, GFX_TOP, GFX_LEFT, SCREEN_TRANSFER_FLAGS);
    C3D_RenderTargetSetOutput(sTargetBottom, GFX_BOTTOM, GFX_LEFT, SCREEN_TRANSFER_FLAGS);

    shaderProgramInit(&sShader);
    shaderProgramSetVsh(&sShader, &sShaderDvlb->DVLE[0]);
    C3D_BindProgram(&sShader);
    sProjLoc = shaderInstanceGetUniformLocation(sShader.vertexShader, "projection");
    C3D_AttrInfo *attr = C3D_GetAttrInfo();
    AttrInfo_Init(attr);
    AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 3); // v0: position
    AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 2); // v1: texture coordinates
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_CullFace(GPU_CULL_NONE);
    // Screen pixels, y downwards (the 3DS's screens are turned sideways)
    Mtx_OrthoTilt(&sProjTop, 0.0f, 400.0f, 240.0f, 0.0f, 1.0f, -1.0f, true);
    Mtx_OrthoTilt(&sProjBottom, 0.0f, 320.0f, 240.0f, 0.0f, 1.0f, -1.0f, true);
    sGpu = true;
    Backend_Log("video: GPU");
}

// GBA BGR555 to RGB565, two pixels at a time (as ToRgb565)
static void GpuConvert(uint16_t *dst, const uint16_t *src, int w, int h)
{
    for (int y = 0; y < h; y++) {
        const uint32_t *s = (const uint32_t *)(src + y * w);
        uint32_t *d = (uint32_t *)(dst + y * TEX_W);
        for (int x = 0; x < w / 2; x++) {
            uint32_t c = s[x];
            d[x] = ((c & 0x001F001F) << 11) | ((c & 0x03E003E0) << 1) | ((c >> 4) & 0x00200020) | ((c >> 10) & 0x001F001F);
        }
    }
}

// A picture into its texture, to be shown at (x, y), w x h on its screen.
static void GpuUpload(GpuPicture *p, const uint16_t *frame, int frameW, int frameH, int x, int y, int w, int h)
{
    GpuConvert(p->stage, frame, frameW, frameH);
    GSPGPU_FlushDataCache(p->stage, TEX_W * frameH * 2);
    C3D_SyncDisplayTransfer((u32 *)p->stage, GX_BUFFER_DIM(TEX_W, TEX_H), (u32 *)p->tex.data,
                            GX_BUFFER_DIM(TEX_W, TEX_H), TEX_TRANSFER_FLAGS);
    p->shown = true;
    p->x = x; p->y = y; p->w = w; p->h = h;
    p->srcW = frameW; p->srcH = frameH;
}

static void GpuDrawPicture(C3D_RenderTarget *target, const C3D_Mtx *proj, GpuPicture *p)
{
    // (texture row 0 is its bottom: the transfer turned the picture upside down)
    float x0 = (float)p->x, y0 = (float)p->y, x1 = (float)(p->x + p->w), y1 = (float)(p->y + p->h);
    float u1 = (float)p->srcW / TEX_W, v0 = 1.0f, v1 = 1.0f - (float)p->srcH / TEX_H;

    C3D_RenderTargetClear(target, C3D_CLEAR_ALL, 0x000000FF, 0); // black borders
    C3D_FrameDrawOn(target);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, sProjLoc, proj);
    C3D_TexBind(0, &p->tex);
    C3D_ImmDrawBegin(GPU_TRIANGLE_STRIP);
    C3D_ImmSendAttrib(x0, y0, 0.5f, 0.0f); C3D_ImmSendAttrib(0.0f, v0, 0.0f, 0.0f);
    C3D_ImmSendAttrib(x1, y0, 0.5f, 0.0f); C3D_ImmSendAttrib(u1, v0, 0.0f, 0.0f);
    C3D_ImmSendAttrib(x0, y1, 0.5f, 0.0f); C3D_ImmSendAttrib(0.0f, v1, 0.0f, 0.0f);
    C3D_ImmSendAttrib(x1, y1, 0.5f, 0.0f); C3D_ImmSendAttrib(u1, v1, 0.0f, 0.0f);
    C3D_ImmDrawEnd();
}

// The frame's pictures on the screens (every frame: the screens are double
// buffered); citro3d copies them to the framebuffers and swaps those.
static void GpuDrawFrame(void)
{
    bool mainShown = sGpuMain.shown, subShown = sGpuSub.shown && sSubModeIsPicture();
    if (!mainShown && !subShown)
        return;
    C3D_FrameBegin(0);
    // The screen change fade: the texture's colours times a grey
    unsigned level = (unsigned)(sFadeLevel * 255 / FADE_FRAMES);
    C3D_TexEnv *env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_CONSTANT, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
    C3D_TexEnvColor(env, 0xFF000000 | (level << 16) | (level << 8) | level);
    if (mainShown)
        GpuDrawPicture(sBottom ? sTargetBottom : sTargetTop, sBottom ? &sProjBottom : &sProjTop, &sGpuMain);
    if (subShown)
        GpuDrawPicture(sBottom ? sTargetTop : sTargetBottom, sBottom ? &sProjTop : &sProjBottom, &sGpuSub);
    C3D_FrameEnd(GX_CMDLIST_FLUSH); // (only the command list: the pictures were flushed)
}

// The main screen's picture, placed as PresentGba/PresentDs place it.
static void GpuPresentMain(const uint16_t *frame, int frameW, int frameH)
{
    const int width = sBottom ? 320 : 400;
    int x, y, w = frameW, h = frameH;
    if (frameW == GBA_W && sScaled) {
        if (sBottom) { x = 0; y = (240 - 213) / 2; w = 320; h = 213; } // 4/3x
        else { x = (400 - 360) / 2; y = 0; w = 360; h = 240; }          // 1.5x
    } else if (frameW == 256 && sScaled) {
        x = (width - 320) / 2; y = 0; w = 320; h = 240; // 1.25x
    } else {
        x = (width - frameW) / 2; y = (240 - frameH) / 2;
    }
    GpuUpload(&sGpuMain, frame, frameW, frameH, x, y, w, h);
}

static void PresentMain(const uint16_t *frame, int frameW, int frameH)
{
    if (sGpu) {
        GpuPresentMain(frame, frameW, frameH);
        return;
    }

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
        PresentDs(frame, frameW, frameH, fb, width, sPresentLut);
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
static uint16_t sSubFrame[SUBSCREEN_W_MAX * SUBSCREEN_H_MAX];

static void PrintStatus(void);

static bool sSubModeIsPicture(void) { return sSubMode == SUB_PICTURE; }

static void PresentSub(void)
{
    gfxScreen_t screen = sBottom ? GFX_TOP : GFX_BOTTOM;
    const int width = sBottom ? 400 : 320;

    bool shown = Port_RenderSubScreen(sSubFrame);
    int subW, subH;
    Port_SubScreenSize(&subW, &subH);
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
    static int sSubFade = -1, sSubShownW;
    static int sPend[4]; // the area the other buffer still lacks (x0, y0, x1, y1)
    int area[4] = { 0, 0, 0, 0 };
    int full = sFadeLevel != sSubFade;
    sSubFade = sFadeLevel;
    if (sSubMode != SUB_PICTURE || subW != sSubShownW) { // (a new size: the old border cleared)
        sSubShownW = subW;
        full = 1;
        gfxSetScreenFormat(screen, GSP_RGB565_OES);
        gfxSetDoubleBuffering(screen, true);
        // both buffers, so the border stays black (the GPU clears its own:
        // and CPU writes left in the cache could later land over its picture)
        for (int i = 0; i < 2 && !sGpu; i++) {
            memset(gfxGetFramebuffer(screen, GFX_LEFT, NULL, NULL), 0, width * 240 * 2);
            gfxScreenSwapBuffers(screen, false);
        }
        sSubMode = SUB_PICTURE;
    }
    if (full) {
        area[2] = subW; area[3] = subH;
        memset(sPend, 0, sizeof(sPend));
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
    if (sGpu) { // the whole picture again, when anything in it changed
        if (area[0] < area[2] && area[1] < area[3])
            GpuUpload(&sGpuSub, sSubFrame, subW, subH, (width - subW) / 2, (240 - subH) / 2, subW, subH);
        Part(T_SUBOUT);
        return;
    }
    if (draw[0] >= draw[2] || draw[1] >= draw[3])
        return;

    const uint16_t *lut = sPresentLut;
    uint16_t *fb = (uint16_t *)gfxGetFramebuffer(screen, GFX_LEFT, NULL, NULL);
    const int ox = (width - subW) / 2, oy = (240 - subH) / 2;
    BlitArgs b = { sSubFrame, subW, fb, ox, oy, draw[0], draw[1], draw[2], draw[3], sIdentity, sIdentity, lut };
    Blit(&b);
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
    // NULL: a frame Frame Skip did not draw. During the screen change fade
    // the last picture is shown again (the fade changes its colours).
    if (frame == NULL && sFadeDir != 0)
        frame = Port_Framebuffer();
    sMainPresented = frame != NULL;
    sMarkTick = svcGetSystemTick();
    if (frame != NULL)
        PresentMain(frame, width, height);
    Part(T_OUT);
    PresentSub(); // (times T_SUB and T_SUBOUT)
}

// --- Dual Core ------------------------------------------------------------------
//
// A thread on the second CPU core runs what Backend_RunAsync is given:
// drawing part of the main screen's picture (port/core/frame.c). If it
// cannot be started, the work is done at once on this core.
//
// A New 3DS has a core of its own for apps (core 2). An Old 3DS lends apps a
// share of its system core (core 1), which the system's own services (the
// screens, sound, the HOME Menu) then do without: asking for 80% made those
// slow (port.log: sound and flushing took twice as long, even with Dual Core
// turned off again); 50% left the second core too little to draw with. So
// the share is CPU_LIMIT_ON only while Dual Core is on
// and the game is in front, and the least the system takes (CPU_LIMIT_OFF)
// otherwise; the thread is stopped when Dual Core is turned off.

#define CPU_LIMIT_ON 70
#define CPU_LIMIT_OFF 5
static Thread sWorker;
static bool sWorkerOld3DS; // the worker uses the system core's share
static aptHookCookie sAptCookie;
static bool sAptHooked, sAppInFront = true;

static void SetCpuLimit(void)
{
    if (!sWorkerOld3DS)
        return;
    APT_SetAppCpuTimeLimit(sWorker != NULL && sAppInFront ? CPU_LIMIT_ON : CPU_LIMIT_OFF);
}

static void AptEvents(APT_HookType hook, void *param)
{
    (void)param;
    if (hook == APTHOOK_ONSUSPEND || hook == APTHOOK_ONSLEEP)
        sAppInFront = false;
    else if (hook == APTHOOK_ONRESTORE || hook == APTHOOK_ONWAKEUP)
        sAppInFront = true;
    else
        return;
    SetCpuLimit();
}
static bool sWorkerTried, sWorkerQuit, sWorkerBusy;
static LightEvent sWorkGo, sWorkDone;
static void (*sWorkFn)(void *);
static void *sWorkArg;

bool Backend_IsNew3DS(void)
{
    bool isNew = false;
    APT_CheckNew3DS(&isNew);
    return isNew;
}

static void WorkerMain(void *unused)
{
    (void)unused;
    for (;;) {
        LightEvent_Wait(&sWorkGo);
        __sync_synchronize();
        if (sWorkerQuit)
            break;
        sWorkFn(sWorkArg);
        __sync_synchronize();
        LightEvent_Signal(&sWorkDone);
    }
}

static void StartWorker(void)
{
    s32 prio = 0x30;
    bool isNew = Backend_IsNew3DS();
    sWorkerTried = true;
    sWorkerQuit = false;
    LightEvent_Init(&sWorkGo, RESET_ONESHOT);
    LightEvent_Init(&sWorkDone, RESET_ONESHOT);
    if (!sAptHooked) {
        aptHook(&sAptCookie, AptEvents, NULL);
        sAptHooked = true;
    }
    sWorkerOld3DS = !isNew;
    if (!isNew)
        APT_SetAppCpuTimeLimit(CPU_LIMIT_ON);
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    sWorker = threadCreate(WorkerMain, NULL, 32 * 1024, prio - 1, isNew ? 2 : 1, false);
    SetCpuLimit();
    Backend_Log("dual core: %s (core %d)", sWorker ? "started" : "not available", isNew ? 2 : 1);
}

void Backend_RunAsync(void (*fn)(void *), void *arg)
{
    if (!sWorkerTried)
        StartWorker();
    if (sWorker == NULL) {
        fn(arg);
        return;
    }
    sWorkFn = fn;
    sWorkArg = arg;
    sWorkerBusy = true;
    __sync_synchronize();
    LightEvent_Signal(&sWorkGo);
}

void Backend_WaitAsync(void)
{
    if (!sWorkerBusy)
        return;
    LightEvent_Wait(&sWorkDone);
    __sync_synchronize();
    sWorkerBusy = false;
}

bool Backend_AsyncDone(void)
{
    if (!sWorkerBusy)
        return true;
    if (!LightEvent_TryWait(&sWorkDone))
        return false;
    __sync_synchronize();
    sWorkerBusy = false;
    return true;
}

static void StopWorker(void)
{
    if (sWorker == NULL)
        return;
    Backend_WaitAsync();
    sWorkerQuit = true;
    __sync_synchronize();
    LightEvent_Signal(&sWorkGo);
    threadJoin(sWorker, U64_MAX);
    threadFree(sWorker);
    sWorker = NULL;
    sWorkerTried = false; // started again when Dual Core is turned on
    SetCpuLimit();
    Backend_Log("dual core: stopped");
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
    if (sGpu) // (the GPU clears the screens it draws on, every frame)
        return;
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
    if (h & KEY_DUP) k |= KEY_BIT_UP; // (the Circle Pad's up/down: apart, below)
    if (h & KEY_DDOWN) k |= KEY_BIT_DOWN;
    if (h & KEY_CPAD_UP) k |= KEY_BIT_CPAD_UP;
    if (h & KEY_CPAD_DOWN) k |= KEY_BIT_CPAD_DOWN;
    if (h & KEY_ZL) k |= KEY_BIT_ZL;
    if (h & KEY_ZR) k |= KEY_BIT_ZR;
    if (h & KEY_R) k |= KEY_BIT_R;
    if (h & KEY_L) k |= KEY_BIT_L;
    if (h & KEY_Y) k |= KEY_BIT_Y;
    if (h & KEY_X) k |= KEY_BIT_X;
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

// --- Data files (Music Options: pmd-blue.nds) ---------------------------------
//
// Read in 64 KiB pieces: SD card reads are much faster in big chunks.

#define DATA_FILE_BUF (64 * 1024)
typedef struct {
    FILE *f;
    uint32_t size;
    uint8_t *buf;
    uint32_t bufOff, bufLen;
} DataFile;

void *Backend_FileOpen(const char *name)
{
    char path[256];
    snprintf(path, sizeof(path), DATA_DIR "/%s", name);
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    DataFile *d = calloc(1, sizeof(*d));
    if (d == NULL || (d->buf = malloc(DATA_FILE_BUF)) == NULL || fseek(f, 0, SEEK_END) != 0) {
        if (d) free(d->buf);
        free(d);
        fclose(f);
        return NULL;
    }
    long end = ftell(f);
    d->f = f;
    d->size = end > 0 ? (uint32_t)end : 0;
    Backend_Log("data: %s, %u bytes", path, (unsigned)d->size);
    return d;
}

uint32_t Backend_FileSize(void *file)
{
    return file ? ((DataFile *)file)->size : 0;
}

bool Backend_FileRead(void *file, uint32_t offset, void *dest, uint32_t size)
{
    DataFile *d = file;
    uint8_t *out = dest;
    if (d == NULL || offset > d->size || size > d->size - offset)
        return false;
    while (size > 0) {
        if (offset >= d->bufOff && offset < d->bufOff + d->bufLen) {
            uint32_t n = d->bufOff + d->bufLen - offset;
            if (n > size) n = size;
            memcpy(out, d->buf + (offset - d->bufOff), n);
            out += n; offset += n; size -= n;
            continue;
        }
        if (size >= DATA_FILE_BUF) { // a big read: straight into place
            if (fseek(d->f, (long)offset, SEEK_SET) != 0 || fread(out, 1, size, d->f) != size)
                return false;
            return true;
        }
        d->bufOff = offset;
        d->bufLen = 0;
        uint32_t n = d->size - offset < DATA_FILE_BUF ? d->size - offset : DATA_FILE_BUF;
        if (fseek(d->f, (long)offset, SEEK_SET) != 0 || fread(d->buf, 1, n, d->f) != n)
            return false;
        d->bufLen = n;
    }
    return true;
}

void Backend_FileClose(void *file)
{
    DataFile *d = file;
    if (d == NULL)
        return;
    fclose(d->f);
    free(d->buf);
    free(d);
}

bool Backend_FileWrite(const char *name, const void *data, uint32_t size)
{
    char path[256], tmp[260];
    snprintf(path, sizeof(path), DATA_DIR "/%s", name);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (f == NULL)
        return false;
    bool ok = fwrite(data, 1, size, f) == size;
    ok = fclose(f) == 0 && ok;
    if (!ok) {
        remove(tmp);
        return false;
    }
    remove(path);
    rename(tmp, path);
    Backend_Log("data: wrote %s", path);
    return true;
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

static void FlushSave(void);

void Backend_SetSaveSlot(int slot)
{
    if (slot < 0 || slot > 2)
        slot = 0;
    if (sSavePaths[slot] == sSavePath)
        return;
    FlushSave(); // what is pending goes to the file in use until now
    sSavePath = sSavePaths[slot];
    snprintf(sSaveTmpPath, sizeof(sSaveTmpPath), "%s.tmp", sSavePath);
    Backend_Log("save: now %s", sSavePath);
}

static void FlushSave(void)
{
    if (!sSaveDirty)
        return;
    sSaveDirty = 0;
    FILE *f = fopen(sSaveTmpPath, "wb");
    if (!f) { Backend_Log("save: cannot open for writing"); return; }
    size_t n = fwrite(sSaveData, 1, sSaveSize, f);
    int ok = (n == sSaveSize) && fflush(f) == 0;
    fclose(f);
    if (!ok) { Backend_Log("save: write failed"); remove(sSaveTmpPath); return; }
    remove(SAVE_PATH);
    rename(sSaveTmpPath, SAVE_PATH);
}

// --- Frame, logging, errors ---------------------------------------------------

static void Shutdown(void)
{
    StopWorker();
    FlushSave();
    if (sAudioOn) { ndspExit(); linearFree(sWaveData); }
    if (sLog) fclose(sLog);
    if (sGpu) C3D_Fini();
    gfxExit();
}

void Backend_EndFrame(void)
{
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
    if (sWorker != NULL && !Port_DualCore())
        StopWorker(); // Dual Core turned off: give the system core back
    sMarkTick = svcGetSystemTick();
    FlushSave();
    if (sGpu) {
        // The GPU draws both pictures and swaps those screens; the CPU only
        // writes the status text (the second screen, when it has no picture).
        if (sSubMode == SUB_TEXT)
            gfxFlushBuffers();
        GpuDrawFrame();
    } else {
        gfxFlushBuffers();
        // A skipped frame leaves the main screen's buffers as they are.
        if (sMainPresented)
            gfxScreenSwapBuffers(sBottom ? GFX_BOTTOM : GFX_TOP, false);
        gfxScreenSwapBuffers(sBottom ? GFX_TOP : GFX_BOTTOM, false);
    }
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
    GpuInit();
    AudioInit();
    SetGameScreen(Port_WantsBottomScreen());

    Port_Init();
    AgbMain();
    Backend_Fatal("AgbMain returned");
}
