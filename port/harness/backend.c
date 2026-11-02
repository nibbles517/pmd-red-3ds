// Backend for the CPU-emulator test harness (tools/harness.py).
//
// Talks to the host through a page of fake I/O registers at 0xF0000000 that the
// harness intercepts.

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "../core/port.h"

#define IO ((volatile uint32_t *)0xF0000000)
enum {
    IO_PUTCHAR = 0, IO_PRESENT = 1, IO_KEYS = 2, IO_ENDFRAME = 3, IO_EXIT = 4,
    IO_BUF = 5, IO_LEN = 6, IO_CMD = 7, IO_RESULT = 8,
    IO_AUDIO_L = 9, IO_AUDIO_R = 10, IO_AUDIO_RATE = 11, IO_AUDIO_COUNT = 12,
    IO_CAPTURE = 13, // read: nonzero = capture the next frame; write: buffer address when done
    IO_DUMPMAPS = 15, // read at start: nonzero = render both world maps (written back at IO_SUBSCREEN... see HarnessMain)
    IO_SUBSCREEN = 14, // read: nonzero = render the second screen; write: its buffer (0 = nothing)
    IO_LATE = 16,      // read: nonzero = report the last frame as late (Frame Skip: Auto)
    IO_PRESENT_SIZE = 17, // write: the next IO_PRESENT's width << 16 | height
    IO_SUB_SIZE = 18,     // write: the next IO_SUBSCREEN picture's width << 16 | height
    IO_ARG = 19, IO_HANDLE = 20, // data files: offset, handle
};
enum { CMD_SAVE_STORE = 1, CMD_SAVE_LOAD = 2, CMD_CONFIG_STORE = 3, CMD_CONFIG_LOAD = 4, CMD_SAVE_SLOT = 5,
       CMD_FILE_OPEN = 6, CMD_FILE_SIZE = 7, CMD_FILE_READ = 8, CMD_FILE_CLOSE = 9, CMD_FILE_WRITE = 10 };

const int Backend_Version16_UpdateRepoIfMissing = 16;

// Dual Core: one core here, so the second core's part is drawn at once.
void Backend_RunAsync(void (*fn)(void *), void *arg) { fn(arg); }
void Backend_WaitAsync(void) {}
bool Backend_AsyncDone(void) { return true; }
bool Backend_IsNew3DS(void) { return false; }

void Backend_Mark(int mark, int skipped) { (void)mark; (void)skipped; }

void AgbMain(void);

static void Puts(const char *s)
{
    while (*s)
        IO[IO_PUTCHAR] = (uint8_t)*s++;
}

void Backend_Log(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Puts(buf);
    Puts("\n");
}

void Backend_Fatal(const char *msg)
{
    Puts("FATAL: ");
    Puts(msg);
    Puts("\n");
    IO[IO_EXIT] = 1;
    for (;;) {}
}

bool Backend_FrameLate(void) { return IO[IO_LATE] != 0; }

void Backend_Present(const uint16_t *frame, int width, int height)
{
    static const uint16_t *sLast;
    static int sLastW, sLastH;
    if (frame == NULL) { // skipped: the last picture again
        frame = sLast;
        width = sLastW;
        height = sLastH;
        if (frame == NULL)
            return;
    }
    sLast = frame;
    sLastW = width;
    sLastH = height;
    // the harness takes 256x192 (or 320x240 in Fit size): a GBA-size picture
    // goes in the middle of a 256x192 one
    static uint16_t ds[256 * 192];
    if (width == GBA_W) {
        memset(ds, 0, sizeof(ds));
        for (int y = 0; y < height; y++)
            memcpy(&ds[(y + 16) * 256 + 8], &frame[y * GBA_W], GBA_W * 2);
        frame = ds;
        width = 256;
        height = 192;
    }
    static uint16_t sub[SUBSCREEN_W_MAX * SUBSCREEN_H_MAX];
    if (IO[IO_SUBSCREEN]) {
        bool shown = Port_RenderSubScreen(sub);
        int sw, sh;
        Port_SubScreenSize(&sw, &sh);
        IO[IO_SUB_SIZE] = (uint32_t)(sw << 16 | sh);
        IO[IO_SUBSCREEN] = shown ? (uint32_t)(uintptr_t)sub : 0;
    }
    IO[IO_PRESENT_SIZE] = (uint32_t)(width << 16 | height);
    IO[IO_PRESENT] = (uint32_t)(uintptr_t)frame;
}
uint16_t Backend_PollKeys(void) { return (uint16_t)IO[IO_KEYS]; }
void Backend_EndFrame(void) { IO[IO_ENDFRAME] = (uint32_t)Port_FrameCount(); }

void Backend_QueueAudio(const int16_t *stereo, int count, int rate)
{
    IO[IO_AUDIO_L] = (uint32_t)(uintptr_t)stereo;
    IO[IO_AUDIO_RATE] = (uint32_t)rate;
    IO[IO_AUDIO_COUNT] = (uint32_t)count; // the write triggers the copy
}

bool Backend_LoadSave(uint8_t *data, uint32_t size)
{
    IO[IO_BUF] = (uint32_t)(uintptr_t)data;
    IO[IO_LEN] = size;
    IO[IO_CMD] = CMD_SAVE_LOAD;
    return IO[IO_RESULT] != 0;
}

