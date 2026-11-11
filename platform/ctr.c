// Nintendo 3DS platform layer (libctru).
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>
#include "port.h"

#define DATA_DIR "sdmc:/3ds/pinball-rs"

u32 __stacksize__ = 0x40000;

// ---- video -------------------------------------------------------------
static u16 sLut[32768];                    // GBA BGR555 -> RGB565
#define TOP_W 400
#define TOP_H 240

static void ClearTop(void)
{
    for (int i = 0; i < 2; i++) {
        u16 w, h;
        u8 *fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &w, &h);
        memset(fb, 0, (size_t)w * h * 2);
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
}

void PlatPresent(const u16 *src)
{
    u16 w, h;
    u16 *fb = (u16 *)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &w, &h);
    const int ox = (TOP_W - GBA_W) / 2, oy = (TOP_H - GBA_H) / 2;
    // The panel is stored rotated: column x, bottom-up.
    for (int x = 0; x < GBA_W; x++) {
        u16 *col = fb + (x + ox) * TOP_H + (TOP_H - 1 - oy);
        const u16 *s = src + x;
        for (int y = 0; y < GBA_H; y++, s += GBA_W) *col-- = sLut[*s & 0x7FFF];
    }
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
}

// ---- input -------------------------------------------------------------
static u16 sKeys;

u16 PlatKeys(void) { return sKeys; }

int PlatPump(void)
{
    if (!aptMainLoop()) return 0;
    hidScanInput();
    u32 k = hidKeysHeld();
    u16 g = 0;
    if (k & KEY_A) g |= 1 << 0;
    if (k & KEY_B) g |= 1 << 1;
    if (k & KEY_SELECT) g |= 1 << 2;
    if (k & KEY_START) g |= 1 << 3;
    if (k & (KEY_DRIGHT | KEY_CPAD_RIGHT)) g |= 1 << 4;
    if (k & (KEY_DLEFT | KEY_CPAD_LEFT)) g |= 1 << 5;
    if (k & (KEY_DUP | KEY_CPAD_UP)) g |= 1 << 6;
    if (k & (KEY_DDOWN | KEY_CPAD_DOWN)) g |= 1 << 7;
    if (k & (KEY_R | KEY_ZR)) g |= 1 << 8;
    if (k & (KEY_L | KEY_ZL)) g |= 1 << 9;
    sKeys = g;
    return 1;
}

// ---- audio -------------------------------------------------------------
#define NBUF 8
#define BUF_FRAMES 672
static ndspWaveBuf sWave[NBUF];
static s16 *sAudioMem;
static int sAudioOk, sNextBuf;

static void AudioInit(void)
{
    if (R_FAILED(ndspInit())) { PlatLog("audio: DSP firmware missing, running silent"); return; }
    sAudioMem = linearAlloc(NBUF * BUF_FRAMES * 4);
    if (!sAudioMem) { ndspExit(); return; }
    memset(sAudioMem, 0, NBUF * BUF_FRAMES * 4);
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(0);
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
    float mix[12] = { 1.0f, 1.0f };
    ndspChnSetMix(0, mix);
    memset(sWave, 0, sizeof sWave);
    for (int i = 0; i < NBUF; i++) {
        sWave[i].data_pcm16 = sAudioMem + i * BUF_FRAMES * 2;
        sWave[i].status = NDSP_WBUF_DONE;
    }
    sAudioOk = 1;
}

void PlatAudioQueue(const s16 *stereo, int frames, int rate)
{
    static int sRateSet;
    if (!sAudioOk) return;
    if (!sRateSet) {
        // One block per displayed frame: match the block rate to the panel's
        // refresh so the queue neither drains nor piles up.
        ndspChnSetRate(0, (float)frames * 59.8261f);
        sRateSet = 1;
        (void)rate;
    }
    if (frames > BUF_FRAMES) frames = BUF_FRAMES;

    int queued = 0;
    for (int i = 0; i < NBUF; i++)
        if (sWave[i].status == NDSP_WBUF_QUEUED || sWave[i].status == NDSP_WBUF_PLAYING) queued++;
    if (queued >= NBUF - 1) return;                 // far ahead: drop this block
    int reps = queued == 0 ? 3 : 1;                 // ran dry: rebuild a small cushion
    while (reps--) {
        ndspWaveBuf *wb = &sWave[sNextBuf];
        if (wb->status == NDSP_WBUF_QUEUED || wb->status == NDSP_WBUF_PLAYING) return;
        if (reps) memset(wb->data_pcm16, 0, frames * 4);
        else memcpy(wb->data_pcm16, stereo, frames * 4);
        wb->nsamples = frames;
        DSP_FlushDataCache(wb->data_pcm16, frames * 4);
        ndspChnWaveBufAdd(0, wb);
        sNextBuf = (sNextBuf + 1) % NBUF;
    }
}

