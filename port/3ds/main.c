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
#include "../core/gpulist.h"
#include "../core/bgcache.h"

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
#define GPU_NOFLIP_FLAG DATA_DIR "/gpu-noflip.txt" // GPU Drawing: the textures' rows top first (see GpuListInit)
#define GPU_GSPFLUSH_FLAG DATA_DIR "/gpu-gspflush.txt" // GPU Drawing: flush its textures through GSP (see GlFlushRange)

void AgbMain(void);

const int Backend_Version18_UpdateRepoIfMissing = 18;


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

// port.log's lines are written by a thread of their own (one priority below
// the game's, on its core: it runs while the game waits for the screen), so
// the SD card's writes don't hold up a frame; LogLine hands it a line.
#define LOG_RING (32 * 1024)
static char sLogRing[LOG_RING];
static size_t sLogHead, sLogTail; // (written up to / taken up to; wrap at LOG_RING)
static LightLock sLogLock;
static LightEvent sLogEvent;
static Thread sLogThread;
static volatile bool sLogQuit;

// What the ring holds, to the file (the thread; or, once it has stopped, a
// fatal error or the end)
static void LogDrain(void)
{
    char chunk[1024];
    for (;;) {
        LightLock_Lock(&sLogLock);
        size_t n = 0;
        while (sLogTail != sLogHead && n < sizeof(chunk)) {
            chunk[n++] = sLogRing[sLogTail];
            sLogTail = (sLogTail + 1) % LOG_RING;
        }
        LightLock_Unlock(&sLogLock);
        if (n == 0)
            break;
        fwrite(chunk, 1, n, sLog);
    }
    fflush(sLog);
}

static void LogMain(void *arg)
{
    (void)arg;
    while (!sLogQuit) {
        LightEvent_Wait(&sLogEvent);
        LogDrain();
    }
}

static void LogLine(const char *text)
{
    if (sLog == NULL)
        return;
    if (sLogThread == NULL) { // (no thread: as it was)
        fprintf(sLog, "%s\n", text);
        fflush(sLog);
        return;
    }
    LightLock_Lock(&sLogLock);
    for (const char *c = text;; c++) {
        size_t next = (sLogHead + 1) % LOG_RING;
        if (next == sLogTail)
            break; // (full: the rest is lost)
        sLogRing[sLogHead] = *c ? *c : '\n';
        sLogHead = next;
        if (!*c)
            break;
    }
    LightLock_Unlock(&sLogLock);
    LightEvent_Signal(&sLogEvent);
}

static void LogStart(void)
{
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    LightLock_Init(&sLogLock);
    LightEvent_Init(&sLogEvent, RESET_ONESHOT);
    sLogThread = threadCreate(LogMain, NULL, 16 * 1024, prio + 1, -2, false);
}

// Before a fatal error's message or the end: the thread's lines, then
// straight to the file
static void LogStop(void)
{
    if (sLogThread != NULL) {
        sLogQuit = true;
        LightEvent_Signal(&sLogEvent);
        threadJoin(sLogThread, U64_MAX);
        threadFree(sLogThread);
        sLogThread = NULL;
        LogDrain();
    }
}
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

// The main screen's copy (on this core: the second one is the sound's)
static void BlitMain(const uint16_t *frame, int srcW, uint16_t *fb, int ox, int oy, int w, int h,
                     const uint16_t *xmap, const uint16_t *ymap, const uint16_t *lut)
{
    BlitArgs args = { frame, srcW, fb, ox, oy, 0, 0, w, h, xmap, ymap, lut };
    Blit(&args);
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
    bool shown;      // on its screen
    bool dirty;      // changed since it was last drawn (new picture, place or setting)
    bool transfer;   // stage converted, not yet copied into the texture (GpuDrawFrame queues the copy)
    unsigned level;  // the fade level it was last drawn with
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
static void GpuListInit(void);
static void GlBench(void);

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
    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 4)) { // (room for GPU Drawing's rectangles)
        Backend_Log("video: CPU (citro3d did not start)");
        return;
    }
    // (with a stencil buffer: GPU Drawing's blending, GpuDrawList)
    sTargetTop = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    sTargetBottom = C3D_RenderTargetCreate(240, 320, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
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
    GpuListInit();
    Port_SetSubScreenFadeByBackend(true); // (its fades: the texture's colours times a grey, as the main screen's)
    Backend_Log("video: GPU");
}

// GBA BGR555 to RGB565, two pixels at a time (as ToRgb565)
static void GpuConvert(uint16_t *dst, const uint16_t *src, int w, int y0, int y1)
{
    for (int y = y0; y < y1; y++) {
        const uint32_t *s = (const uint32_t *)(src + y * w);
        uint32_t *d = (uint32_t *)(dst + y * TEX_W);
        for (int x = 0; x < w / 2; x++) {
            uint32_t c = s[x];
            d[x] = ((c & 0x001F001F) << 11) | ((c & 0x03E003E0) << 1) | ((c >> 4) & 0x00200020) | ((c >> 10) & 0x001F001F);
        }
    }
}

// The CPU's writes to linear memory, out of its cache for the GPU. Through
// the kernel, on this core: GSPGPU_FlushDataCache asks the GSP service, on
// the system core, where it waits while the DS music's worker (Dual Core)
// has the core, up to the 2 ms its frame's music takes. gpu-gspflush.txt:
// through GSP again.
static bool sGspFlush;
static u64 sGlFrameFlush; // (the slow frame lines: this frame's flushing of the list's textures)
static void GlFlushRange(const void *p, u32 size)
{
    if (sGspFlush || R_FAILED(svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)p, size)))
        GSPGPU_FlushDataCache(p, size);
}

// A picture into its texture, to be shown at (x, y), w x h on its screen.
// Only its rows y0 .. y1 - 1 changed since the last time (the rest of the
// converted picture is kept); all of it: 0, frameH.
static void GpuUpload(GpuPicture *p, const uint16_t *frame, int frameW, int frameH, int x, int y, int w, int h,
                      int y0, int y1)
{
    if (frameW != p->srcW || frameH != p->srcH)
        y0 = 0, y1 = frameH; // (a new size: none of it kept)
    GpuConvert(p->stage, frame, frameW, y0, y1);
    GlFlushRange(p->stage + y0 * TEX_W, TEX_W * (y1 - y0) * 2);
    p->transfer = true; // (the copy into the texture: queued with the frame's GPU work, not waited for)
    p->shown = true;
    p->dirty = true;
    p->x = x; p->y = y; p->w = w; p->h = h;
    p->srcW = frameW; p->srcH = frameH;
}

