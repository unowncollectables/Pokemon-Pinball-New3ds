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
#define TOP_W 400
#define TOP_H 240
#define BOT_W 320
#define BOT_H 240

static int sConsole = 1;                            // log lines also go to the bottom screen until the game draws

static void ClearTop(void)
{
    for (int i = 0; i < 2; i++) {
        u16 w, h;
        u8 *fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &w, &h);
        memset(fb, 0, (size_t)w * h * 2);
        if (!sConsole) {                            // the bottom screen is double buffered too once the game draws
            fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &w, &h);
            memset(fb, 0, (size_t)w * h * 2);
        }
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
}

// Which picture layout is on the panels: 0 = the plain 240x160 centred on the top screen, 1 = the tall table over both.
static int sLayout;

static void ClearBottom(void)
{
    u16 w, h;
    u8 *fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &w, &h);
    memset(fb, 0, (size_t)w * h * 2);
}

static void SetLayout(int layout)
{
    if (sConsole) {
        // From here the game draws the bottom screen every frame: double buffer it like the top one,
        // or the panel shows half-drawn frames (tearing).
        sConsole = 0;
        gfxSetDoubleBuffering(GFX_BOTTOM, true);
        ClearTop();
        LayoutRepaint();
    }
    if (layout == sLayout) return;
    sLayout = layout;
    ClearTop();                                     // both buffers: nothing of the old picture may linger
    ClearBottom();
    gfxFlushBuffers();
}

static void BlitCols(u16 *fb, int panelW, const u16 *img, int c0, int c1);
static int s3d;                                 // stereo output is switched on

void PlatPresent(const u16 *src)
{
    SetLayout(0);
    u32 t0 = PlatTicksUs();
    if (s3d) { s3d = 0; gfxSet3D(false); }
    LayoutMenuFrame(src);
    u16 w, h;
    u16 *top = (u16 *)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &w, &h);
    if (LayoutMarginsDirty()) {
        BlitCols(top, TOP_W, LayoutTop(), 0, 16);
        BlitCols(top, TOP_W, LayoutTop(), 384, TOP_W);
        BlitCols((u16 *)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &w, &h), BOT_W, LayoutBot(), 0, BOT_W);
        LayoutMarginsDone();
    }
    BlitCols(top, TOP_W, LayoutTop(), 16, 384);
    gfxFlushBuffers();
    gfxSwapBuffers();
    u32 t1 = PlatTicksUs();
    gspWaitForVBlank();
    PerfAdd(PERF_BLIT, t1 - t0);
    PerfAdd(PERF_VSYNC, PlatTicksUs() - t1);
}

// Rotates columns [c0, c1) of a row-major panel image onto the console panel (c0 and c1 multiples of 8).
static void BlitCols(u16 *fb, int panelW, const u16 *img, int c0, int c1)
{
    const int H = TOP_H;
    for (int x0 = c0; x0 < c1; x0 += 8) {
        u16 *col = fb + x0 * H + (H - 1);
        const u16 *s = img + x0;
        for (int y = 0; y < H; y++, s += panelW) {
            u16 *d = col - y;
            d[0] = s[0];
            d[H] = s[1];
            d[2 * H] = s[2];
            d[3 * H] = s[3];
            d[4 * H] = s[4];
            d[5 * H] = s[5];
            d[6 * H] = s[6];
            d[7 * H] = s[7];
        }
    }
}

// The tall table, arranged by layout.c: the upper half on the top screen, the lower half on the bottom one.
int PlatDepth(void)
{
    float v = osGet3DSliderState();
    return v < 0.03f ? 0 : (int)(v * 256.0f);
}

void PlatPresentTall(const u16 *src, const u16 *right)
{
    SetLayout(1);
    u32 t0 = PlatTicksUs();
    int on3d = right != NULL;
    if (on3d != s3d) { s3d = on3d; gfxSet3D(on3d); LayoutRepaint(); }
    LayoutFrame(src, right);
    int tx, tw, bx, bw;
    LayoutTableCols(&tx, &tw, &bx, &bw);
    u16 w, h;
    u16 *top = (u16 *)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &w, &h);
    u16 *topR = on3d ? (u16 *)gfxGetFramebuffer(GFX_TOP, GFX_RIGHT, &w, &h) : NULL;
    u16 *bot = (u16 *)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &w, &h);
    if (LayoutMarginsDirty()) {
        BlitCols(top, TOP_W, LayoutTop(), 0, tx);
        BlitCols(top, TOP_W, LayoutTop(), tx + tw, TOP_W);
        if (topR) {
            BlitCols(topR, TOP_W, LayoutTopR(), 0, tx);
            BlitCols(topR, TOP_W, LayoutTopR(), tx + tw, TOP_W);
        }
        BlitCols(bot, BOT_W, LayoutBot(), 0, BOT_W);
        LayoutMarginsDone();
    }
    BlitCols(top, TOP_W, LayoutTop(), tx, tx + tw);
    if (topR) BlitCols(topR, TOP_W, LayoutTopR(), tx, tx + tw);
    BlitCols(bot, BOT_W, LayoutBot(), bx, bx + bw);
    gfxFlushBuffers();
    gfxSwapBuffers();
    u32 t1 = PlatTicksUs();
    gspWaitForVBlank();
    PerfAdd(PERF_BLIT, t1 - t0);
    PerfAdd(PERF_VSYNC, PlatTicksUs() - t1);
}

static void SaveView(void);
static void WriteView(void);
static int sViewSaveAt;                        // frame number at which view.cfg is written (0 = nothing pending)

// ---- second thread -----------------------------------------------------
static Thread sWorker;
static LightEvent sGo, sDone;
static void (*sJobFn)(void *);
static void *sJobArg;
static volatile int sWorkerState;                 // 0 untried, 1 running, -1 unavailable
static volatile int sQuit;

