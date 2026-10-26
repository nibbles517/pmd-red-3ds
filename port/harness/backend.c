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
};
enum { CMD_SAVE_STORE = 1, CMD_SAVE_LOAD = 2 };

const int Backend_Version5_UpdateRepoIfMissing = 5;

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

void Backend_Present(const uint16_t *frame)
{
    static uint16_t sub[SUBSCREEN_W * SUBSCREEN_H];
    if (IO[IO_SUBSCREEN])
        IO[IO_SUBSCREEN] = Port_RenderSubScreen(sub) ? (uint32_t)(uintptr_t)sub : 0;
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

bool Backend_LoadConfig(uint8_t *data, uint32_t size) { (void)data; (void)size; return false; }
void Backend_StoreConfig(const uint8_t *data, uint32_t size) { (void)data; (void)size; }

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
        static uint16_t sprites[SUBSCREEN_W * SUBSCREEN_H];
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