static void GpuDrawPicture(C3D_RenderTarget *target, const C3D_Mtx *proj, GpuPicture *p)
{
    // The picture's first row is at v = 0, its last at v = srcH / TEX_H (seen
    // on a 3DS: drawing v from 1 at the top showed it upside down, its first
    // 16 rows missing and 16 black ones at the top, the rest of the texture).
    float x0 = (float)p->x, y0 = (float)p->y, x1 = (float)(p->x + p->w), y1 = (float)(p->y + p->h);
    float u1 = (float)p->srcW / TEX_W, v0 = 0.0f, v1 = (float)p->srcH / TEX_H;

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

// --- GPU Drawing: the drawing list on the GPU (docs/GPU.md) ------------------------
//
// A frame the core could describe (port/core/gpulist.h) is drawn by the GPU
// straight onto the screen, at the picture's place and scale: the backdrop,
// then the list's rectangles back to front. Its textures are in linear
// memory, written by the CPU in the GPU's own layout (8x8 tiles, the pixels
// of each in Z order), as RGBA5551 (alpha 0: transparent):
//   - one per background cache (512x512): only its tiles that changed (the
//     cache marks them, gpuDirty) are written;
//   - the strip and sprite textures: only where the frame's rectangles read.
// They are written just before the GPU draws the frame (GpuDrawFrame, after
// C3D_FrameBegin: the GPU is done with the last one).
//
// Transparent texels are left out by the alpha test. Alpha blending, as the
// list says: the stencil buffer holds, per pixel, whether what is there is a
// second target (cleared to the backdrop's); a rectangle that isn't a first
// target covers what is there and sets it to whether it is a second target;
// one that is draws where it is 0 as it is, then where it is 1 blended with
// what is there (the blend colour: eva, evb), setting it to 0 (a first target
// is never a second one in a listed frame).
//
// Rows: texture coordinate v = y / 512 for the image's row y (y down, as the
// picture textures are drawn), which the GPU reads from memory row 511 - y:
// the rows are written bottom row first (the first build wrote the top row
// first, and every piece came out upside down). With gpu-noflip.txt in the
// data folder they are written top row first again.

#define GL_TEX 512 // every list texture is 512x512
static C3D_Tex sGlBg[4], sGlStrip, sGlObj;
static bool sGlReady, sGlFlip;
static const GpuList *sGlList;    // the main screen's picture is this list (else sGpuMain's texture)
static bool sGlUploaded;          // ... its textures written
static int sGlFrameW, sGlFrameH, sGlOx, sGlOy; // where the list goes in the frame
static uint16_t sGlMorton[8][8];  // (y, x) in a tile -> its place in the tile
static uint8_t sGlPair[8][4];     // (y, x / 2) -> the place of texels x, x + 1 (together: Z order) in 32-bit words
static u64 sGlUploadTicks;        // the meter: writing the textures
static uint32_t sGlLastListed, sGlLastWhy[GPU_WHY_COUNT];

static void GpuListInit(void)
{
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            sGlMorton[y][x] = (uint16_t)((x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3));
    for (int y = 0; y < 8; y++)
        for (int k = 0; k < 4; k++)
            sGlPair[y][k] = (uint8_t)(sGlMorton[y][2 * k] >> 1);
    sGlFlip = true;
    FILE *flag = fopen(GPU_NOFLIP_FLAG, "r");
    if (flag != NULL) {
        fclose(flag);
        sGlFlip = false;
    }
    flag = fopen(GPU_GSPFLUSH_FLAG, "r");
    if (flag != NULL) {
        fclose(flag);
        sGspFlush = true;
    }
    C3D_Tex *all[6] = { &sGlBg[0], &sGlBg[1], &sGlBg[2], &sGlBg[3], &sGlStrip, &sGlObj };
    for (int i = 0; i < 6; i++) {
        if (!C3D_TexInit(all[i], GL_TEX, GL_TEX, GPU_RGBA5551)) { // (linear memory)
            Backend_Log("gpu drawing: not available (no memory for its textures)");
            return;
        }
        memset(all[i]->data, 0, GL_TEX * GL_TEX * 2);
        GSPGPU_FlushDataCache(all[i]->data, GL_TEX * GL_TEX * 2);
        C3D_TexSetFilter(all[i], GPU_NEAREST, GPU_NEAREST);
        C3D_TexSetWrap(all[i], GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    }
    sGlReady = true;
    GlBench();
    Backend_Log("gpu drawing: ready%s%s", sGlFlip ? "" : " (rows top first: gpu-noflip.txt)",
                sGspFlush ? " (flushes through GSP: gpu-gspflush.txt)" : "");
}

int Backend_GpuListMode(void)
{
    return sGpu && sGlReady && Port_GpuDraw() ? 3 : 0;
}

// GBA BGR555 (bit 15: transparent) -> RGBA5551
static inline uint16_t To5551(uint16_t c)
{
    if (c & 0x8000)
        return 0;
    return (uint16_t)(((c & 0x1F) << 11) | ((c & 0x3E0) << 1) | ((c >> 9) & 0x3E) | 1);
}

// Two pixels at once (the low half: the left one), as To5551
static inline uint32_t To5551x2(uint32_t p)
{
    uint32_t clear = ((p >> 15) & 0x00010001u) * 0xFFFFu; // (bit 15: transparent)
    uint32_t c = ((p & 0x001F001Fu) << 11) | ((p & 0x03E003E0u) << 1) | ((p >> 9) & 0x003E003Eu) | 0x00010001u;
    return c & ~clear;
}

// Texel (x, y) of a list texture (y: the image's row, from its top)
static inline uint16_t *GlTexel(uint16_t *tex, int x, int y)
{
    if (sGlFlip)
        y = GL_TEX - 1 - y;
    return tex + ((((y >> 3) * (GL_TEX / 8) + (x >> 3)) << 6) | sGlMorton[y & 7][x & 7]);
}

// The rows of a texture written since the last flush (in memory order)
static int sGlLo, sGlHi;
static void GlTouch(int y0, int y1) // (image rows y0 .. y1 - 1)
{
    int a = sGlFlip ? GL_TEX - y1 : y0, b = sGlFlip ? GL_TEX - y0 : y1;
    a &= ~7;
    b = (b + 7) & ~7;
    if (a < sGlLo) sGlLo = a;
    if (b > sGlHi) sGlHi = b;
}
static void GlFlush(C3D_Tex *t)
{
    if (sGlHi > sGlLo) {
        u64 t0 = svcGetSystemTick();
        GlFlushRange((uint16_t *)t->data + sGlLo * GL_TEX, (sGlHi - sGlLo) * GL_TEX * 2);
        sGlFrameFlush += svcGetSystemTick() - t0;
    }
    sGlLo = GL_TEX;
    sGlHi = 0;
}


// The list's textures. Colours come from the list's base palette (as
// RGBA5551: sGlLut), so during a fade (only its amount changes) nothing is
// written again. Backgrounds: their tiles that the frame's rectangles show
// and that changed: decoded again (gpuDirty, the cache's) or using a palette
// bank whose base colours changed (added to gpuDirty here); the others stay
// marked until they're shown. Then the strips (pixels) and the sprites
// (colour numbers) where the rectangles read.
static uint16_t sGlLut[512], sGlLastBase[512];
static bool sGlLastBaseOk;

// The colour of each colour number as a background tile's texel: sGlLut
// with colour 0 clear (and, 16 colours a bank, the wide map's black: 0x10)
static uint16_t sGlLut16[256], sGlLut256[256];

// A tile's 64 texels are together in the texture, in Z order. They're made
// in a buffer first and copied whole: the texture's memory isn't in the
// cache, and 64 scattered 2-byte writes to it cost far more than one copy.
static inline uint16_t *GlTile(uint16_t *tex, int tx, int ty)
{
    return tex + (((sGlFlip ? GL_TEX / 8 - 1 - ty : ty) * (GL_TEX / 8) + tx) << 6);
}

// The colours each background tile in the GPU's textures uses (made when
// it is written): 16 colours a bank, bit n: colour n of its bank (not 0);
// 256 colours, bit n: one of colours 16n..16n+15 (not 0). A palette change
// writes again only the tiles using a changed colour, without reading them.
static uint16_t sGlTileUse[4][64][64];

// Each colour number's texel (low half) and its bit of sGlTileUse (high
// half), 16 colours a bank and 256: a texel is one look-up (the bench on an
// Old 3DS: making a tile was most of writing one, and not the memory's
// waiting: the arithmetic per texel)
static uint32_t sGlTab16[256], sGlTab256[256];

// A tile of colour numbers (8 rows, BGC_STRIDE apart) into buf32 in the
// texture's layout; returns the colours it uses (sGlTileUse)
static inline unsigned GlMakeIdx(const uint8_t *idx, const uint32_t *tab, uint32_t *buf32)
{
    uint32_t use = 0;
    for (int r = 0; r < 8; r++, idx += BGC_STRIDE) {
        int y = sGlFlip ? 7 - r : r;
        uint32_t *o = buf32 + ((y & 1) | ((y & 2) << 1) | ((y & 4) << 2)); // (texels x, x + 1: o[0], o[2], o[8], o[10])
        uint32_t w0, w1;
        memcpy(&w0, idx, 4);
        memcpy(&w1, idx + 4, 4);
        uint32_t e0 = tab[w0 & 0xFF], e1 = tab[(w0 >> 8) & 0xFF], e2 = tab[(w0 >> 16) & 0xFF], e3 = tab[w0 >> 24];
        uint32_t e4 = tab[w1 & 0xFF], e5 = tab[(w1 >> 8) & 0xFF], e6 = tab[(w1 >> 16) & 0xFF], e7 = tab[w1 >> 24];
        o[0] = (e0 & 0xFFFF) | (e1 << 16);
        o[2] = (e2 & 0xFFFF) | (e3 << 16);
        o[8] = (e4 & 0xFFFF) | (e5 << 16);
        o[10] = (e6 & 0xFFFF) | (e7 << 16);
        use |= e0 | e1 | e2 | e3 | e4 | e5 | e6 | e7;
    }
    return use >> 16;
}

static void GlBgTile(BgCache *c, int layer, uint16_t *tex, int tx, int ty)
{
    uint32_t buf32[32];
    int x0 = tx * 8, y0 = ty * 8;
    if (c->indexOk) // (colour numbers: 0 is clear, as their pixels say)
        sGlTileUse[layer][ty][tx] = (uint16_t)GlMakeIdx(&c->idx[y0 * BGC_STRIDE + x0],
                                                         c->color256 ? sGlTab256 : sGlTab16, buf32);
    else {
        const uint16_t *px = &c->px[y0 * BGC_STRIDE + x0];
        for (int r = 0; r < 8; r++, px += BGC_STRIDE) {
            const uint8_t *m = sGlPair[sGlFlip ? 7 - r : r];
            for (int k = 0; k < 4; k++)
                buf32[m[k]] = To5551x2(px[2 * k] | ((uint32_t)px[2 * k + 1] << 16));
        }
    }
    memcpy(GlTile(tex, tx, ty), buf32, sizeof(buf32));
    GlTouch(y0, y0 + 8);
}

// Once at start: what writing a background tile takes (port.log "gpu
// bench": per tile, 2048 tiles in screen order, as an upload writes them):
// the first way (per texel: two look-ups and the colour's bit worked out),
// the way it's done now (one look-up), only copying made tiles into the
// texture, and making them the way it's done now without the copying
static void GlBench(void)
{
    static uint32_t made[64][32]; // (64 tiles, in the cache)
    BgCache *c = &gBgCache[0];
    uint16_t *tex = (uint16_t *)sGlBg[1].data;
    double us = 1e6 / SYSCLOCK_ARM11;
    u64 t[5];
    for (int pass = 0; pass < 4; pass++) {
        t[pass] = svcGetSystemTick();
        for (int i = 0; i < 2048; i++) {
            int tx = i & 63, ty = (i >> 6) & 63;
            uint32_t *buf32 = made[i & 63];
            if (pass == 1 || pass == 3) {
                buf32[0] |= GlMakeIdx(&c->idx[ty * 8 * BGC_STRIDE + tx * 8], sGlTab16, buf32) & 1;
            } else if (pass == 0) {
                const uint8_t *idx = &c->idx[ty * 8 * BGC_STRIDE + tx * 8];
                unsigned use = 0;
                for (int r = 0; r < 8; r++, idx += BGC_STRIDE) {
                    const uint8_t *m = sGlPair[sGlFlip ? 7 - r : r];
                    for (int k = 0; k < 4; k++) {
                        unsigned a = idx[2 * k], b = idx[2 * k + 1];
                        buf32[m[k]] = sGlLut16[a] | ((uint32_t)sGlLut16[b] << 16);
                        use |= ((1u << (a & 15)) & -(a != 0)) | ((1u << (b & 15)) & -(b != 0));
                    }
                }
                buf32[0] |= use & 1; // (kept)
            }
            if (pass != 3)
                memcpy(GlTile(tex, tx, ty), buf32, 128);
        }
    }
    t[4] = svcGetSystemTick();
    memset(tex, 0, GL_TEX * GL_TEX * 2);
    GlFlushRange(tex, GL_TEX * GL_TEX * 2);
    Backend_Log("gpu bench: tile the first way %.2f us, now %.2f; copied only %.2f, made only (now) %.2f",
                (t[1] - t[0]) * us / 2048, (t[2] - t[1]) * us / 2048, (t[3] - t[2]) * us / 2048,
                (t[4] - t[3]) * us / 2048);
}

static bool sGlNoBudget;
static int sGlFramePalTiles; // (... of which a palette animation's)
static int sGlFrameTiles; // (the slow frame lines: this frame's texture writing)
static u64 sGlFrameBg, sGlFrameStrip, sGlFrameObj;

static void GpuListUpload(const GpuList *l)
{
    u64 start = svcGetSystemTick();
    sGlFrameTiles = 0;
    sGlFramePalTiles = 0;
    sGlFrameFlush = 0;

    // The base palette's colours; the BG and OBJ banks whose colours changed
    // (and which BG colours: a palette animation changes a few of a bank's)
    static uint8_t changedEntry[256];
    unsigned changedBanks = 0, changedObj = 0;
    for (int i = 0; i < 512; i++) {
        uint16_t b = l->base[i];
        int changed = !sGlLastBaseOk || b != sGlLastBase[i];
        if (i < 256)
            changedEntry[i] = (uint8_t)changed;
        if (changed) {
            if (i < 256)
                changedBanks |= 1u << (i >> 4);
            else
                changedObj |= 1u << ((i - 256) >> 4);
            sGlLastBase[i] = b;
        }
        sGlLut[i] = (uint16_t)(To5551(b) | 1);
    }
    sGlLastBaseOk = true;
    // Which colours changed, as sGlTileUse has them: 16 colours a bank (each
    // bank's colours 1-15), 256 colours (the groups of 16)
    uint16_t changed16[16] = { 0 }, changed256 = 0;
    for (int i = 0; i < 256; i++)
        if (changedEntry[i]) {
            if (i & 15)
                changed16[i >> 4] |= (uint16_t)(1u << (i & 15));
            if (i)
                changed256 |= (uint16_t)(1u << (i >> 4));
        }
    memcpy(sGlLut16, sGlLut, sizeof(sGlLut16));
    memcpy(sGlLut256, sGlLut, sizeof(sGlLut256));
    sGlLut16[0] = sGlLut256[0] = 0;
    sGlLut16[0x10] = 1; // (the wide map's black)
    for (int n = 0; n < 256; n++) {
        sGlTab16[n] = sGlLut16[n] | ((n & 15) ? 1u << (16 + (n & 15)) : 0); // (colour 0 of a bank: clear, or the black)
        sGlTab256[n] = sGlLut256[n] | (n ? 1u << (16 + (n >> 4)) : 0);
    }
    // The tiles using a changed colour: written again when shown (a tile
    // already marked will be written anyway, and its sGlTileUse is old).
    // A palette animation (the base colours changing, steady or under a
    // fade) can change hundreds of tiles every few frames (the world map:
    // ~800 every 6; the ground maps: ~575 every 10, also while they fade in
    // and out, where it was the fades' stutter), a few microseconds each on
    // an Old 3DS: those are written a part a frame (sGlPalPending), a tile
    // a frame or two late at most. (A new scene's tiles are decoded again:
    // gpuDirty, not spread.)
    static uint32_t sGlPalPending[4][64][2];
    static bool sGlPalAny[4];
    // (Not when the base colours change meaning: from or to the palettes as
    // they are (GPU_PAL_OTHER: a fade the GPU can't do, the dungeon's
    // brightness at times), or after a black or held frame: every tile
    // changes then, and written a part a frame the dungeon's first floor
    // came in in chunks, some still in the old colours.)
    static int sGlLastClass = -1;
    bool spread = l->palette != GPU_PAL_OTHER && sGlLastClass >= 0 && sGlLastClass != GPU_PAL_OTHER;
    sGlLastClass = (l->quadCount == 0 || sGlNoBudget) ? -1 : l->palette;
    for (int b = 0; !spread && b < 4; b++) // (tiles still waiting from a palette animation: now, whole)
        if (sGlPalAny[b]) {
            for (int ty = 0; ty < 64; ty++)
                for (int w = 0; w < 2; w++) {
                    gBgCache[b].gpuDirty[ty][w] |= sGlPalPending[b][ty][w];
                    sGlPalPending[b][ty][w] = 0;
                }
            sGlPalAny[b] = false;
            gBgCache[b].gpuAny = 1;
        }
    for (int b = 0; changedBanks && b < 4; b++) {
        BgCache *c = &gBgCache[b];
        if (!c->valid || !c->indexOk)
            continue; // (no colour numbers: their pixels, marked by the cache)
        int any = 0;
        for (int ty = 0; ty < c->tilesH; ty++) {
            const uint16_t *use = sGlTileUse[b][ty];
            uint32_t *dirty = spread ? sGlPalPending[b][ty] : c->gpuDirty[ty];
            for (int tx = 0; tx < c->tilesW; tx++) {
                unsigned ch = c->color256 ? changed256 : changed16[c->entry[ty * 64 + tx] >> 12];
                if (use[tx] & ch) {
                    dirty[tx >> 5] |= 1u << (tx & 31);
                    any = 1;
                }
            }
        }
        if (any) {
            if (spread)
                sGlPalAny[b] = true;
            else
                c->gpuAny = 1;
        }
    }

    // While a fade has the screen black (or nearly: under 10%), little shows:
    // a scene's new tiles (which come with its new palette, on the first
    // frame of its fade in) are written a part a frame instead of all at
    // once; one not written yet shows its old tile at that brightness
    int dark = l->fadeOn && !(l->fade.add[0] | l->fade.add[1] | l->fade.add[2]);
    // (spread only over frames that show nothing: black. Spread over dim
    // ones, a dungeon's first floor faded in half a screen at a time)
    int budget = dark && l->fade.mul == 0 ? 400 : 1 << 30;
    int palBudget = 250;
    if (sGlNoBudget) // (Backend_UploadList: the screen is black, nothing to keep on time)
        budget = palBudget = 1 << 30;
    sGlLo = GL_TEX;
    sGlHi = 0;
    for (int b = 0; b < 4; b++) {
        BgCache *c = &gBgCache[b];
        if (!c->gpuAny && !sGlPalAny[b])
            continue;
        bool palLeft = false;
        uint16_t *tex = (uint16_t *)sGlBg[b].data;
        for (int i = 0; i < l->quadCount; i++) {
            const GpuQuad *q = &l->quads[i];
            if (q->tex != b)
                continue;
            for (int ty = q->sy >> 3; ty <= (q->sy + q->h - 1) >> 3; ty++)
                for (int tx = q->sx >> 3; tx <= (q->sx + q->w - 1) >> 3; tx++) {
                    uint32_t bit = 1u << (tx & 31);
                    uint32_t *dirty = &c->gpuDirty[ty][tx >> 5], *pal = &sGlPalPending[b][ty][tx >> 5];
                    if (*dirty & bit) {
                        if (budget <= 0)
                            continue;
                        budget--;
                    } else if (*pal & bit) {
                        if (palBudget <= 0) {
                            palLeft = true;
                            continue;
                        }
                        palBudget--;
                        sGlFramePalTiles++;
                    } else {
                        continue;
                    }
                    *dirty &= ~bit;
                    *pal &= ~bit;
                    GlBgTile(c, b, tex, tx, ty);
                    sGlFrameTiles++;
                }
        }
        if (!palLeft) { // (those not shown: written when they are, whole: as if decoded again)
            for (int ty = 0; sGlPalAny[b] && ty < 64; ty++)
                for (int w = 0; w < 2; w++) {
                    c->gpuDirty[ty][w] |= sGlPalPending[b][ty][w];
                    sGlPalPending[b][ty][w] = 0;
                }
            sGlPalAny[b] = false;
        }
        GlFlush(&sGlBg[b]);
    }
    u64 t1 = svcGetSystemTick();
    sGlFrameBg = t1 - start;
    // The strips (pixels): the tiles their rectangles read, each once and
    // whole (gGpuStrip has this frame's rows; texels of a tile no rectangle
    // reads are never shown)
    static uint32_t stripTiles[GL_TEX / 8][GL_TEX / 8 / 32];
    int stripTy0 = GL_TEX / 8, stripTy1 = 0;
    for (int i = 0; i < l->quadCount; i++) {
        const GpuQuad *q = &l->quads[i];
        if (q->tex != GTEX_STRIP || q->w <= 0 || q->h <= 0)
            continue;
        int ty0 = q->sy >> 3, ty1 = (q->sy + q->h - 1) >> 3;
        for (int ty = ty0; ty <= ty1; ty++)
            for (int tx = q->sx >> 3; tx <= (q->sx + q->w - 1) >> 3; tx++)
                stripTiles[ty][tx >> 5] |= 1u << (tx & 31);
        if (ty0 < stripTy0) stripTy0 = ty0;
        if (ty1 + 1 > stripTy1) stripTy1 = ty1 + 1;
    }
    uint16_t *strip = (uint16_t *)sGlStrip.data;
    for (int ty = stripTy0; ty < stripTy1; ty++) {
        for (int w = 0; w < GL_TEX / 8 / 32; w++)
            for (uint32_t bits = stripTiles[ty][w]; bits; bits &= bits - 1) {
                int tx = w * 32 + __builtin_ctz(bits);
                uint32_t buf32[32];
                            for (int r = 0; r < 8; r++) {
                    const uint16_t *src = &gGpuStrip[ty * 8 + r][tx * 8];
                    const uint8_t *m = sGlPair[sGlFlip ? 7 - r : r];
                    for (int k = 0; k < 4; k++)
                        buf32[m[k]] = To5551x2(src[2 * k] | ((uint32_t)src[2 * k + 1] << 16));
                }
                memcpy(GlTile(strip, tx, ty), buf32, sizeof(buf32));
            }
        memset(stripTiles[ty], 0, sizeof(stripTiles[ty]));
        GlTouch(ty * 8, ty * 8 + 8);
    }
    GlFlush(&sGlStrip);
    u64 t2 = svcGetSystemTick();
    sGlFrameStrip = t2 - t1;
    // The sprites (colour numbers): the slots shown this frame that were made
    // again, or whose bank's base colours changed (those not shown: later)
    uint16_t *obj = (uint16_t *)sGlObj.data;
    for (int i = 0; i < GPU_OBJ_SLOTS; i++) {
        GpuObjSlot *o = &gGpuObjSlots[i];
        if (!o->look)
            continue;
        if (changedObj && (o->bank == 0xFFFF || ((changedObj >> o->bank) & 1)))
            o->gpuDirty = 1;
        if (!o->gpuDirty || o->used != l->frame)
            continue;
        o->gpuDirty = 0;
        for (int ty = o->y >> 3; ty < (o->y + o->h) >> 3; ty++) // (slots are whole tiles)
            for (int tx = o->x >> 3; tx < (o->x + o->w) >> 3; tx++) {
                uint32_t buf32[32];
                            for (int r = 0; r < 8; r++) {
                    const uint16_t *src = &gGpuObj[ty * 8 + r][tx * 8];
                    const uint8_t *m = sGlPair[sGlFlip ? 7 - r : r];
                    for (int k = 0; k < 4; k++) {
                        unsigned a = src[2 * k], b = src[2 * k + 1];
                        uint32_t ta = (a & 0x8000) ? 0 : sGlLut[256 + (a & 0xFF)];
                        uint32_t tb = (b & 0x8000) ? 0 : sGlLut[256 + (b & 0xFF)];
                        buf32[m[k]] = ta | (tb << 16);
                    }
                }
                memcpy(GlTile(obj, tx, ty), buf32, sizeof(buf32));
            }
        GlTouch(o->y, o->y + o->h);
    }
    GlFlush(&sGlObj);
    u64 t3 = svcGetSystemTick();
    sGlFrameObj = t3 - t2;
    sGlUploadTicks += t3 - start;
}

// A texture coordinate: v at the top edge of image row y (v = 0: the top row,
// as the picture textures; where the row is in memory is GlTexel's)
static inline float GlV(int y)
{
    return (float)y / GL_TEX;
}

static int sGlState = -1; // (the last GlSetBlend's, -1: unknown)
static void GlSetBlend(bool blend, int stencilRef, GPU_TESTFUNC test, GPU_STENCILOP pass)
{
    int state = (blend ? 1 : 0) | (stencilRef << 1) | ((int)test << 2) | ((int)pass << 6);
    if (state == sGlState)
        return;
    sGlState = state;
    if (blend)
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_CONSTANT_COLOR, GPU_CONSTANT_ALPHA, GPU_ONE, GPU_ZERO);
    else
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    C3D_StencilTest(true, test, stencilRef, 0xFF, 0xFF);
    C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, pass);
}