static void WorkerMain(void *arg)
{
    (void)arg;
    for (;;) {
        LightEvent_Wait(&sGo);
        if (sQuit) return;
        sJobFn(sJobArg);
        LightEvent_Signal(&sDone);
    }
}

int PlatJobStart(void (*fn)(void *), void *arg)
{
    if (sWorkerState == 0) {
        LightEvent_Init(&sGo, RESET_ONESHOT);
        LightEvent_Init(&sDone, RESET_ONESHOT);
        s32 prio = 0x30;
        svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
        sWorker = threadCreate(WorkerMain, NULL, 64 * 1024, prio, 2, false);   // core 2: free on the New 3DS
        sWorkerState = sWorker ? 1 : -1;
        PlatLog("worker thread: %s", sWorker ? "running on core 2" : "unavailable, drawing on one core");
    }
    if (sWorkerState < 0) return 0;
    sJobFn = fn; sJobArg = arg;
    LightEvent_Signal(&sGo);
    return 1;
}

void PlatJobWait(void) { LightEvent_Wait(&sDone); }

// ---- input -------------------------------------------------------------
static u16 sKeys;

u16 PlatKeys(void) { return sKeys; }

int PlatPump(void)
{
    if (!aptMainLoop()) return 0;
    hidScanInput();
    if (sViewSaveAt && (int)gPortFrame >= sViewSaveAt) { sViewSaveAt = 0; WriteView(); }
    if (hidKeysDown() & KEY_Y) { gPortView ^= 1; SaveView(); }
    u32 k = hidKeysHeld();
    if ((k & KEY_SELECT) && gPortView) {    // SELECT + up/down: tune how many table rows hide behind the gap between the screens
        if ((hidKeysDown() & KEY_DUP) && gPortSeam < 24) { gPortSeam++; SaveView(); }
        if ((hidKeysDown() & KEY_DDOWN) && gPortSeam > 0) { gPortSeam--; SaveView(); }
    }
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

static void WriteView(void) { char v[2] = { gPortView ? '1' : '0', (char)('A' + gPortSeam) }; PlatFileWrite("view.cfg", v, 2); }
static void SaveView(void) { sViewSaveAt = (int)gPortFrame + 90; }   // after the buttons have rested for 1.5 s, so a press never stalls a frame

void PlatLog(const char *fmt, ...)
{
    char line[256];
    va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    if (sConsole) printf("%s\n", line);
    if (sLog) { fprintf(sLog, "%s\n", line); fflush(sLog); }
    if (strncmp(line, "perf:", 5)) CrashNote("%s", line);     // perf lines live in log.txt; keep the crumbs for events
}

u32 PlatTicksMs(void) { return (u32)osGetTime(); }
u32 PlatTicksUs(void) { return (u32)(svcGetSystemTick() * 1000000ull / SYSCLOCK_ARM11); }

void PlatShutdown(void)
{
    if (sViewSaveAt) { sViewSaveAt = 0; WriteView(); }
    if (sLog) fclose(sLog);
    if (sAudioOk) { ndspChnWaveBufClear(0); ndspExit(); linearFree(sAudioMem); }
    gfxExit();
}

void PlatFatalScreen(const char *text)
{
    gfxSetDoubleBuffering(GFX_BOTTOM, false);
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
    CrashFatal("%s\npc  %08lx  lr %08lx\nsp  %08lx  far %08lx\nr0  %08lx  r1 %08lx\nr2  %08lx  r3 %08lx\nr4  %08lx  r5 %08lx\nr6  %08lx  r7 %08lx\nstack: %s",
               ex->type < 4 ? kinds[ex->type] : "exception",
               (unsigned long)regs->pc, (unsigned long)regs->lr, (unsigned long)regs->sp, (unsigned long)ex->far,
               (unsigned long)regs->r[0], (unsigned long)regs->r[1], (unsigned long)regs->r[2], (unsigned long)regs->r[3],
               (unsigned long)regs->r[4], (unsigned long)regs->r[5], (unsigned long)regs->r[6], (unsigned long)regs->r[7], stk);
}

void PlatInit(int argc, char **argv)
{
    (void)argc; (void)argv;
    osSetSpeedupEnable(true);                       // New 3DS: full clock speed
    gfxInitDefault();
    gfxSetScreenFormat(GFX_TOP, GSP_RGB565_OES);
    gfxSetScreenFormat(GFX_BOTTOM, GSP_RGB565_OES);
    gfxSetDoubleBuffering(GFX_TOP, true);
    gfxSetDoubleBuffering(GFX_BOTTOM, false);
    consoleInit(GFX_BOTTOM, NULL);
    threadOnException(OnException, sExcStack + sizeof sExcStack, WRITE_DATA_TO_HANDLER_STACK);

    mkdir("sdmc:/3ds", 0777);
    mkdir(DATA_DIR, 0777);
    sLog = fopen(DATA_DIR "/log.txt", "w");

    { char v[2] = { 0, 0 }; int n = PlatFileRead("view.cfg", v, 2); if (n >= 1) gPortView = v[0] == '1'; if (n >= 2 && v[1] >= 'A' && v[1] <= 'A' + 24) gPortSeam = v[1] - 'A'; }
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
    {
        bool n3 = false;
        APT_CheckNew3DS(&n3);
        PlatLog("system: new3ds=%d heap=%uMB linear free=%uKB", (int)n3, (unsigned)(envGetHeapSize() >> 20),
                (unsigned)(linearSpaceFree() >> 10));
        PlatLog("perf lines below: fps, frame time, then ms per frame spent in game / ppu / blit / sound / idle (waiting for vsync)");
    }
    PortSaveInit();
    PortAudioInit();
    AgbMain();
    return 0;
}
