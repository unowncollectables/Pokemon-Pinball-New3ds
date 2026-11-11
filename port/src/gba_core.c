// GBA machine model: memory regions, BIOS calls, DMA, interrupts, frame loop.
#include <string.h>
#include <stdlib.h>
#include "port.h"

u8 gGbaPltt[0x400] __attribute__((aligned(16)));
u8 gGbaVram[0x18000] __attribute__((aligned(16)));
u8 gGbaOam[0x400] __attribute__((aligned(16)));
u8 gGbaIo[0x400] __attribute__((aligned(16)));
u8 gGbaSram[0x10000] __attribute__((aligned(16)));

struct SoundInfo;
struct SoundInfo *gGbaSoundInfoPtr;
u16 gGbaIntrCheck;
void *gGbaIntrVector;
u32 gPortFrame;

typedef void (*IntrFunc)(void);
extern IntrFunc gIntrTable[14];

enum {
    REG_DISPCNT = 0x00, REG_DISPSTAT = 0x04, REG_VCOUNT = 0x06,
    REG_KEYINPUT = 0x130, REG_IE = 0x200, REG_IF = 0x202, REG_IME = 0x208,
};
#define DISPSTAT_VBLANK 1
#define DISPSTAT_HBLANK 2
#define DISPSTAT_VCOUNT 4
#define DISPSTAT_VBLANK_INTR 8
#define DISPSTAT_VCOUNT_INTR 0x20
#define INTR_VBLANK 1
#define INTR_VCOUNT 4

// The data tables are linked against the port's arrays, so real GBA bus
// addresses should never show up. If one does anyway, map it instead of
// faulting: nothing of ours lives in 0x02000000-0x07FFFFFF.
void *GbaXlat(const void *p)
{
    uintptr_t a = (uintptr_t)p;
    switch (a >> 24) {
    case 2: return gGbaEwram + (a & 0x3FFFF);
    case 3: return gGbaIwram + (a & 0x7FFF);
    case 4: return gGbaIo + (a & 0x3FF);
    case 5: return gGbaPltt + (a & 0x3FF);
    case 6: return gGbaVram + (a & 0x1FFFF) % 0x18000;
    case 7: return gGbaOam + (a & 0x3FF);
    }
    return (void *)p;
}

// ---- BIOS ------------------------------------------------------------
void CpuSet(const void *src, void *dest, u32 control)
{
    u32 count = control & 0x1FFFFF;
    int fixed = (control >> 24) & 1;
    src = GbaXlat(src); dest = GbaXlat(dest);
    if (control & (1 << 26)) {
        const u32 *s = src; u32 *d = dest;
        if (fixed) { u32 v = *s; while (count--) *d++ = v; }
        else memmove(d, s, count * 4);
    } else {
        const u16 *s = src; u16 *d = dest;
        if (fixed) { u16 v = *s; while (count--) *d++ = v; }
        else memmove(d, s, count * 2);
    }
}

void CpuFastSet(const void *src, void *dest, u32 control)
{
    u32 count = ((control & 0x1FFFFF) + 7) & ~7u;
    const u32 *s = GbaXlat(src); u32 *d = GbaXlat(dest);
    if (control & (1 << 24)) { u32 v = *s; while (count--) *d++ = v; }
    else memmove(d, s, count * 4);
}

static void Lz77(const u8 *src, u8 *dst)
{
    u32 header = src[0] | (src[1] << 8) | (src[2] << 16) | ((u32)src[3] << 24);
    u32 remaining = header >> 8;
    src += 4;
    while (remaining) {
        u8 flags = *src++;
        for (int i = 0; i < 8 && remaining; i++, flags <<= 1) {
            if (flags & 0x80) {
                u32 len = (src[0] >> 4) + 3;
                u32 disp = (((src[0] & 0xF) << 8) | src[1]) + 1;
                src += 2;
                if (len > remaining) len = remaining;
                remaining -= len;
                while (len--) { *dst = *(dst - disp); dst++; }
            } else {
                *dst++ = *src++;
                remaining--;
            }
        }
    }
}
void LZ77UnCompWram(const void *src, void *dest) { Lz77(GbaXlat(src), GbaXlat(dest)); }
void LZ77UnCompVram(const void *src, void *dest) { Lz77(GbaXlat(src), GbaXlat(dest)); }

static void Rl(const u8 *src, u8 *dst)
{
    u32 remaining = (src[1] | (src[2] << 8) | (src[3] << 16));
    src += 4;
    while (remaining) {
        u8 f = *src++;
        u32 len;
        if (f & 0x80) {
            len = (f & 0x7F) + 3;
            if (len > remaining) len = remaining;
            memset(dst, *src++, len);
        } else {
            len = (f & 0x7F) + 1;
            if (len > remaining) len = remaining;
            memcpy(dst, src, len);
            src += len;
        }
        dst += len; remaining -= len;
    }
}
void RLUnCompWram(const void *src, void *dest) { Rl(GbaXlat(src), GbaXlat(dest)); }
void RLUnCompVram(const void *src, void *dest) { Rl(GbaXlat(src), GbaXlat(dest)); }