// The list on its screen: picture (x, y, w, h) on the screen is the frame
static void GpuDrawList(C3D_RenderTarget *target, const C3D_Mtx *proj, const GpuList *l, unsigned level)
{
    const float sx = (float)sGpuMain.w / sGlFrameW, sy = (float)sGpuMain.h / sGlFrameH;
    const float px = sGpuMain.x + sGlOx * sx, py = sGpuMain.y + sGlOy * sy;
    C3D_Tex *texs[6] = { &sGlBg[0], &sGlBg[1], &sGlBg[2], &sGlBg[3], &sGlStrip, &sGlObj };
    static uint16_t order[GPU_MAX_QUADS], start[1024];

    C3D_RenderTargetClear(target, C3D_CLEAR_ALL, 0x000000FF, (u32)(l->bdSecond ? 1 : 0) << 24);
    C3D_FrameDrawOn(target);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, sProjLoc, proj);
    C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_COLOR); // (no depth: the stencil's ops on "pass")

    // The backdrop: the list's area in its colour (base entry 0, faded, and
    // as the screen change fades)
    C3D_TexEnv *env = C3D_GetTexEnv(0);
    unsigned rgb[3];
    for (int k = 0; k < 3; k++) {
        unsigned c8 = ((l->base[0] >> (5 * k)) & 31) * 255 / 31;
        unsigned f = c8 * l->fade.mul / 256 + l->fade.add[k];
        rgb[k] = (f > 255 ? 255 : f) * level / 255;
    }
    unsigned r = rgb[0], g = rgb[1], b = rgb[2];
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    C3D_TexEnvColor(env, 0xFF000000 | (b << 16) | (g << 8) | r);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_StencilTest(false, GPU_ALWAYS, 0, 0, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    {
        float x0 = px, y0 = py, x1 = px + l->w * sx, y1 = py + l->h * sy;
        C3D_ImmDrawBegin(GPU_TRIANGLE_STRIP);
        C3D_ImmSendAttrib(x0, y0, 0.5f, 0.0f); C3D_ImmSendAttrib(0.0f, 0.0f, 0.0f, 0.0f);
        C3D_ImmSendAttrib(x1, y0, 0.5f, 0.0f); C3D_ImmSendAttrib(0.0f, 0.0f, 0.0f, 0.0f);
        C3D_ImmSendAttrib(x0, y1, 0.5f, 0.0f); C3D_ImmSendAttrib(0.0f, 0.0f, 0.0f, 0.0f);
        C3D_ImmSendAttrib(x1, y1, 0.5f, 0.0f); C3D_ImmSendAttrib(0.0f, 0.0f, 0.0f, 0.0f);
        C3D_ImmDrawEnd();
    }

    // The rectangles: texel * mul + add (the list's fade: stages 0 and 1; not
    // for those already coloured, GQ_PREFADED), times the screen change
    // fade's grey (stage 2); transparent texels left out
    env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_CONSTANT, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
    C3D_TexEnv *env1 = C3D_GetTexEnv(1);
    C3D_TexEnvInit(env1);
    C3D_TexEnvSrc(env1, C3D_Both, GPU_PREVIOUS, GPU_CONSTANT, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env1, C3D_RGB, GPU_ADD);
    C3D_TexEnvFunc(env1, C3D_Alpha, GPU_REPLACE);
    C3D_TexEnv *env2 = C3D_GetTexEnv(2);
    C3D_TexEnvInit(env2);
    C3D_TexEnvSrc(env2, C3D_Both, GPU_PREVIOUS, GPU_CONSTANT, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env2, C3D_RGB, GPU_MODULATE);
    C3D_TexEnvFunc(env2, C3D_Alpha, GPU_REPLACE);
    C3D_TexEnvColor(env2, 0xFF000000 | (level << 16) | (level << 8) | level);
    unsigned mul = l->fade.mul >= 256 ? 255 : l->fade.mul;
    const u32 fadeMul = 0xFF000000 | (mul << 16) | (mul << 8) | mul;
    const u32 fadeAdd = 0xFF000000 | ((u32)l->fade.add[2] << 16) | ((u32)l->fade.add[1] << 8) | l->fade.add[0];
    int prefadedNow = -1;
    C3D_AlphaTest(true, GPU_GREATER, 0);
    sGlState = -1;
    unsigned eva = l->eva * 255 / 16, evb = l->evb * 255 / 16;
    C3D_BlendingColor((evb << 24) | (eva << 16) | (eva << 8) | eva);

    // Back to front: the keys from the largest down (each key's in the order added)
    memset(start, 0, sizeof(start));
    for (int i = 0; i < l->quadCount; i++)
        start[1023 - (l->quads[i].key & 1023)]++;
    for (int k = 0, sum = 0; k < 1024; k++) {
        int c = start[k];
        start[k] = (uint16_t)sum;
        sum += c;
    }
    for (int i = 0; i < l->quadCount; i++)
        order[start[1023 - (l->quads[i].key & 1023)]++] = (uint16_t)i;

    // Batches: the rectangles in a row with the same texture and state go in
    // one draw (as triangles); a first target's (two passes) only with its
    // own layer's (the same key: they don't overlap, so each pass can take
    // them all), as overlapping ones must be drawn in their order
    typedef struct { float x0, y0, x1, y1, u0, v0, u1, v1; } GlRect;
    static GlRect batch[128];
    int bn = 0, bTex = -1, bFlags = 0, bKey = -1, boundTex = -1;
#define GL_FLUSH_BATCH() do { \
        if (bn) { \
            if (bTex != boundTex) { C3D_TexBind(0, texs[bTex]); boundTex = bTex; } \
            int prefaded = (bFlags & GQ_PREFADED) ? 1 : 0; \
            if (prefaded != prefadedNow) { \
                C3D_TexEnvColor(env, prefaded ? 0xFFFFFFFF : fadeMul); \
                C3D_TexEnvColor(env1, prefaded ? 0xFF000000 : fadeAdd); \
                C3D_DirtyTexEnv(env); \
                C3D_DirtyTexEnv(env1); \
                prefadedNow = prefaded; \
            } \
            int passes = (bFlags & GQ_BLEND) ? 2 : 1; \
            for (int pass = 0; pass < passes; pass++) { \
                if (passes == 1) /* covers; the flag: whether it is a second target */ \
                    GlSetBlend(false, (bFlags & GQ_SECOND) ? 1 : 0, GPU_ALWAYS, GPU_STENCIL_REPLACE); \
                else if (pass == 0) /* a first target over what isn't a second one: as it is */ \
                    GlSetBlend(false, 0, GPU_EQUAL, GPU_STENCIL_KEEP); \
                else /* ... over a second target: blended, and the flag cleared */ \
                    GlSetBlend(true, 1, GPU_EQUAL, GPU_STENCIL_ZERO); \
                C3D_ImmDrawBegin(GPU_TRIANGLES); \
                for (int k = 0; k < bn; k++) { \
                    const GlRect *r = &batch[k]; \
                    C3D_ImmSendAttrib(r->x0, r->y0, 0.5f, 0.0f); C3D_ImmSendAttrib(r->u0, r->v0, 0.0f, 0.0f); \
                    C3D_ImmSendAttrib(r->x1, r->y0, 0.5f, 0.0f); C3D_ImmSendAttrib(r->u1, r->v0, 0.0f, 0.0f); \
                    C3D_ImmSendAttrib(r->x0, r->y1, 0.5f, 0.0f); C3D_ImmSendAttrib(r->u0, r->v1, 0.0f, 0.0f); \
                    C3D_ImmSendAttrib(r->x1, r->y0, 0.5f, 0.0f); C3D_ImmSendAttrib(r->u1, r->v0, 0.0f, 0.0f); \
                    C3D_ImmSendAttrib(r->x1, r->y1, 0.5f, 0.0f); C3D_ImmSendAttrib(r->u1, r->v1, 0.0f, 0.0f); \
                    C3D_ImmSendAttrib(r->x0, r->y1, 0.5f, 0.0f); C3D_ImmSendAttrib(r->u0, r->v1, 0.0f, 0.0f); \
                } \
                C3D_ImmDrawEnd(); \
            } \
            bn = 0; \
        } \
    } while (0)

    for (int n = 0; n < l->quadCount; n++) {
        const GpuQuad *q = &l->quads[order[n]];
        // Cut to the list's area (sprites can reach past it)
        int dx0 = q->dx, dy0 = q->dy, dx1 = q->dx + q->w, dy1 = q->dy + q->h;
        int cx0 = dx0 < 0 ? 0 : dx0, cy0 = dy0 < 0 ? 0 : dy0;
        int cx1 = dx1 > l->w ? l->w : dx1, cy1 = dy1 > l->h ? l->h : dy1;
        if (cx0 >= cx1 || cy0 >= cy1)
            continue;
        // The texels at the cut rectangle's edges (flips: read from the other end)
        int u0, u1, t0, t1;
        if (q->flags & GQ_HFLIP) { u0 = q->sx + (dx1 - cx0); u1 = q->sx + (dx1 - cx1); }
        else                     { u0 = q->sx + (cx0 - dx0); u1 = q->sx + (cx1 - dx0); }
        if (q->flags & GQ_VFLIP) { t0 = q->sy + (dy1 - cy0); t1 = q->sy + (dy1 - cy1); }
        else                     { t0 = q->sy + (cy0 - dy0); t1 = q->sy + (cy1 - dy0); }
        int flags = q->flags & (GQ_BLEND | GQ_SECOND | GQ_PREFADED);
        if (bn && (q->tex != bTex || flags != bFlags || ((flags & GQ_BLEND) && q->key != bKey)
                   || bn == (int)(sizeof(batch) / sizeof(batch[0]))))
            GL_FLUSH_BATCH();
        bTex = q->tex;
        bFlags = flags;
        bKey = q->key;
        GlRect *r = &batch[bn++];
        r->x0 = px + cx0 * sx; r->y0 = py + cy0 * sy; r->x1 = px + cx1 * sx; r->y1 = py + cy1 * sy;
        r->u0 = (float)u0 / GL_TEX; r->u1 = (float)u1 / GL_TEX; r->v0 = GlV(t0); r->v1 = GlV(t1);
    }
    GL_FLUSH_BATCH();
