// Internal interface of the portable platform core.
//
// The core is plain C with no 3DS or host dependencies. A backend (3DS, or the
// test harness) supplies the Backend_* functions declared at the bottom.

#ifndef PORT_H
#define PORT_H

#include <stdint.h>
#include <stdbool.h>

#define GBA_W 240
#define GBA_H 160

// Shadow GBA memory (also declared, as plain arrays, in pmd-red's defines.h).
extern unsigned char gPortRegs[0x400];
extern unsigned char gPortVram[0x18000];
extern unsigned char gPortPltt[0x400];
extern unsigned char gPortOam[0x400];

// GBA key bits, active high (A=bit0 ... L=bit9).
enum {
    KEY_BIT_A = 1 << 0, KEY_BIT_B = 1 << 1, KEY_BIT_SELECT = 1 << 2, KEY_BIT_START = 1 << 3,
    KEY_BIT_RIGHT = 1 << 4, KEY_BIT_LEFT = 1 << 5, KEY_BIT_UP = 1 << 6, KEY_BIT_DOWN = 1 << 7,
    KEY_BIT_R = 1 << 8, KEY_BIT_L = 1 << 9,
};

// Called before each visible scanline is rendered, if set (test tooling).
extern void (*gPortLineHook)(int line);

// ppu.c
void Ppu_BeginFrame(void);
void Ppu_RenderLine(int line, uint16_t *dst);

// dma.c
void Dma_HBlank(void);
void Dma_VBlank(void);
void Dma_Reset(void);

// frame.c
void Port_Init(void);
uint64_t Port_FrameCount(void);
const uint16_t *Port_Framebuffer(void); // 240x160 BGR555, last completed frame

// sound.c
void Sound_EndFrame(void);

// Port settings kept in the game's own options (port/core/options.c).
bool Port_WantsBottomScreen(void); // Others > Game Options > Main Screen

// Backend: implemented once per platform.
void Backend_Present(const uint16_t *frame);   // 240x160 BGR555
uint16_t Backend_PollKeys(void);               // active-high GBA key bits
void Backend_EndFrame(void);                   // pacing; may also service the OS
void Backend_Log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void Backend_Fatal(const char *msg) __attribute__((noreturn));
// Renamed whenever a backend must change together with the core: linking an
// older backend (port/3ds/main.c from an old copy of the repo) with a newer
// pmd-red-game.o then fails with an undefined reference naming this symbol.
extern const int Backend_Version3_UpdateRepoIfMissing;
// One frame of audio: `count` interleaved 16-bit stereo samples at `rate` Hz.
void Backend_QueueAudio(const int16_t *stereo, int count, int rate);
// 128 KiB flash save image.
bool Backend_LoadSave(uint8_t *data, uint32_t size);
void Backend_StoreSave(const uint8_t *data, uint32_t size);

#endif
