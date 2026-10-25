// Nintendo 3DS backend (libctru).
//
// Faithful mode: the game's 240x160 picture on the top screen, either 1:1 in
// the centre or stretched 1.5x to 360x240 (toggle with Y). The bottom screen
// shows a short status text. Sound goes through the DSP (needs
// sdmc:/3ds/dspfirm.cdc, which most custom firmware setups already have).
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
#define DEBUG_FLAG DATA_DIR "/debug.txt"

void AgbMain(void);

static uint16_t sLut[0x8000]; // GBA BGR555 -> RGB565
static int sScaled;
static FILE *sLog;

// --- Video -------------------------------------------------------------------

static void BuildLut(void)
{
    for (unsigned c = 0; c < 0x8000; c++) {
        unsigned r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
        unsigned g6 = (g << 1) | (g >> 4);
        sLut[c] = (uint16_t)((r << 11) | (g6 << 5) | b);
    }
}

void Backend_Present(const uint16_t *frame)
{
    // The top framebuffer is 240 pixels per column, columns left to right,
    // each column stored bottom to top.
    uint16_t *fb = (uint16_t *)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);

    if (!sScaled) {
        const int ox = (400 - GBA_W) / 2, oy = (240 - GBA_H) / 2;
        for (int x = 0; x < GBA_W; x++) {
            uint16_t *col = fb + (ox + x) * 240 + (239 - oy);
            for (int y = 0; y < GBA_H; y++)
                col[-y] = sLut[frame[y * GBA_W + x] & 0x7FFF];
        }
        return;
    }

    // 1.5x: every source pixel becomes 1 or 2 screen pixels in each direction.
    const int ox = (400 - 360) / 2;
    for (int sx = 0; sx < 360; sx++) {
        int x = sx * 2 / 3;
        uint16_t *col = fb + (ox + sx) * 240 + 239;
        for (int sy = 0; sy < 240; sy++)
            col[-sy] = sLut[frame[(sy * 2 / 3) * GBA_W + x] & 0x7FFF];
    }
}

static void ClearTop(void)
{
    for (int i = 0; i < 2; i++) {
        memset(gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL), 0, 400 * 240 * 2);
        gfxFlushBuffers();
        gfxSwapBuffers();
    }
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

static int sAudioOn;
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

void Backend_QueueAudio(const int8_t *left, const int8_t *right, int count, int rate)
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
        int16_t *d = wb->data_pcm16;
        for (int s = 0; s < count; s++) {
            d[s * 2] = (int16_t)(left[s] * 256);
            d[s * 2 + 1] = (int16_t)(right[s] * 256);
        }
        wb->nsamples = (u32)count;
        DSP_FlushDataCache(d, (u32)count * 4);
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
        ClearTop();
    }
    FlushSave();
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
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
    gfxSetScreenFormat(GFX_TOP, GSP_RGB565_OES);
    gfxSetDoubleBuffering(GFX_TOP, true);
    consoleInit(GFX_BOTTOM, NULL);
    osSetSpeedupEnable(true); // 804 MHz on New 3DS models

    mkdir("sdmc:/3ds", 0777);
    mkdir(DATA_DIR, 0777);
    FILE *flag = fopen(DEBUG_FLAG, "r");
    if (flag) { fclose(flag); sLog = fopen(LOG_PATH, "w"); }

    BuildLut();
    ClearTop();
    AudioInit();

    printf("\x1b[1mPokemon Mystery Dungeon: Red Rescue Team\x1b[0m\n");
    printf("pmd-red-3ds (early test build)\n\n");
    printf("Y      : 1:1 / 1.5x picture\n");
    printf("HOME   : exit (save in-game first)\n\n");
    printf("Save: %s\n", SAVE_PATH);
    if (!sAudioOn)
        printf("\n\x1b[33mNo sound: sdmc:/3ds/dspfirm.cdc missing.\x1b[0m\n");

    Port_Init();
    AgbMain();
    Backend_Fatal("AgbMain returned");
}