u16 Sqrt(u32 x)
{
    u32 r = 0, bit = 1u << 30;
    while (bit > x) bit >>= 2;
    while (bit) {
        if (x >= r + bit) { x -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return (u16)r;
}

// Same fixed-point polynomial the GBA BIOS uses: the ball physics feeds on
// these exact values, so this must not be "improved".
static s32 BiosArcTan(s32 i)
{
    s32 a = -((i * i) >> 14);
    s32 b = ((0xA9 * a) >> 14) + 0x390;
    b = ((b * a) >> 14) + 0x91C;
    b = ((b * a) >> 14) + 0xFB6;
    b = ((b * a) >> 14) + 0x16AA;
    b = ((b * a) >> 14) + 0x2081;
    b = ((b * a) >> 14) + 0x3651;
    b = ((b * a) >> 14) + 0xA2F9;
    return (i * b) >> 16;
}

u16 ArcTan2(s16 xs, s16 ys)
{
    s32 x = xs, y = ys;
    if (!y) return x >= 0 ? 0 : 0x8000;
    if (!x) return y >= 0 ? 0x4000 : 0xC000;
    if (y >= 0) {
        if (x >= 0) {
            if (x >= y) return BiosArcTan((y << 14) / x);
        } else if (-x >= y) {
            return BiosArcTan((y << 14) / x) + 0x8000;
        }
        return 0x4000 - BiosArcTan((x << 14) / y);
    } else {
        if (x <= 0) {
            if (-x > -y) return BiosArcTan((y << 14) / x) + 0x8000;
        } else if (x >= -y) {
            return BiosArcTan((y << 14) / x) + 0x10000;
        }
        return 0xC000 - BiosArcTan((x << 14) / y);
    }
}

void SoftReset(u32 flags) { (void)flags; CrashFatal("SoftReset called"); }
int MultiBoot(void *mp) { (void)mp; return 1; }

void RegisterRamReset(u32 flags)
{
    if (flags & 0x01) memset(gGbaEwram, 0, gGbaEwramEnd - gGbaEwram);
    if (flags & 0x02) memset(gGbaIwram, 0, gGbaIwramEnd - gGbaIwram);
    if (flags & 0x04) memset(gGbaPltt, 0, sizeof gGbaPltt);
    if (flags & 0x08) memset(gGbaVram, 0, sizeof gGbaVram);
    if (flags & 0x10) memset(gGbaOam, 0, sizeof gGbaOam);
    if (flags & 0xE0) memset(gGbaIo, 0, sizeof gGbaIo);
    IO16(REG_KEYINPUT) = 0x3FF;
    IO16(REG_DISPSTAT) = DISPSTAT_HBLANK;
    IO16(0x20) = 0x100; IO16(0x26) = 0x100; IO16(0x30) = 0x100; IO16(0x36) = 0x100;
}

// Division by zero returned 0 on the GBA toolchain; keep that instead of trapping.
int __aeabi_idiv0(int r) { (void)r; return 0; }
long long __aeabi_ldiv0(long long r) { (void)r; return 0; }

// ---- DMA -------------------------------------------------------------
void GbaDmaSet(int num, const void *src, void *dest, unsigned int control)
{
    u32 base = 0xB0 + num * 12;
    IO32(base) = (u32)(uintptr_t)src;
    IO32(base + 4) = (u32)(uintptr_t)dest;
    IO32(base + 8) = control;

    u32 hi = control >> 16;
    if (!(hi & 0x8000)) return;
    if ((hi >> 12) & 3) return;            // sound FIFO / HBlank timing: handled natively
    u32 count = control & 0xFFFF;
    if (!count) count = num == 3 ? 0x10000 : 0x4000;
    int sctl = (hi >> 7) & 3, dctl = (hi >> 5) & 3;
    // A source inside the BIOS cannot be read by DMA; the hardware repeats
    // the last value it transferred. The game uses this (source 0) in places.
    static u32 sLatch;
    if ((uintptr_t)src < 0x4000) { src = &sLatch; sctl = 2; }
    src = GbaXlat(src); dest = GbaXlat(dest);
    if (hi & 0x0400) {
        if (count) sLatch = sctl == 0 ? ((const u32 *)src)[count - 1] : *(const u32 *)src;
    } else if (count) {
        u16 v = sctl == 0 ? ((const u16 *)src)[count - 1] : *(const u16 *)src;
        sLatch = v | (v << 16);
    }
    if (hi & 0x0400) {
        const u32 *s = src; u32 *d = dest;
        if (sctl == 0 && (dctl == 0 || dctl == 3)) memmove(d, s, count * 4);
        else while (count--) {
            *d = *s;
            if (sctl == 0) s++; else if (sctl == 1) s--;
            if (dctl == 0 || dctl == 3) d++; else if (dctl == 1) d--;
        }
    } else {
        const u16 *s = src; u16 *d = dest;
        if (sctl == 0 && (dctl == 0 || dctl == 3)) memmove(d, s, count * 2);
        else while (count--) {
            *d = *s;
            if (sctl == 0) s++; else if (sctl == 1) s--;
            if (dctl == 0 || dctl == 3) d++; else if (dctl == 1) d--;
        }
    }
    IO32(base + 8) = control & ~0x80000000u;   // immediate transfers finish at once
}

// ---- Interrupts ------------------------------------------------------
// Same dispatch order as the game's own IRQ entry code.
static const u16 sIrqOrder[13] = {
    1 << 7, 1 << 6, 1 << 0, 1 << 1, 1 << 2, 1 << 3, 1 << 4, 1 << 5,
    1 << 8, 1 << 9, 1 << 10, 1 << 11, 1 << 12,
};

void GbaRaiseIrq(u16 flag)
{
    if (!(IO16(REG_IME) & 1) || !(IO16(REG_IE) & flag)) return;
    for (int i = 0; i < 13; i++) {
        if (sIrqOrder[i] == flag) {
            u16 ime = IO16(REG_IME);
            IO16(REG_IF) &= ~flag;          // the handler acknowledges it
            if (gIntrTable[i]) gIntrTable[i]();
            IO16(REG_IME) = ime;
            return;
        }
    }
}

// ---- Frame -----------------------------------------------------------
static u16 sFrame[GBA_W * GBA_H];
static u16 sTallFrame[GBA_W * TALL_H];
static u16 sTallRight[GBA_W * 240];
static int sDepth;
static int sWaited;
static u32 sLastPresentMs;

static void SetLine(int line)
{
    u16 st = IO16(REG_DISPSTAT) & ~(DISPSTAT_VBLANK | DISPSTAT_VCOUNT);
    st |= DISPSTAT_HBLANK;                 // busy-waits for HBlank fall straight through
    if (line >= 160 && line < 227) st |= DISPSTAT_VBLANK;
    if (line == (st >> 8)) st |= DISPSTAT_VCOUNT;
    IO16(REG_DISPSTAT) = st;
    IO16(REG_VCOUNT) = line;
}

void GbaRunFrame(void)
{
    int tall = gSnapTall;                  // a main board: drawn whole, after the frame, from latched registers
    for (int line = 0; line < 228; line++) {
        SetLine(line);
        if (line < GBA_H) {
            u32 t = PlatTicksUs();
            if (tall) PpuLatchLine(line);
            else PpuRenderLine(line, sFrame + line * GBA_W);
            PerfAdd(PERF_PPU, PlatTicksUs() - t);
        }
        if (line == 160 && tall) {
            u32 t = PlatTicksUs();
            sDepth = PlatDepth();
            PpuRenderTall(sTallFrame, sTallRight, sDepth);
            PerfAdd(PERF_PPU, PlatTicksUs() - t);
        }
        u16 st = IO16(REG_DISPSTAT);
        if (line == 160 && (st & DISPSTAT_VBLANK_INTR)) GbaRaiseIrq(INTR_VBLANK);
        // The match fires at the start of the line; handlers wait for that
        // line's HBlank, so their writes land from the next line on.
        if ((st & DISPSTAT_VCOUNT_INTR) && (st & DISPSTAT_VCOUNT)) GbaRaiseIrq(INTR_VCOUNT);
        if (line == 160) {
            if (tall) PlatPresentTall(sTallFrame, sDepth > 0 ? sTallRight : NULL);
            else PlatPresent(sFrame);
            PerfFrameEnd();
            PortAudioFrame();
            PortSaveTick();
            gPortFrame++;
            {   // canary: the game must not write past the IWRAM image (see prepare.py)
                extern u8 gGbaIwramPad[];
                static int sPadNoted;
                if (!sPadNoted)
                    for (int i = 0; i < 0x2000; i += 4)
                        if (*(u32 *)(gGbaIwramPad + i)) { PlatLog("iwram pad written at +0x%x = %08x (frame %u)", i, *(u32 *)(gGbaIwramPad + i), (unsigned)gPortFrame); sPadNoted = 1; break; }
            }
            sLastPresentMs = PlatTicksMs();
            if (!PlatPump()) { PortSaveFlushNow(); PlatShutdown(); exit(0); }
            IO16(REG_KEYINPUT) = (~PlatKeys()) & 0x3FF;
        }
    }
    SetLine(160);                          // callers resume inside VBlank
}

void VBlankIntrWait(void)
{
    sWaited = 1;
    GbaRunFrame();
}

// Called once per main-loop pass. Loading states run with interrupts off and
// never wait for VBlank; keep the system serviced if that goes on for long.
void PortMainLoopTick(void)
{
    if (sWaited) { sWaited = 0; return; }
    if (PlatTicksMs() - sLastPresentMs > 100) GbaRunFrame();
}
