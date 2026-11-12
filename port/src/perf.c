// Frame timing: how long each part of a frame takes, logged every two
// seconds so one run on a console shows where the time goes.
#include "port.h"

static u32 sAcc[PERF_N], sMax[PERF_N];
static u32 sFrames, sLast, sWindowStart, sWorst;
static const char *const kName[PERF_N] = { "ppu", "blit", "vsync", "sound", "gpu", "gc" };

void PerfAdd(int section, u32 us)
{
    sAcc[section] += us;
    if (us > sMax[section]) sMax[section] = us;
}

void PerfFrameEnd(void)
{
    u32 now = PlatTicksUs();
    if (!sLast) { sLast = sWindowStart = now; return; }
    u32 dt = now - sLast;
    sLast = now;
    if (dt > sWorst) sWorst = dt;
    if (++sFrames < PERF_WINDOW) return;

    unsigned st0, st1, st2, st3, st4, st5;
    PpuStats(&st0, &st1, &st2, &st3, &st4, &st5);
    unsigned lm0, lm1, lm2;
    PpuLampStats(&lm0, &lm1, &lm2);
    u32 span = now - sWindowStart;
    // "game" is everything else: the game's own logic, physics and drawing code.
    u32 other = span;
    for (int i = 0; i < PERF_N; i++) other -= sAcc[i];
    PlatLog("perf: %u.%u fps  frame %u.%ums (worst %u.%u)  game %u.%ums  ppu %u.%u  blit %u.%u  sound %u.%u  idle %u.%u  gpu %u.%u  gc %u.%u  view %d 3d %d seam %d  slow %u rebuilt %u bld %04x disp %04x  core1 %uus wait %uus  lamps %u/%u/%u",
            (unsigned)(sFrames * 1000000 / span), (unsigned)(sFrames * 10000000 / span % 10),
            (unsigned)(span / sFrames / 1000), (unsigned)(span / sFrames / 100 % 10),
            (unsigned)(sWorst / 1000), (unsigned)(sWorst / 100 % 10),
            (unsigned)(other / sFrames / 1000), (unsigned)(other / sFrames / 100 % 10),
            (unsigned)(sAcc[PERF_PPU] / sFrames / 1000), (unsigned)(sAcc[PERF_PPU] / sFrames / 100 % 10),
            (unsigned)(sAcc[PERF_BLIT] / sFrames / 1000), (unsigned)(sAcc[PERF_BLIT] / sFrames / 100 % 10),
            (unsigned)(sAcc[PERF_SOUND] / sFrames / 1000), (unsigned)(sAcc[PERF_SOUND] / sFrames / 100 % 10),
            (unsigned)(sAcc[PERF_VSYNC] / sFrames / 1000), (unsigned)(sAcc[PERF_VSYNC] / sFrames / 100 % 10),
            (unsigned)(sAcc[PERF_GPU] / sFrames / 1000), (unsigned)(sAcc[PERF_GPU] / sFrames / 100 % 10),
            (unsigned)(sAcc[PERF_GPUC] / sFrames / 1000), (unsigned)(sAcc[PERF_GPUC] / sFrames / 100 % 10),
            gPortView, PlatDepth() > 0, gPortSeam, st0, st1, st2, st3, st4, st5, lm0, lm1, lm2);
    (void)kName; (void)sMax;
    for (int i = 0; i < PERF_N; i++) sAcc[i] = sMax[i] = 0;
    sFrames = 0; sWorst = 0; sWindowStart = now;
}