// ---- files / log -------------------------------------------------------
static FILE *sLog;

int PlatFileRead(const char *name, void *buf, size_t size)
{
    char path[160]; snprintf(path, sizeof path, DATA_DIR "/%s", name);
    FILE *f = fopen(path, "rb"); if (!f) return -1;
    int n = fread(buf, 1, size, f); fclose(f); return n;
}

int PlatFileWrite(const char *name, const void *buf, size_t size)
{
    char path[160], tmp[168];
    snprintf(path, sizeof path, DATA_DIR "/%s", name);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb"); if (!f) return -1;
    size_t n = fwrite(buf, 1, size, f);
    fclose(f);
    if (n != size) { remove(tmp); return -1; }
    remove(path);                                   // write-then-rename: never a half-written save
    if (rename(tmp, path) != 0) return -1;
    return (int)n;
}

void PlatLog(const char *fmt, ...)
{
    char line[256];
    va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    printf("%s\n", line);
    if (sLog) { fprintf(sLog, "%s\n", line); fflush(sLog); }
    CrashNote("%s", line);
}

u32 PlatTicksMs(void) { return (u32)osGetTime(); }

void PlatShutdown(void)
{
    if (sLog) fclose(sLog);
    if (sAudioOk) { ndspChnWaveBufClear(0); ndspExit(); linearFree(sAudioMem); }
    gfxExit();
}

void PlatFatalScreen(const char *text)
{
    consoleInit(GFX_BOTTOM, NULL);
    printf("\x1b[2J\x1b[H%s\n\nSaved to %s/crash.txt\nPress START to exit.\n", text, DATA_DIR);
    gfxFlushBuffers(); gfxSwapBuffers();
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gspWaitForVBlank();
    }
    PlatShutdown();
    exit(1);
}

// ---- CPU exceptions -> crash logger -------------------------------------
static u8 sExcStack[0x4000] __attribute__((aligned(8)));
extern char __start__[], __end__[];
extern char etext[] __attribute__((weak));

static void OnException(ERRF_ExceptionInfo *ex, CpuRegisters *regs)
{
    static const char *kinds[] = { "prefetch abort", "data abort", "undefined instruction", "VFP" };
    static char stk[400];
    int n = 0;
    u32 *sp = (u32 *)regs->sp;
    // Return addresses still on the stack, newest first: a poor man's backtrace.
    if (((u32)sp & 3) == 0 && (u32)sp >= 0x08000000)
        for (int i = 0; i < 300 && n < 380; i++) {
            u32 v = sp[i];
            if (v >= (u32)__start__ && v < (u32)__end__ && (v & 3) == 0 && v < (u32)__start__ + 0x400000)
                n += snprintf(stk + n, sizeof stk - n, "%08lx ", (unsigned long)v);
        }
    CrashFatal("%s\npc  %08lx  lr %08lx\nsp  %08lx  far %08lx\nr0  %08lx  r1 %08lx\nr2  %08lx  r3 %08lx\nstack: %s",
               ex->type < 4 ? kinds[ex->type] : "exception",
               (unsigned long)regs->pc, (unsigned long)regs->lr, (unsigned long)regs->sp, (unsigned long)ex->far,
               (unsigned long)regs->r[0], (unsigned long)regs->r[1], (unsigned long)regs->r[2], (unsigned long)regs->r[3], stk);
}

void PlatInit(int argc, char **argv)
{
    (void)argc; (void)argv;
    osSetSpeedupEnable(true);                       // New 3DS: full clock speed
    gfxInitDefault();
    gfxSetScreenFormat(GFX_TOP, GSP_RGB565_OES);
    gfxSetDoubleBuffering(GFX_TOP, true);
    gfxSetDoubleBuffering(GFX_BOTTOM, false);
    consoleInit(GFX_BOTTOM, NULL);
    threadOnException(OnException, sExcStack + sizeof sExcStack, WRITE_DATA_TO_HANDLER_STACK);

    mkdir("sdmc:/3ds", 0777);
    mkdir(DATA_DIR, 0777);
    sLog = fopen(DATA_DIR "/log.txt", "w");

    for (int i = 0; i < 32768; i++) {
        int r = i & 31, g = (i >> 5) & 31, b = (i >> 10) & 31;
        sLut[i] = (r << 11) | (((g << 1) | (g >> 4)) << 5) | b;
    }
    ClearTop();
    AudioInit();
}

void AgbMain(void);

int main(int argc, char **argv)
{
    PlatInit(argc, argv);
    CrashInit();
    PlatLog("Pokemon Pinball R&S 3DS port");
    PlatLog("build %s", PORT_BUILD_ID);
    PlatLog("L/R or ZL/ZR flippers, A/B, START");
    PortSaveInit();
    PortAudioInit();
    AgbMain();
    return 0;
}