// harness.py --save FILE is Save Data 1; FILE-2 and FILE-3 the others.
void Backend_SetSaveSlot(int slot)
{
    IO[IO_LEN] = (uint32_t)slot;
    IO[IO_CMD] = CMD_SAVE_SLOT;
}

// Data files: harness.py --data DIR (default: the --save file's folder).
void *Backend_FileOpen(const char *name)
{
    IO[IO_BUF] = (uint32_t)(uintptr_t)name;
    IO[IO_LEN] = (uint32_t)strlen(name);
    IO[IO_CMD] = CMD_FILE_OPEN;
    return (void *)(uintptr_t)IO[IO_RESULT]; // a handle number, 0 = none
}

uint32_t Backend_FileSize(void *file)
{
    IO[IO_HANDLE] = (uint32_t)(uintptr_t)file;
    IO[IO_CMD] = CMD_FILE_SIZE;
    return IO[IO_RESULT];
}

bool Backend_FileRead(void *file, uint32_t offset, void *dest, uint32_t size)
{
    IO[IO_HANDLE] = (uint32_t)(uintptr_t)file;
    IO[IO_ARG] = offset;
    IO[IO_BUF] = (uint32_t)(uintptr_t)dest;
    IO[IO_LEN] = size;
    IO[IO_CMD] = CMD_FILE_READ;
    return IO[IO_RESULT] != 0;
}

void Backend_FileClose(void *file)
{
    IO[IO_HANDLE] = (uint32_t)(uintptr_t)file;
    IO[IO_CMD] = CMD_FILE_CLOSE;
}

bool Backend_FileWrite(const char *name, const void *data, uint32_t size)
{
    IO[IO_HANDLE] = (uint32_t)(uintptr_t)name; // (the name, here)
    IO[IO_ARG] = (uint32_t)strlen(name);
    IO[IO_BUF] = (uint32_t)(uintptr_t)data;
    IO[IO_LEN] = size;
    IO[IO_CMD] = CMD_FILE_WRITE;
    return IO[IO_RESULT] != 0;
}

// The port's settings: harness.py --config FILE.
bool Backend_LoadConfig(uint8_t *data, uint32_t size)
{
    IO[IO_BUF] = (uint32_t)(uintptr_t)data;
    IO[IO_LEN] = size;
    IO[IO_CMD] = CMD_CONFIG_LOAD;
    return IO[IO_RESULT] != 0;
}

void Backend_StoreConfig(const uint8_t *data, uint32_t size)
{
    IO[IO_BUF] = (uint32_t)(uintptr_t)data;
    IO[IO_LEN] = size;
    IO[IO_CMD] = CMD_CONFIG_STORE;
}

void Backend_StoreSave(const uint8_t *data, uint32_t size)
{
    IO[IO_BUF] = (uint32_t)(uintptr_t)data;
    IO[IO_LEN] = size;
    IO[IO_CMD] = CMD_SAVE_STORE;
}

// Frame capture for renderer regression tests: the registers at the start of
// every visible line, plus VRAM, palette and OAM at line 0.
static struct {
    uint8_t regs[GBA_H][0x400];
    uint8_t vram[0x18000];
    uint8_t pltt[0x400];
    uint8_t oam[0x400];
} sCapture;
static int sCapturing;

static void CaptureLine(int line)
{
    if (line == 0) {
        sCapturing = IO[IO_CAPTURE] != 0;
        if (sCapturing) {
            memcpy(sCapture.vram, gPortVram, sizeof(sCapture.vram));
            memcpy(sCapture.pltt, gPortPltt, sizeof(sCapture.pltt));
            memcpy(sCapture.oam, gPortOam, sizeof(sCapture.oam));
        }
    }
    if (!sCapturing)
        return;
    memcpy(sCapture.regs[line], gPortRegs, 0x400);
    if (line == GBA_H - 1) {
        IO[IO_CAPTURE] = (uint32_t)(uintptr_t)&sCapture;
        sCapturing = 0;
    }
}

void HarnessMain(void)
{
    Port_Init();
    gPortLineHook = CaptureLine;
    if (IO[IO_DUMPMAPS]) {
        static uint16_t map[512 * 512];
        void Port_DebugRenderFullMap(int set, uint16_t *out);
        for (int set = 0; set < 2; set++) {
            Port_DebugRenderFullMap(set, map);
            IO[IO_DUMPMAPS] = (uint32_t)(uintptr_t)map;
        }
        void Port_DebugRenderMapSprites(uint16_t *out);
        static uint16_t sprites[SUBSCREEN_W_MAX * SUBSCREEN_H_MAX];
        Port_DebugRenderMapSprites(sprites);
        IO[IO_SUBSCREEN] = (uint32_t)(uintptr_t)sprites;
    }
    Backend_Log("pmd-red-3ds harness: entering AgbMain");
    AgbMain();
    Backend_Fatal("AgbMain returned");
}

// Minimal newlib support: a heap for anything vsnprintf needs.
extern char __heap_start[], __heap_end[];
void *_sbrk(int incr)
{
    static char *brk;
    if (!brk) brk = __heap_start;
    if (brk + incr > __heap_end) return (void *)-1;
    char *old = brk;
    brk += incr;
    return old;
}
