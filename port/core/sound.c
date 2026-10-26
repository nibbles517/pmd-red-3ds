// Glue between pmd-red's m4a sound driver and the backend's audio output.
//
// Compiled with pmd-red's include path, since it reads the driver's SoundInfo.
//
// On the GBA, the mixer writes one frame's worth of samples per VBlank into a
// ring buffer (pcmBuffer) that DMA streams to the sound FIFOs. Here, after each
// frame, the chunk the mixer just produced is handed to the backend instead.

#include "global.h"
#include "gba/m4a_internal.h"
#include "../core/port.h"

// On hardware the BIOS call SoundGetJumpList (swi 0x2A) fills the sound
// driver's command table. pmd-red's own m4a_1.s carries the same table and a
// copy routine (MPlayJumpTableCopy), so the port uses that instead.
void Port_SoundGetJumpList(void *table)
{
    MPlayJumpTableCopy((MPlayFunc *)table);
}

static u8 sLastCounter;

void Cgb_Mix(s32 *left, s32 *right, int n, int rate);

void Sound_EndFrame(void)
{
    static s32 left[1024], right[1024];
    static s16 out[1024 * 2];
    struct SoundInfo *info = SOUND_INFO_PTR;
    int spv, chunk, counter, i;
    u16 cnt;
    const s8 *a, *b;

    if (info == NULL || info->ident != ID_NUMBER)
        return;
    spv = info->pcmSamplesPerVBlank;
    counter = info->pcmDmaCounter;
    if (spv <= 0 || spv > 1024 || info->pcmDmaPeriod == 0 || counter == sLastCounter)
        return; // the mixer did not run this frame
    sLastCounter = (u8)counter;

    // m4aSoundVSync counts pcmDmaCounter down from pcmDmaPeriod to 1; SoundMain
    // then mixes into chunk (period - (counter - 1)), or chunk 0 when counter is 1.
    chunk = (counter > 1) ? info->pcmDmaPeriod - (counter - 1) : 0;
    if (chunk < 0 || chunk >= info->pcmDmaPeriod)
        return;

    // Direct Sound: FIFO A gets the first half of pcmBuffer, FIFO B the second
    // (SoundInit). SOUNDCNT_H routes each to left/right at 50% or 100%.
    a = &info->pcmBuffer[chunk * spv];
    b = &info->pcmBuffer[PCM_DMA_BUF_SIZE + chunk * spv];
    cnt = REG_SOUNDCNT_H;
    {
        int volA = (cnt & 0x0004) ? 4 : 2, volB = (cnt & 0x0008) ? 4 : 2;
        int aR = (cnt >> 8) & 1, aL = (cnt >> 9) & 1, bR = (cnt >> 12) & 1, bL = (cnt >> 13) & 1;
        for (i = 0; i < spv; i++) {
            left[i] = (aL ? a[i] * volA : 0) + (bL ? b[i] * volB : 0);
            right[i] = (aR ? a[i] * volA : 0) + (bR ? b[i] * volB : 0);
        }
    }

    // Game Boy channels (square, wave, noise).
    Cgb_Mix(left, right, spv, info->pcmFreq);

    // The GBA's mix spans about +-512; scale to 16 bits.
    for (i = 0; i < spv; i++) {
        s32 l = left[i], r = right[i];
        if (l > 511) l = 511; else if (l < -512) l = -512;
        if (r > 511) r = 511; else if (r < -512) r = -512;
        out[i * 2] = (s16)(l * 64);
        out[i * 2 + 1] = (s16)(r * 64);
    }
    Backend_QueueAudio(out, spv, info->pcmFreq);
}
