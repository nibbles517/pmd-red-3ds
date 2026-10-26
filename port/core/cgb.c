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
    LatchRegisters(rate);

    uint8_t nr50 = Reg(NR50), nr51 = Reg(NR51);
    int volL = ((nr50 >> 4) & 7) + 1, volR = (nr50 & 7) + 1;
    uint16_t soundcntH = (uint16_t)(gPortRegs[0x82] | (gPortRegs[0x83] << 8));
    static const int kRatio[4] = { 1, 2, 4, 4 }; // 25%, 50%, 100% (3 is invalid)
    int ratio = kRatio[soundcntH & 3];
    uint8_t duty1 = sDuty[Reg(NR11) >> 6], duty2 = sDuty[Reg(NR21) >> 6];
    int waveShift = (Reg(NR32) & 0x80) ? -1 : (int[]){ 8, 0, 1, 2 }[(Reg(NR32) >> 5) & 3];
    const uint8_t *wave = &gPortRegs[WAVE_RAM];
    uint32_t seqInc = PhaseInc(512.0, rate);

    for (int i = 0; i < n; i++) {
        uint32_t prev = sFrameSeq;
        sFrameSeq += seqInc;
        if (sFrameSeq < prev)
            SequencerStep(rate);

        int out[4] = { 0, 0, 0, 0 };
        Channel *c = &sCh[0];
        if (c->on) { c->phase += c->inc; out[0] = ((duty1 >> (c->phase >> 29)) & 1) ? c->volume : -c->volume; }
        c = &sCh[1];
        if (c->on) { c->phase += c->inc; out[1] = ((duty2 >> (c->phase >> 29)) & 1) ? c->volume : -c->volume; }
        c = &sCh[2];
        if (c->on) {
            c->phase += c->inc;
            unsigned pos = c->phase >> 27; // 0-31
            unsigned s = (wave[pos >> 1] >> ((pos & 1) ? 0 : 4)) & 0xF;
            int v = (int)s * 2 - 15; // centre the 4-bit sample
            if (waveShift < 0) v = v * 3 / 4;
            else if (waveShift == 8) v = 0;
            else v >>= waveShift;
            out[2] = v;
        }
        c = &sCh[3];
        if (c->on) {
            c->phase += c->inc;
            while (c->phase >= 0x10000) {
                c->phase -= 0x10000;
                unsigned bit = (c->lfsr ^ (c->lfsr >> 1)) & 1;
                c->lfsr = (uint16_t)((c->lfsr >> 1) | (bit << 14));
                if (c->width7) c->lfsr = (uint16_t)((c->lfsr & ~0x40) | (bit << 6));
            }
            out[3] = (c->lfsr & 1) ? -c->volume : c->volume;
        }

        int l = 0, r = 0;
        for (int ch = 0; ch < 4; ch++) {
            if (nr51 & (0x10 << ch)) l += out[ch];
            if (nr51 & (0x01 << ch)) r += out[ch];
        }
        // NR50 volume (1-8) and the PSG share of the mix (25/50/100%).
        left[i] += l * volL * ratio / 4;
        right[i] += r * volR * ratio / 4;
    }
}
