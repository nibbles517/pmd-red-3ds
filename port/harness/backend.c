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
};
enum { CMD_SAVE_STORE = 1, CMD_SAVE_LOAD = 2 };

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

void Backend_Present(const uint16_t *frame) { IO[IO_PRESENT] = (uint32_t)(uintptr_t)frame; }
uint16_t Backend_PollKeys(void) { return (uint16_t)IO[IO_KEYS]; }
void Backend_EndFrame(void) { IO[IO_ENDFRAME] = (uint32_t)Port_FrameCount(); }

void Backend_QueueAudio(const int8_t *left, const int8_t *right, int count, int rate)
{
    IO[IO_AUDIO_L] = (uint32_t)(uintptr_t)left;
    IO[IO_AUDIO_R] = (uint32_t)(uintptr_t)right;
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

void Backend_StoreSave(const uint8_t *data, uint32_t size)
{
    IO[IO_BUF] = (uint32_t)(uintptr_t)data;
    IO[IO_LEN] = size;
    IO[IO_CMD] = CMD_SAVE_STORE;
}

void HarnessMain(void)
{
    Port_Init();
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
