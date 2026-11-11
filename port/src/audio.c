// Audio output for the port.
//
// The game's own sound engine (sequencer and PCM mixer) is assembled from the
// user's decomp tree and runs unchanged; it leaves each frame's samples in
// its 8-bit buffer exactly as it did for the GBA's sound FIFOs. This file is
// the part that hardware used to do: pick those samples up, synthesise the
// four Game Boy tone channels from the sound registers, and hand the result
// to the platform.
#include <string.h>
#include "port.h"

#define OUT_SPF 672                 // 3x the engine's 224 samples per frame
#define OUT_RATE 40137
#define PCM_HALF 0x630              // right buffer, then left buffer

extern u8 *gGbaSoundInfoPtr;
extern const u32 gPortSongFixups[];

static s32 sMix[OUT_SPF * 2];
static s16 sOut[OUT_SPF * 2];
static u32 sPcmOffset;
static s32 sPrevR, sPrevL;

// ---- Game Boy tone channels -------------------------------------------
// The engine drives these through the sound registers once per frame, so
// the synth simply reads the registers back after it has run.
static struct {
    u32 phase;          // 16.16 position within the waveform period
    u32 lfsr;
    s32 length;         // remaining samples when length counting, else -1
    u8 on;
    u32 sweepClock, sweepFreq;
} sPsg[4];

static void PsgFrame(void)
{
    u8 *io = gGbaIo;
    u16 cntX = *(u16 *)(io + 0x84);
    if (!(cntX & 0x80)) return;
    u8 nr50 = io[0x80], nr51 = io[0x81];
    int ratio = *(u16 *)(io + 0x82) & 3;        // 0: 25%, 1: 50%, 2: 100%
    s32 masterR = (nr50 & 7) + 1, masterL = ((nr50 >> 4) & 7) + 1;

    static const u8 duty[4] = { 0x80, 0x81, 0xE1, 0x7E };

    for (int c = 0; c < 4; c++) {
        int base = c == 0 ? 0x60 : c == 1 ? 0x66 : c == 2 ? 0x70 : 0x76;
        u8 r1 = c == 0 ? io[0x62] : c == 1 ? io[0x68] : c == 2 ? io[0x72] : io[0x78];
        u8 r2 = c == 0 ? io[0x63] : c == 1 ? io[0x69] : c == 2 ? io[0x73] : io[0x79];
        u8 r3 = c == 0 ? io[0x64] : c == 1 ? io[0x6C] : c == 2 ? io[0x74] : io[0x7C];
        u8 *r4 = c == 0 ? &io[0x65] : c == 1 ? &io[0x6D] : c == 2 ? &io[0x75] : &io[0x7D];
        (void)base;

        if (*r4 & 0x80) {                        // trigger
            *r4 &= 0x7F;
            sPsg[c].on = 1;
            sPsg[c].phase = 0;
            sPsg[c].lfsr = 0x7FFF;
            sPsg[c].sweepClock = 0;
            sPsg[c].sweepFreq = r3 | ((*r4 & 7) << 8);
            if (*r4 & 0x40) {
                int len = c == 2 ? 256 - r1 : 64 - (r1 & 0x3F);
                sPsg[c].length = len * OUT_RATE / 256;
            } else sPsg[c].length = -1;
        }
        if (!sPsg[c].on) continue;

        s32 vol;                                 // 0..15 scale
        if (c == 2) {
            if (!(io[0x70] & 0x80)) continue;
            static const u8 wv[4] = { 0, 4, 2, 1 };
            vol = (r2 & 0x80) ? 3 : wv[(r2 >> 5) & 3];   // quarters
        } else {
            vol = r2 >> 4;
        }
        if (!vol) continue;

        u32 freq = r3 | ((*r4 & 7) << 8);
        u32 step;                                // waveform steps per sample, 16.16
        if (c < 2) step = (u32)(((unsigned long long)(131072 * 8) << 16) / ((2048 - freq) * OUT_RATE));
        else if (c == 2) step = (u32)(((unsigned long long)2097152 << 16) / ((2048 - freq) * OUT_RATE));
        else {
            int r = r3 & 7, s = r3 >> 4;
            u32 hz = r ? (524288 / r) >> (s + 1) : (524288 * 2) >> (s + 1);
            step = (u32)(((unsigned long long)hz << 16) / OUT_RATE);
        }

        int panR = nr51 & (1 << c), panL = nr51 & (0x10 << c);
        // One channel at full volume spans a quarter of a direct-sound voice.
        s32 gainR = panR ? (masterR * 8192 / 8) >> (2 - ratio) : 0;
        s32 gainL = panL ? (masterL * 8192 / 8) >> (2 - ratio) : 0;
        const u8 *wave = io + 0x90;
        u8 d = duty[r1 >> 6];
        s32 *out = sMix;

        for (int i = 0; i < OUT_SPF; i++) {
            s32 s;
            if (sPsg[c].length >= 0 && sPsg[c].length-- == 0) { sPsg[c].on = 0; break; }
            if (c < 2) {
                s = ((d >> ((sPsg[c].phase >> 16) & 7)) & 1) ? vol : -vol;
                s = s * 16 / 15;
            } else if (c == 2) {
                u32 pos = (sPsg[c].phase >> 16) & 31;
                u8 b = wave[pos >> 1];
                s32 smp = (pos & 1) ? (b & 0xF) : (b >> 4);
                s = ((smp * 2 - 15) * vol * 16) / (15 * 4);
            } else {
                u32 n = sPsg[c].phase >> 16;
                sPsg[c].phase &= 0xFFFF;
                while (n--) {
                    u32 bit = (sPsg[c].lfsr ^ (sPsg[c].lfsr >> 1)) & 1;
                    sPsg[c].lfsr >>= 1;
                    if (r3 & 8) sPsg[c].lfsr = (sPsg[c].lfsr & ~0x40u) | (bit << 6);
                    else sPsg[c].lfsr = (sPsg[c].lfsr & ~0x4000u) | (bit << 14);
                }
                s = (sPsg[c].lfsr & 1) ? -vol : vol;
                s = s * 16 / 15;
            }
            sPsg[c].phase += step;
            out[0] += (s * gainL) >> 4;
            out[1] += (s * gainR) >> 4;
            out += 2;
        }
    }
}