#undef GL_FLUSH_BATCH

    // Back to how the pictures are drawn
    C3D_TexEnvInit(C3D_GetTexEnv(1));
    C3D_TexEnvInit(C3D_GetTexEnv(2));
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_StencilTest(false, GPU_ALWAYS, 0, 0, 0);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
}


// A screen's swap (asked for when the GPU finished copying its picture to
// the framebuffer) happens at its next VBlank; until then the screen still
// shows the other buffer, the one the next copy goes into. Copying over the
// picture being shown is what tore the screen (one side a frame ahead: seen
// in screen fades, where every frame changes). So before the next copy, wait
// for that swap: asked for (the back buffer changed) and done (none pending).
static void *sGpuCopiedTo[2]; // each screen's buffer the last copy went into
static u32 sBusyDrops;        // Busy Screen: Skip: main pictures left out (port.log)

static void GpuWaitForSwap(gfxScreen_t screen)
{
    if (sGpuCopiedTo[screen] == NULL)
        return;
    u64 giveUp = svcGetSystemTick() + SYSCLOCK_ARM11 / 30; // (2 frames: a swap asked for comes at the next VBlank, or not at all)
    // (C3D_FrameBegin waited for the last frame's copies, and citro3d asks
    // for each copied screen's swap as its copy ends: only a swap still
    // pending is waited for. Waiting for the buffer to change too waited the
    // whole time at one change of map every run, the second screen's swap
    // never asked for.)
    while (gspIsPresentPending(screen) && svcGetSystemTick() < giveUp)
        gspWaitForAnyEvent();
    if (svcGetSystemTick() >= giveUp) {
        static int sLogged;
        if (sLogged++ < 20)
            Backend_Log("swap wait gave up: %s screen", screen == GFX_TOP ? "top" : "bottom");
    }
    sGpuCopiedTo[screen] = NULL;
}

