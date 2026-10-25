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

void Sound_EndFrame(void)
{
    struct SoundInfo *info = SOUND_INFO_PTR;
    int spv, chunk, counter;

    if (info == NULL || info->ident != ID_NUMBER)
        return;
    spv = info->pcmSamplesPerVBlank;
    counter = info->pcmDmaCounter;
    if (spv <= 0 || info->pcmDmaPeriod == 0 || counter == sLastCounter)
        return; // the mixer did not run this frame
    sLastCounter = (u8)counter;

    // m4aSoundVSync counts pcmDmaCounter down from pcmDmaPeriod to 1; SoundMain
    // then mixes into chunk (period - (counter - 1)), or chunk 0 when counter is 1.
    chunk = (counter > 1) ? info->pcmDmaPeriod - (counter - 1) : 0;
    if (chunk < 0 || chunk >= info->pcmDmaPeriod)
        return;

    Backend_QueueAudio(&info->pcmBuffer[chunk * spv],
                       &info->pcmBuffer[PCM_DMA_BUF_SIZE + chunk * spv],
                       spv, info->pcmFreq);
}