// Jump targets inside song data are filled in here (see tools/prepare.py).
void PortAudioInit(void)
{
    for (const u32 *f = gPortSongFixups; f[0]; f += 2) memcpy((void *)f[0], &f[1], 4);
}

// Where in its buffer the engine is about to write this frame's samples.
static u32 sMixStart;

void PortAudioBeforeMix(void)
{
    sMixStart = PlatTicksUs();
    const u8 *si = gGbaSoundInfoPtr;
    sPcmOffset = 0;
    if (!si) return;
    u32 counter = si[4], period = si[0xB], spv = *(const u32 *)(si + 0x10);
    if (counter > 1) sPcmOffset = (period - (counter - 1)) * spv;
}

void PortAudioAfterMix(void)
{
    const u8 *si = gGbaSoundInfoPtr;
    u32 spv = si ? *(const u32 *)(si + 0x10) : 0;
    memset(sMix, 0, sizeof sMix);
    if (si && spv && spv <= OUT_SPF && sPcmOffset + spv <= PCM_HALF) {
        const s8 *r = (const s8 *)si + 0x350 + sPcmOffset, *l = r + PCM_HALF;
        // Linear upsample to the output rate.
        u32 pos = 0, step = (spv << 16) / OUT_SPF;
        for (int i = 0; i < OUT_SPF; i++, pos += step) {
            u32 k = pos >> 16; s32 fr = pos & 0xFFFF;
            s32 r0 = k ? r[k - 1] << 8 : sPrevR, r1 = r[k] << 8;
            s32 l0 = k ? l[k - 1] << 8 : sPrevL, l1 = l[k] << 8;
            sMix[i * 2] = l0 + (((l1 - l0) * fr) >> 16);
            sMix[i * 2 + 1] = r0 + (((r1 - r0) * fr) >> 16);
        }
        sPrevR = r[spv - 1] << 8; sPrevL = l[spv - 1] << 8;
    }
    PsgFrame();
    for (int i = 0; i < OUT_SPF * 2; i++) {
        s32 v = sMix[i];
        sOut[i] = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
    }
    PlatAudioQueue(sOut, OUT_SPF, OUT_RATE);
    PerfAdd(PERF_SOUND, PlatTicksUs() - sMixStart);
}

void PortAudioFrame(void) {}