// The pictures on the screens, those that changed since they were last drawn
// (a new picture, place, Scale setting or fade level). A screen with nothing
// new keeps showing what it shows: no GPU work, no swap.
static void GpuDrawFrame(void)
{
    unsigned level = (unsigned)(sFadeLevel * 255 / FADE_FRAMES);
    unsigned subLevel = level * (unsigned)Port_SubScreenBrightness() >> 8; // (its own fades too)
    gfxScreen_t mainScreen = sBottom ? GFX_BOTTOM : GFX_TOP, subScreen = sBottom ? GFX_TOP : GFX_BOTTOM;
    bool drawMain = sGpuMain.shown && (sGpuMain.dirty || sGpuMain.level != level);
    bool drawSub = sGpuSub.shown && sSubModeIsPicture() && (sGpuSub.dirty || sGpuSub.level != subLevel);
    // Busy Screen: Skip. The main screen's last picture still waits for its
    // swap (at the next VBlank: after a late frame, no VBlank was waited for)
    // and the next one could only go into the buffer it shows: rather than
    // wait for it (up to a whole refresh, making this frame late too), this
    // frame leaves its picture out; the next frame draws its own (still
    // dirty). Not more than 2 frames in a row (a swap that never comes).
    static int sBusySkipped;
    if (drawMain && Port_BusySkip() && sGpuCopiedTo[mainScreen] != NULL && gspIsPresentPending(mainScreen)
        && sBusySkipped < 2) {
        drawMain = false;
        sBusySkipped++;
        sBusyDrops++;
    }
    else if (drawMain)
        sBusySkipped = 0;
    if (!drawMain && !drawSub)
        return;
    C3D_FrameBegin(0); // (waits for the last frame's copies)
    // The pictures' copies into their textures: in the frame's queue, before
    // its drawing (C3D_SyncDisplayTransfer inside a frame only queues it).
    // Waiting for each copy outside a frame cost the second screen ~4 ms
    // whenever it changed (the world map's water) on an Old 3DS.
    GpuPicture *pics[2] = { &sGpuMain, &sGpuSub };
    for (int i = 0; i < 2; i++)
        if (pics[i]->transfer) {
            C3D_SyncDisplayTransfer((u32 *)pics[i]->stage, GX_BUFFER_DIM(TEX_W, TEX_H), (u32 *)pics[i]->tex.data,
                                    GX_BUFFER_DIM(TEX_W, TEX_H), TEX_TRANSFER_FLAGS);
            pics[i]->transfer = false;
        }
    if (drawMain)
        GpuWaitForSwap(mainScreen);
    if (drawSub)
        GpuWaitForSwap(subScreen);
    if (drawMain && sGlList != NULL) { // GPU Drawing: the drawing list
        if (!sGlUploaded) {
            GpuListUpload(sGlList);
            sGlUploaded = true;
        }
        GpuDrawList(sBottom ? sTargetBottom : sTargetTop, sBottom ? &sProjBottom : &sProjTop, sGlList, level);
    }
    // The screen change fade: the texture's colours times a grey
    C3D_TexEnv *env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_CONSTANT, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
    C3D_TexEnvColor(env, 0xFF000000 | (level << 16) | (level << 8) | level);
    if (drawMain) {
        C3D_TexEnvColor(env, 0xFF000000 | (level << 16) | (level << 8) | level);
        C3D_DirtyTexEnv(env); // (a new colour alone is not sent again)
        if (sGlList == NULL)
            GpuDrawPicture(sBottom ? sTargetBottom : sTargetTop, sBottom ? &sProjBottom : &sProjTop, &sGpuMain);
        sGpuMain.dirty = false;
        sGpuMain.level = level;
        sGpuCopiedTo[mainScreen] = gfxGetFramebuffer(mainScreen, GFX_LEFT, NULL, NULL);
    }
    if (drawSub) {
        C3D_TexEnvColor(env, 0xFF000000 | (subLevel << 16) | (subLevel << 8) | subLevel);
        C3D_DirtyTexEnv(env); // (a new colour alone is not sent again)
        GpuDrawPicture(sBottom ? sTargetTop : sTargetBottom, sBottom ? &sProjTop : &sProjBottom, &sGpuSub);
        sGpuSub.dirty = false;
        sGpuSub.level = subLevel;
        sGpuCopiedTo[subScreen] = gfxGetFramebuffer(subScreen, GFX_LEFT, NULL, NULL);
    }
    C3D_FrameEnd(GX_CMDLIST_FLUSH); // (only the command list: the pictures were flushed)
}

