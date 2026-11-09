// Emulation of the GBA's four Game Boy sound channels (two square waves with
// envelope, one with sweep; a 4-bit wave channel; noise).
//
// pmd-red's m4a driver drives them by writing sound registers (CgbSound in
// m4a.c), which in the port land in shadow memory. The driver runs once per
// frame, so the registers are sampled once per frame: a note start ("trigger",
// bit 7 of NRx4) is consumed and cleared, and the channel state then evolves
// at the hardware's rates (length 256 Hz, sweep 128 Hz, envelope 64 Hz) while
// the frame's samples are generated.

#include <string.h>
#include "port.h"

enum {
    NR10 = 0x60, NR11 = 0x62, NR12 = 0x63, NR13 = 0x64, NR14 = 0x65,
    NR21 = 0x68, NR22 = 0x69, NR23 = 0x6C, NR24 = 0x6D,
    NR30 = 0x70, NR31 = 0x72, NR32 = 0x73, NR33 = 0x74, NR34 = 0x75,
    NR41 = 0x78, NR42 = 0x79, NR43 = 0x7C, NR44 = 0x7D,
    NR50 = 0x80, NR51 = 0x81, SOUNDCNT_X = 0x84, WAVE_RAM = 0x90,
};

typedef struct {
    int on;
    uint32_t phase, inc;   // oscillator phase (32-bit fixed point of one period)
    int volume;            // 0-15
    int envDir, envPeriod, envTimer;
    int lengthEnabled, length;
    // channel 1 sweep
    int sweepPeriod, sweepDir, sweepShift, sweepTimer, shadowFreq;
    // noise
    uint16_t lfsr;
    int width7;
} Channel;

static Channel sCh[4];
static uint32_t sFrameSeq; // 512 Hz frame-sequencer phase, in 1/rate units
static int sSeqStep;

static inline uint8_t Reg(unsigned off) { return gPortRegs[off]; }

static uint32_t PhaseInc(double hz, int rate)
{
    double inc = hz * 4294967296.0 / rate;
    return inc >= 4294967295.0 ? 0xFFFFFFFFu : (uint32_t)inc;
}

static void SetSquareFreq(Channel *c, int freq, int rate)
{
    c->inc = PhaseInc(131072.0 / (2048 - (freq & 0x7FF)), rate);
}

static void SetWaveFreq(Channel *c, int freq, int rate)
{
    // 32 samples per period: phase covers the whole 32-sample table.
    c->inc = PhaseInc(65536.0 / (2048 - (freq & 0x7FF)), rate);
}

static void SetNoiseFreq(Channel *c, uint8_t nr43, int rate)
{
    int r = nr43 & 7, s = nr43 >> 4;
    double hz = 524288.0 / (r ? r : 0.5) / (double)(1 << (s + 1));
    // `inc` counts LFSR clocks per sample in 16.16 fixed point.
    double clocks = hz / rate;
    c->inc = clocks * 65536.0 > 4294967295.0 ? 0xFFFFFFFFu : (uint32_t)(clocks * 65536.0);
    c->width7 = (nr43 >> 3) & 1;
}

static void Envelope(Channel *c, uint8_t nrx2)
{
    c->volume = nrx2 >> 4;
    c->envDir = (nrx2 >> 3) & 1;
    c->envPeriod = nrx2 & 7;
    c->envTimer = c->envPeriod;
}

// Consumes a pending trigger (bit 7 of NRx4) and refreshes frequencies.
static void LatchRegisters(int rate)
{
    // Channel 1: square with sweep.
    Channel *c = &sCh[0];
    int f1 = Reg(NR13) | ((Reg(NR14) & 7) << 8);
    if (Reg(NR14) & 0x80) {
        gPortRegs[NR14] &= 0x7F;
        c->on = (Reg(NR12) & 0xF8) != 0;
        Envelope(c, Reg(NR12));
        c->length = 64 - (Reg(NR11) & 0x3F);
        c->shadowFreq = f1;
        c->sweepPeriod = (Reg(NR10) >> 4) & 7;
        c->sweepDir = (Reg(NR10) >> 3) & 1;
        c->sweepShift = Reg(NR10) & 7;
        c->sweepTimer = c->sweepPeriod ? c->sweepPeriod : 8;
    }
    c->lengthEnabled = (Reg(NR14) >> 6) & 1;
    if (!c->sweepPeriod) c->shadowFreq = f1;
    SetSquareFreq(c, c->shadowFreq, rate);

    // Channel 2: square.
    c = &sCh[1];
    if (Reg(NR24) & 0x80) {
        gPortRegs[NR24] &= 0x7F;
        c->on = (Reg(NR22) & 0xF8) != 0;
        Envelope(c, Reg(NR22));
        c->length = 64 - (Reg(NR21) & 0x3F);
    }
    c->lengthEnabled = (Reg(NR24) >> 6) & 1;
    SetSquareFreq(c, Reg(NR23) | ((Reg(NR24) & 7) << 8), rate);

    // Channel 3: wave.
    c = &sCh[2];
    if (Reg(NR34) & 0x80) {
        gPortRegs[NR34] &= 0x7F;
        c->on = (Reg(NR30) & 0x80) != 0;
        c->length = 256 - Reg(NR31);
        c->phase = 0;
    }
    if (!(Reg(NR30) & 0x80)) c->on = 0; // DAC off
    c->lengthEnabled = (Reg(NR34) >> 6) & 1;
    SetWaveFreq(c, Reg(NR33) | ((Reg(NR34) & 7) << 8), rate);

    // Channel 4: noise.
    c = &sCh[3];
    if (Reg(NR44) & 0x80) {
        gPortRegs[NR44] &= 0x7F;
        c->on = (Reg(NR42) & 0xF8) != 0;
        Envelope(c, Reg(NR42));
        c->length = 64 - (Reg(NR41) & 0x3F);
        c->lfsr = 0x7FFF;
        c->phase = 0;
    }
    c->lengthEnabled = (Reg(NR44) >> 6) & 1;
    SetNoiseFreq(c, Reg(NR43), rate);
}

