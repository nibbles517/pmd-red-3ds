// Internal interface of the portable platform core.
//
// The core is plain C with no 3DS or host dependencies. A backend (3DS, or the
// test harness) supplies the Backend_* functions declared at the bottom.

#ifndef PORT_H
#define PORT_H

#include <stdint.h>
#include <stdbool.h>
#include "port_view.h"

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
void Ppu_SetView(int wide); // wide: SCREEN_W-pixel lines, lines past 159 too (port_view.h)
// Dual Core: the lines are drawn after the frame's events, from the
// registers each line had (NULL: the live ones); two renderers share them
// out, the second one (PpuB_*, ppu_b.c) on the second CPU core. Ppu_SkipLine:
// a line the other renderer draws.
void Ppu_SetLineRegs(const uint8_t *regs);
void Ppu_SkipLine(int line);
void PpuB_BeginFrame(void);
void PpuB_RenderLine(int line, uint16_t *dst);
void PpuB_SkipLine(int line);
void PpuB_SetView(int wide);
void PpuB_SetLineRegs(const uint8_t *regs);

// dma.c
void Dma_HBlank(void);
void Dma_VBlank(void);
void Dma_Reset(void);

// frame.c
void Port_Init(void);
uint64_t Port_FrameCount(void);
const uint16_t *Port_Framebuffer(void); // last completed frame, as passed to Backend_Present

// sound.c
void Sound_EndFrame(void);

// Port settings kept in the game's own options (port/core/options.c).
bool Port_WantsBottomScreen(void); // Others > Screen/Layout Options > Main Screen
bool Port_WideSize(void);          // ... > Main Screen Settings > Size is DS or Fit (port_view.h)
void Port_LoadConfig(void);
int Port_FrameSkipMode(void);
int Port_DualCore(void);            // Graphics Options > Dual Core: On       // Performance Settings > Frame Skip: 0 Off, 1 Auto, 2 Always

// subscreen.c: the second screen, DS-style. Fills out (BGR555) and returns
// true when there is something to show.
#define SUBSCREEN_W_MAX 320 // the second screen's picture: 256x192, or 320x240 (Fit)
#define SUBSCREEN_H_MAX 240
// The caller keeps `out` between calls: when nothing changed since the last
// call, out is left as it was and Port_SubScreenChanged() returns false.
// `out` holds SUBSCREEN_W_MAX x SUBSCREEN_H_MAX pixels; the picture is
// Port_SubScreenSize() (rows that wide) after the call. When the size
// changes, the whole picture is drawn again.
bool Port_RenderSubScreen(uint16_t *out);
void Port_SubScreenSize(int *w, int *h);
bool Port_SubScreenChanged(void);
void Port_SubScreenChangedArea(int *x0, int *y0, int *x1, int *y1); // what changed (x1/y1 exclusive)

// Backend: implemented once per platform.
// BGR555: 240x160 (GBA size), 256x192 (DS size) or 320x240 (Fit). frame is NULL
// on a frame skipped by Frame Skip: the main screen keeps its last picture
// (the second screen is still updated).
void Backend_Present(const uint16_t *frame, int width, int height);
// Whether the last frame took longer than a 60th of a second (Frame Skip:
// Auto then skips drawing the next one).
bool Backend_FrameLate(void);
// Dual Core: runs fn(arg) on the second CPU core (or at once, if there is
// none); Backend_WaitAsync waits for it to return.
void Backend_RunAsync(void (*fn)(void *), void *arg);
void Backend_WaitAsync(void);
bool Backend_AsyncDone(void);  // whether it has returned already (without waiting)
bool Backend_IsNew3DS(void); // (Dual Core's default)
// Performance meter: the core marks where each frame's parts end (the
// backend times them). skipped: Frame Skip left the frame undrawn.
enum { PORT_MARK_FRAME_START, PORT_MARK_DRAWN, PORT_MARK_PRESENTED, PORT_MARK_VBLANK_DONE };
void Backend_Mark(int mark, int skipped);
const char *Port_SceneName(void); // "dungeon", "ground" or "other" (options.c)
const char *Port_SettingsSummary(void); // e.g. "size=DS sub=Blue skip=Auto"
uint16_t Backend_PollKeys(void);               // active-high GBA key bits
void Backend_EndFrame(void);                   // pacing; may also service the OS
void Backend_Log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void Backend_Fatal(const char *msg) __attribute__((noreturn));
// Renamed whenever a backend must change together with the core: linking an
// older backend (port/3ds/main.c from an old copy of the repo) with a newer
// pmd-red-game.o then fails with an undefined reference naming this symbol.
extern const int Backend_Version11_UpdateRepoIfMissing;
// One frame of audio: `count` interleaved 16-bit stereo samples at `rate` Hz.
void Backend_QueueAudio(const int16_t *stereo, int count, int rate);
// 128 KiB flash save image.
bool Backend_LoadSave(uint8_t *data, uint32_t size);
void Backend_StoreSave(const uint8_t *data, uint32_t size);
// The port's own settings (options.c), a few bytes kept apart from the save.
bool Backend_LoadConfig(uint8_t *data, uint32_t size);
void Backend_StoreConfig(const uint8_t *data, uint32_t size);

#endif