// Where a picture goes on a screen (screenW x 240), as its Scale setting says:
// None 1:1, Full as large as fits (the same shape), Stretch the whole screen.
static void ScaleRect(int frameW, int frameH, int screenW, int scale, int *x, int *y, int *w, int *h)
{
    if (scale == PORT_SCALE_STRETCH) {
        *x = 0; *y = 0; *w = screenW; *h = 240;
        return;
    }
    *w = frameW; *h = frameH;
    if (scale == PORT_SCALE_FULL) {
        if (screenW * frameH <= 240 * frameW) { // the width fills the screen
            *w = screenW;
            *h = frameH * screenW / frameW;
        } else {                                 // the height
            *h = 240;
            *w = frameW * 240 / frameH;
        }
    }
    *x = (screenW - *w) / 2;
    *y = (240 - *h) / 2;
}

// The main screen's picture, placed as its Scale setting says.
static void GpuPresentMain(const uint16_t *frame, int frameW, int frameH)
{
    int x, y, w, h;
    ScaleRect(frameW, frameH, sBottom ? 320 : 400, Port_ScreenScale(0), &x, &y, &w, &h);
    GpuUpload(&sGpuMain, frame, frameW, frameH, x, y, w, h, 0, frameH);
}

static void PresentMain(const uint16_t *frame, int frameW, int frameH)
{
    sScaled = Port_ScreenScale(0) != PORT_SCALE_NONE; // (the CPU: Full and Stretch alike)
    sGlList = NULL; // (the picture, not a drawing list)
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
            sGpuCopiedTo[screen] = NULL; // (the text: one buffer, no swaps)
        }
        return;
    }
    // The second screen is double-buffered: what changed is drawn into this
    // frame's buffer, and again into the other one next frame (it missed it).
    // While nothing changes, neither is touched.
    static int sSubFade = -1, sSubShownW;
    static int sPend[4]; // the area the other buffer still lacks (x0, y0, x1, y1)
    int area[4] = { 0, 0, 0, 0 };
    int full = !sGpu && sFadeLevel != sSubFade; // (the GPU fades it itself)
    sSubFade = sFadeLevel;
    if (sSubMode != SUB_PICTURE || subW != sSubShownW) { // (a new size: the old border cleared)
        sSubShownW = subW;
        full = 1;
        gfxSetScreenFormat(screen, GSP_RGB565_OES);
        gfxSetDoubleBuffering(screen, true);
        sGpuCopiedTo[screen] = NULL; // (its buffers set up again: no swap of the last copy to wait for)
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
    if (sGpu) { // the rows that changed, when anything did
        if (area[0] < area[2] && area[1] < area[3]) {
            int x, y, w, h;
            ScaleRect(subW, subH, width, Port_ScreenScale(1), &x, &y, &w, &h);
            GpuUpload(&sGpuSub, sSubFrame, subW, subH, x, y, w, h, area[1], area[3]); // (only the rows that changed)
        } else if (sGpuSub.shown) { // (the setting may have changed)
            int x, y, w, h;
            ScaleRect(subW, subH, width, Port_ScreenScale(1), &x, &y, &w, &h);
            if (x != sGpuSub.x || y != sGpuSub.y || w != sGpuSub.w || h != sGpuSub.h) {
                sGpuSub.x = x; sGpuSub.y = y; sGpuSub.w = w; sGpuSub.h = h;
                sGpuSub.dirty = true;
            }
        }
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

static void WorkerTicks(u32 *all, u32 *music);

static void MeterEndFrame(void)
{
    u64 now = svcGetSystemTick();
    if (sMeterStart == 0) {
        sMeterStart = sMeterBoot = now;
        u32 a, m;
        WorkerTicks(&a, &m); // (from now on)
        return;
    }
    sMeterFrames++;
    sMeterLate += sLate;
    // A slow frame (over 20 ms): logged with where its time went (a fade's
    // hitch is lost in the 5-second lines); up to 20 every 5 seconds, kept
    // here and handed to the log's thread with the 5-second line.
    static char slowLines[20][320];
    static int slowCount;
    {
        static u64 lastNow, lastPart[T_COUNT], lastUp;
        double ms = 1000.0 / SYSCLOCK_ARM11, took = lastNow ? (now - lastNow) * ms : 0;
        if (took > 20.0 && sLog != NULL && slowCount < 20) {
            char *line = slowLines[slowCount];
            const int lineSize = (int)sizeof(slowLines[0]);
            int n = snprintf(line, lineSize, "slow frame t=%.2fs %.1f ms:", (now - sMeterBoot) / (double)SYSCLOCK_ARM11, took);
            for (int i = 0; i < T_COUNT && n < lineSize; i++)
                n += snprintf(line + n, lineSize - n, " %s %.1f", sPartNames[i],
                              (sPartTicks[i] >= lastPart[i] ? sPartTicks[i] - lastPart[i] : sPartTicks[i]) * ms); // (reset every 5 s)
            if (n < lineSize)
                n += snprintf(line + n, lineSize - n, " | up %.1f %s",
                              (sGlUploadTicks >= lastUp ? sGlUploadTicks - lastUp : sGlUploadTicks) * ms,
                              sGlList == NULL ? "cpu-picture" : sGlList->palette == GPU_PAL_FADE ? "list fade" :
                              sGlList->palette == GPU_PAL_OTHER ? "list palette-change" : "list");
            if (sGlList != NULL && sGlList->fadeOn && n < lineSize)
                n += snprintf(line + n, lineSize - n, " (mul %d add %d,%d,%d)", sGlList->fade.mul,
                              sGlList->fade.add[0], sGlList->fade.add[1], sGlList->fade.add[2]);
            if (sGlList != NULL && n < lineSize)
                snprintf(line + n, lineSize - n, " | quads %d tiles %d (palette %d): bg %.1f strips %.1f obj %.1f (cache flush %.1f)",
                         sGlList->quadCount, sGlFrameTiles, sGlFramePalTiles, sGlFrameBg * ms, sGlFrameStrip * ms, sGlFrameObj * ms,
                         sGlFrameFlush * ms);
            slowCount++;
        }
        lastNow = now;
        memcpy(lastPart, sPartTicks, sizeof(lastPart));
        lastUp = sGlUploadTicks;
    }
    if (now - sMeterStart < 5 * (u64)SYSCLOCK_ARM11)
        return;
    if (sLog != NULL) {
        for (int i = 0; i < slowCount; i++)
            Backend_Log("%s", slowLines[i]);
        slowCount = 0;
        char line[768];
        double ms = 1000.0 / SYSCLOCK_ARM11, secs = (now - sMeterStart) / (double)SYSCLOCK_ARM11;
        bool isNew = false;
        APT_CheckNew3DS(&isNew);
        int n = snprintf(line, sizeof(line), "perf t=%us %s %s fps=%.1f drawn=%d/%d late=%d %s |",
            (unsigned)((now - sMeterBoot) / SYSCLOCK_ARM11), isNew ? "New3DS" : "Old3DS", Port_SceneName(),
            sMeterFrames / secs, sMeterFrames - sMeterSkipped, sMeterFrames, sMeterLate, Port_SettingsSummary());
        for (int i = 0; i < T_COUNT && n < (int)sizeof(line); i++)
            n += snprintf(line + n, sizeof(line) - n, " %s %.1f", sPartNames[i], sPartTicks[i] * ms / sMeterFrames);
        if (n < (int)sizeof(line))
            n += snprintf(line + n, sizeof(line) - n, " ms/frame");
        { // the second core: busy, and of that the DS music (ms/frame; its own share of the time)
            u32 busy, music;
            WorkerTicks(&busy, &music);
            if (n < (int)sizeof(line))
                n += snprintf(line + n, sizeof(line) - n, " | core2 %.1f (music %.1f) ms/frame, %d%% busy",
                              busy * ms / sMeterFrames, music * ms / sMeterFrames, (int)(busy * 100.0 / (now - sMeterStart)));
        }
        if (n < (int)sizeof(line))
            n += snprintf(line + n, sizeof(line) - n, " | sub: %s", Port_SubScreenStats());
        if (sGlReady && n < (int)sizeof(line)) { // GPU Drawing: frames from the list, its textures, why not
            uint32_t listed, why[GPU_WHY_COUNT];
            Port_GpuListCounts(&listed, why);
            n += snprintf(line + n, sizeof(line) - n, " | gpu: %s list=%lu/%d up %.1f ms/frame",
                          Port_GpuDraw() ? "on" : "off", (unsigned long)(listed - sGlLastListed),
                          sMeterFrames - sMeterSkipped, sGlUploadTicks * ms / sMeterFrames);
            for (int i = 1; i < GPU_WHY_COUNT && n < (int)sizeof(line); i++)
                if (why[i] != sGlLastWhy[i])
                    n += snprintf(line + n, sizeof(line) - n, " %s=%lu", gGpuWhy[i], (unsigned long)(why[i] - sGlLastWhy[i]));
            sGlLastListed = listed;
            memcpy(sGlLastWhy, why, sizeof(why));
            sGlUploadTicks = 0;
        }
        if (Port_BusySkip() && n < (int)sizeof(line)) {
            static u32 sLastDrops;
            n += snprintf(line + n, sizeof(line) - n, " | busy: skipped %lu", (unsigned long)(sBusyDrops - sLastDrops));
            sLastDrops = sBusyDrops;
        }
        LogLine(line);
    }
    memset(sPartTicks, 0, sizeof(sPartTicks));
    sMeterFrames = sMeterSkipped = sMeterLate = 0;
    sMeterStart = now;
}

void Backend_Present(const uint16_t *frame, int width, int height)
{
    // NULL: a frame Frame Skip did not draw. During the screen change fade
    // the last picture is shown again (the fade changes its colours).
    if (frame == NULL && sFadeDir != 0 && sGlList == NULL)
        frame = Port_Framebuffer(); // (a list: drawn again as it is, GpuDrawFrame)
    sMainPresented = frame != NULL;
    sMarkTick = svcGetSystemTick();
    if (frame != NULL)
        PresentMain(frame, width, height);
    Part(T_OUT);
    PresentSub(); // (times T_SUB and T_SUBOUT)
}

// GPU Drawing: the frame is the drawing list (frame.c), drawn in GpuDrawFrame
void Backend_PresentList(const void *list, int frameW, int frameH, int ox, int oy)
{
    int x, y, w, h;
    sMainPresented = 1;
    sMarkTick = svcGetSystemTick();
    ScaleRect(frameW, frameH, sBottom ? 320 : 400, Port_ScreenScale(0), &x, &y, &w, &h);
    sGlList = (const GpuList *)list;
    sGlUploaded = false;
    sGlFrameW = frameW; sGlFrameH = frameH;
    sGlOx = ox; sGlOy = oy;
    sGpuMain.shown = true;
    sGpuMain.dirty = true;
    sGpuMain.x = x; sGpuMain.y = y; sGpuMain.w = w; sGpuMain.h = h;
    sGpuMain.srcW = 0; // (the picture texture no longer holds what is shown)
    // The end of a fade out (frame.c: an empty list faded to black): on the
    // screen now, before the second screen's picture (a new map's can take
    // 50 ms) and whatever loading the game does next, so the fade doesn't
    // hang on its last dim frame
    const GpuList *l = sGlList;
    if (l->quadCount == 0 && l->fadeOn && l->fade.mul == 0 && !(l->fade.add[0] | l->fade.add[1] | l->fade.add[2]))
        GpuDrawFrame();
    Part(T_OUT);
    PresentSub(); // (times T_SUB and T_SUBOUT)
}

// Coming out of black (frame.c): the list's textures sent, all of them, but
// not drawn (the screen stays black). The GPU isn't reading them: what it
// draws now is the empty black list.
void Backend_UploadList(const void *list)
{
    if (!sGlReady || !sGpu)
        return;
    sGlNoBudget = true;
    GpuListUpload((const GpuList *)list);
    sGlNoBudget = false;
}

// --- Dual Core ------------------------------------------------------------------
//
// A thread on the second CPU core runs what Backend_RunAsync is given: only
// the DS music and sound effects (port/core/music.c; the drawing is all on
// this core). If it cannot be started, the work is done at once on this core.
//
// A New 3DS has a core of its own for apps (core 2). An Old 3DS lends apps a
// share of its system core (core 1), which the system's own services (the
// screens, sound, the HOME Menu) then do without: asking for 80% made those
// slow (port.log: sound and flushing took twice as long, even with Dual Core
// turned off again); 50% left the second core too little to draw with (when
// it drew part of the picture); 70% was fine, and 75% is being tried. So
// the share is CPU_LIMIT_ON only while Dual Core is on
// and the game is in front, and the least the system takes (CPU_LIMIT_OFF)
// otherwise; the thread is stopped when Dual Core is turned off.

#define CPU_LIMIT_ON 75
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
static bool sWorkerTried, sWorkerQuit;
static LightEvent sWorkGo, sWorkDone;
// The jobs given and not done yet, in order (the DS music's next frame, then
// a part of the picture, say): the worker takes them from sJobHead, this
// core adds them at sJobTail.
#define WORK_JOBS 4
static struct { void (*fn)(void *); void *arg; } sJobs[WORK_JOBS];
static volatile unsigned sJobHead, sJobTail;
// The performance meter: the worker's busy time (all of it, and the DS music's)
static volatile u32 sWorkerTicks, sWorkerMusicTicks; // (only the worker adds to them)

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
        while (sJobHead != sJobTail) {
            unsigned at = sJobHead % WORK_JOBS;
            void (*fn)(void *) = sJobs[at].fn;
            u64 t0 = svcGetSystemTick();
            fn(sJobs[at].arg);
            u32 dt = (u32)(svcGetSystemTick() - t0);
            sWorkerTicks += dt;
            if (fn == Port_MusicRenderJob)
                sWorkerMusicTicks += dt;
            __sync_synchronize();
            sJobHead++;
            LightEvent_Signal(&sWorkDone);
        }
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
    // The lowest priority: on the system core, the system's services (GSP,
    // which this core asks to send the GPU its work and copy pictures) come
    // first instead of waiting for the frame's DS music (up to 2 ms)
    (void)prio;
    sWorker = threadCreate(WorkerMain, NULL, 32 * 1024, 0x3F, isNew ? 2 : 1, false);
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
    while (sJobTail - sJobHead >= WORK_JOBS) // (full: never, with two at most)
        LightEvent_Wait(&sWorkDone);
    unsigned at = sJobTail % WORK_JOBS;
    sJobs[at].fn = fn;
    sJobs[at].arg = arg;
    __sync_synchronize();
    sJobTail++;
    LightEvent_Signal(&sWorkGo);
}

// All the jobs given done
void Backend_WaitAsync(void)
{
    while (sJobHead != sJobTail)
        LightEvent_Wait(&sWorkDone);
    __sync_synchronize();
}

bool Backend_AsyncDone(void)
{
    if (sJobHead != sJobTail)
        return false;
    __sync_synchronize();
    return true;
}

// The performance meter: the worker's busy time since the last call (ticks)
static void WorkerTicks(u32 *all, u32 *music)
{
    static u32 sLastAll, sLastMusic;
    u32 a = sWorkerTicks, m = sWorkerMusicTicks;
    *all = a - sLastAll;
    *music = m - sLastMusic;
    sLastAll = a;
    sLastMusic = m;
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

int Backend_GameOnBottom(void)
{
    return sBottom;
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
    sGpuMain.dirty = sGpuSub.dirty = true; // (both drawn again, on their new screens)
    sGpuCopiedTo[0] = sGpuCopiedTo[1] = NULL;
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

// The settings are in config.ini now (options.c, through Backend_FileWrite);
// config.bin is only read, once, when there is no config.ini yet.
void Backend_StoreConfig(const uint8_t *data, uint32_t size)
{
    (void)data;
    (void)size;
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
    LogStop();
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
    LogLine(buf);
}

void Backend_Fatal(const char *msg)
{
    LogStop(); // (the message straight to the file)
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
    if (flag) {
        fclose(flag);
        sLog = fopen(LOG_PATH, "w");
        if (sLog != NULL)
            LogStart();
    }

    BuildLut();
    BuildMaps();
    GpuInit();
    AudioInit();
    SetGameScreen(Port_WantsBottomScreen());

    Port_Init();
    AgbMain();
    Backend_Fatal("AgbMain returned");
}