// 512 Hz frame sequencer: length at 256 Hz, sweep at 128 Hz, envelope at 64 Hz.
static void SequencerStep(int rate)
{
    int step = sSeqStep++ & 7;
    if ((step & 1) == 0) {
        for (int i = 0; i < 4; i++) {
            Channel *c = &sCh[i];
            if (c->lengthEnabled && c->length > 0 && --c->length == 0)
                c->on = 0;
        }
    }
    if (step == 2 || step == 6) {
        Channel *c = &sCh[0];
        if (c->sweepPeriod && --c->sweepTimer <= 0) {
            c->sweepTimer = c->sweepPeriod;
            int delta = c->shadowFreq >> c->sweepShift;
            int nf = c->sweepDir ? c->shadowFreq - delta : c->shadowFreq + delta;
            if (nf > 2047) c->on = 0;
            else if (c->sweepShift) { c->shadowFreq = nf; SetSquareFreq(c, nf, rate); }
        }
    }
    if (step == 7) {
        for (int i = 0; i < 4; i++) {
            if (i == 2) continue;
            Channel *c = &sCh[i];
            if (c->envPeriod && --c->envTimer <= 0) {
                c->envTimer = c->envPeriod;
                if (c->envDir && c->volume < 15) c->volume++;
                else if (!c->envDir && c->volume > 0) c->volume--;
            }
        }
    }
}

static const uint8_t sDuty[4] = { 0x01, 0x81, 0x87, 0x7E }; // 12.5%, 25%, 50%, 75%

// The noise LFSR, one clock.
static inline uint16_t LfsrStep(uint16_t l, int width7)
{
    unsigned bit = (l ^ (l >> 1)) & 1;
    l = (uint16_t)((l >> 1) | (bit << 14));
    if (width7) l = (uint16_t)((l & ~0x40) | (bit << 6));
    return l;
}

// At high noise pitches the LFSR clocks dozens of times a sample. Its states
// run in a fixed cycle, so a sample advances a position in a table instead:
// 15-bit mode, all 32767 non-zero states; 7-bit mode, 127 states (once the
// register has had 15 clocks in that mode, all its bits come from the cycle).
#define LFSR15 32767
#define LFSR7 127
static uint16_t sSeq15[LFSR15], sPos15[0x8000];
static uint16_t sSeq7[LFSR7];
static uint8_t sPos7[0x8000]; // 0xFF: not in the 7-bit cycle (yet)
static int sLfsrReady;

static void MakeLfsrTables(void)
{
    uint16_t l = 0x7FFF;
    for (int i = 0; i < LFSR15; i++) {
        sSeq15[i] = l;
        sPos15[l] = (uint16_t)i;
        l = LfsrStep(l, 0);
    }
    memset(sPos7, 0xFF, sizeof(sPos7));
    for (int i = 0; i < 64; i++)
        l = LfsrStep(l, 1);
    for (int i = 0; i < LFSR7; i++) {
        sSeq7[i] = l;
        sPos7[l] = (uint8_t)i;
        l = LfsrStep(l, 1);
    }
    sLfsrReady = 1;
}

// Adds the four channels into left/right (GBA-scale units, see sound.c).
void Cgb_Mix(int32_t *left, int32_t *right, int n, int rate)
{
#ifdef CGB_MUTE_FOR_TEST
    return;
#endif
    if (!(Reg(SOUNDCNT_X) & 0x80)) {
        memset(sCh, 0, sizeof(sCh));
        return;
    }
    if (!sLfsrReady)
        MakeLfsrTables();
    LatchRegisters(rate);

    uint8_t nr50 = Reg(NR50), nr51 = Reg(NR51);
    int volL = ((nr50 >> 4) & 7) + 1, volR = (nr50 & 7) + 1;
    uint16_t soundcntH = (uint16_t)(gPortRegs[0x82] | (gPortRegs[0x83] << 8));
    static const int kRatio[4] = { 1, 2, 4, 4 }; // 25%, 50%, 100% (3 is invalid)
    int ratio = kRatio[soundcntH & 3];
    int mulL = volL * ratio, mulR = volR * ratio;
    uint8_t duty1 = sDuty[Reg(NR11) >> 6], duty2 = sDuty[Reg(NR21) >> 6];
    int waveShift = (Reg(NR32) & 0x80) ? -1 : (int[]){ 8, 0, 1, 2 }[(Reg(NR32) >> 5) & 3];
    const uint8_t *wave = &gPortRegs[WAVE_RAM];
    uint32_t seqInc = PhaseInc(512.0, rate);
    // NR51: which channels go left/right (as masks to add with)
    int mL[4], mR[4];
    for (int ch = 0; ch < 4; ch++) {
        mL[ch] = (nr51 & (0x10 << ch)) ? -1 : 0;
        mR[ch] = (nr51 & (0x01 << ch)) ? -1 : 0;
    }

    // Nothing playing (as with DS music): only the frame sequencer moves on.
    if (!sCh[0].on && !sCh[1].on && !sCh[2].on && !sCh[3].on) {
        for (int i = 0; i < n; i++) {
            uint32_t prev = sFrameSeq;
            sFrameSeq += seqInc;
            if (sFrameSeq < prev)
                SequencerStep(rate);
        }
        return;
    }

    // The noise channel's position in its LFSR cycle (-1: stepped one clock at a time)
    Channel *nc = &sCh[3];
    const uint16_t *nSeq = nc->width7 ? sSeq7 : sSeq15;
    int nPeriod = nc->width7 ? LFSR7 : LFSR15;
    // (A register of 0 stays 0; in 7-bit mode one whose low 7 bits are 0 ends up there.)
    int nPos = !nc->on || nc->lfsr == 0 ? -1
               : nc->width7 ? (sPos7[nc->lfsr] == 0xFF ? -1 : sPos7[nc->lfsr]) : sPos15[nc->lfsr];

    for (int i = 0; i < n; i++) {
        uint32_t prev = sFrameSeq;
        sFrameSeq += seqInc;
        if (sFrameSeq < prev)
            SequencerStep(rate);

        int l = 0, r = 0, o;
        Channel *c = &sCh[0];
        if (c->on) {
            c->phase += c->inc;
            o = ((duty1 >> (c->phase >> 29)) & 1) ? c->volume : -c->volume;
            l += o & mL[0]; r += o & mR[0];
        }
        c = &sCh[1];
        if (c->on) {
            c->phase += c->inc;
            o = ((duty2 >> (c->phase >> 29)) & 1) ? c->volume : -c->volume;
            l += o & mL[1]; r += o & mR[1];
        }
        c = &sCh[2];
        if (c->on) {
            c->phase += c->inc;
            unsigned pos = c->phase >> 27; // 0-31
            unsigned s = (wave[pos >> 1] >> ((pos & 1) ? 0 : 4)) & 0xF;
            int v = (int)s * 2 - 15; // centre the 4-bit sample
            if (waveShift < 0) v = v * 3 / 4;
            else if (waveShift == 8) v = 0;
            else v >>= waveShift;
            l += v & mL[2]; r += v & mR[2];
        }
        if (nc->on) {
            nc->phase += nc->inc;
            unsigned clocks = nc->phase >> 16;
            nc->phase &= 0xFFFF;
            if (nPos >= 0) {
                nPos += (int)clocks;
                while (nPos >= nPeriod)
                    nPos -= nPeriod;
                o = (nSeq[nPos] & 1) ? -nc->volume : nc->volume;
            }
            else {
                // (7-bit mode, just switched to: until the register is in the cycle)
                while (clocks-- && nc->lfsr != 0) {
                    nc->lfsr = LfsrStep(nc->lfsr, 1);
                    if (sPos7[nc->lfsr] != 0xFF) {
                        nPos = sPos7[nc->lfsr] + (int)clocks;
                        while (nPos >= nPeriod)
                            nPos -= nPeriod;
                        nc->lfsr = nSeq[nPos];
                        break;
                    }
                }
                o = (nc->lfsr & 1) ? -nc->volume : nc->volume;
            }
            l += o & mL[3]; r += o & mR[3];
        }

        // NR50 volume (1-8) and the PSG share of the mix (25/50/100%).
        left[i] += l * mulL / 4;
        right[i] += r * mulR / 4;
    }
    if (nPos >= 0)
        nc->lfsr = nSeq[nPos];
}
