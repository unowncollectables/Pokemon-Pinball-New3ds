// Headless test platform: runs the game with scripted input and dumps frames.
//   pinball_headless --frames N --dump 60,120 --keys "100:A,200:START" --out dir
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <signal.h>
#include <ucontext.h>
#include "port.h"

static u32 sMaxFrames = 600;
static char sOut[256] = ".";
static u32 sDump[4096]; static int sNumDump; static u32 sDumpEvery;
static struct { u32 frame, len; u16 keys; } sKeys[4096]; static int sNumKeys;
static FILE *sWav; static u32 sWavFrames; static int sWavRate;
static int sBonusCheck, sTrace;

int HeadlessBonusStep(int isBonusField)
{
    if (!sBonusCheck) return 0;
    if (gPortFrame == 1300 && !isBonusField) {
        PlatLog("check: enter bonus");
        return 1;
    }
    if (gPortFrame == 1600 && isBonusField) {
        PlatLog("check: leave bonus");
        return 1;
    }
    return 0;
}

static u16 KeyBit(const char *n)
{
    static const char *names[] = { "A", "B", "SELECT", "START", "RIGHT", "LEFT", "UP", "DOWN", "R", "L" };
    for (int i = 0; i < 10; i++) if (!strcmp(n, names[i])) return 1 << i;
    return 0;
}

void PlatInit(int argc, char **argv)
{
    const char *view = getenv("PORT_VIEW");
    if (view) gPortView = atoi(view);
    sBonusCheck = getenv("PORT_TEST_BONUS") != NULL;
    sTrace = getenv("PORT_TEST_TRACE") != NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames")) sMaxFrames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--out")) snprintf(sOut, sizeof sOut, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--slow-ppu")) gPpuForceSlow = 1;
        else if (!strcmp(argv[i], "--every")) sDumpEvery = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dump")) {
            for (char *t = strtok(argv[++i], ","); t; t = strtok(NULL, ",")) sDump[sNumDump++] = atoi(t);
        } else if (!strcmp(argv[i], "--keys")) {      // frame[+len]:KEY[+KEY]
            for (char *t = strtok(argv[++i], ","); t; t = strtok(NULL, ",")) {
                char *c = strchr(t, ':'); if (!c) continue; *c++ = 0;
                char *pl = strchr(t, '+');
                sKeys[sNumKeys].len = pl ? atoi(pl + 1) : 4;
                sKeys[sNumKeys].frame = atoi(t);
                u16 k = 0; char *q;
                while ((q = strchr(c, '+'))) { *q = 0; k |= KeyBit(c); c = q + 1; }
                k |= KeyBit(c);
                sKeys[sNumKeys++].keys = k;
            }
        } else if (!strcmp(argv[i], "--wav")) {
            sWav = fopen(argv[++i], "wb");
            if (sWav) { char hdr[44] = {0}; fwrite(hdr, 1, 44, sWav); }
        }
    }
}

static void FinishWav(void)
{
    if (!sWav) return;
    u32 bytes = sWavFrames * 4, v;
    fseek(sWav, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, sWav); v = 36 + bytes; fwrite(&v, 4, 1, sWav);
    fwrite("WAVEfmt ", 1, 8, sWav); v = 16; fwrite(&v, 4, 1, sWav);
    u16 h = 1; fwrite(&h, 2, 1, sWav); h = 2; fwrite(&h, 2, 1, sWav);
    v = sWavRate; fwrite(&v, 4, 1, sWav); v = sWavRate * 4; fwrite(&v, 4, 1, sWav);
    h = 4; fwrite(&h, 2, 1, sWav); h = 16; fwrite(&h, 2, 1, sWav);
    fwrite("data", 1, 4, sWav); fwrite(&bytes, 4, 1, sWav);
    fclose(sWav); sWav = NULL;
}

int PlatPump(void) { return gPortFrame < sMaxFrames; }

u16 PlatKeys(void)
{
    u16 k = 0;
    for (int i = 0; i < sNumKeys; i++)
        if (gPortFrame >= sKeys[i].frame && gPortFrame < sKeys[i].frame + sKeys[i].len) k |= sKeys[i].keys;
    return k;
}

static void DumpPpm(const u16 *fb, int h);
static void DumpPanels(int withRight);
void PlatPresent(const u16 *fb) { DumpPpm(fb, GBA_H); LayoutMenuFrame(fb); DumpPanels(0); }
#include <pthread.h>
static pthread_t sWorker;
static pthread_mutex_t sMu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sCv = PTHREAD_COND_INITIALIZER;
static void (*sJobFn)(void *);
static void *sJobArg;
static int sJobPending, sJobBusy, sWorkerState;

static void *WorkerMain(void *a)
{
    (void)a;
    pthread_mutex_lock(&sMu);
    for (;;) {
        while (!sJobPending) pthread_cond_wait(&sCv, &sMu);
        void (*fn)(void *) = sJobFn; void *arg = sJobArg;
        sJobPending = 0; sJobBusy = 1;
        pthread_mutex_unlock(&sMu);
        fn(arg);
        pthread_mutex_lock(&sMu);
        sJobBusy = 0;
        pthread_cond_broadcast(&sCv);
    }
    return NULL;
}

int PlatJobStart(void (*fn)(void *), void *arg)
{
    if (sWorkerState == 0) {
        sWorkerState = getenv("PORT_NOTHREAD") || pthread_create(&sWorker, NULL, WorkerMain, NULL) ? -1 : 1;
    }
    if (sWorkerState < 0) return 0;
    pthread_mutex_lock(&sMu);
    sJobFn = fn; sJobArg = arg; sJobPending = 1;
    pthread_cond_broadcast(&sCv);
    pthread_mutex_unlock(&sMu);
    return 1;
}

void PlatJobWait(void)
{
    pthread_mutex_lock(&sMu);
    while (sJobPending || sJobBusy) pthread_cond_wait(&sCv, &sMu);
    pthread_mutex_unlock(&sMu);
}

static void DumpPanels(int withRight)
{
    int want = sDumpEvery && gPortFrame % sDumpEvery == 0;
    for (int i = 0; i < sNumDump; i++) if (sDump[i] == gPortFrame) want = 1;
    if (!want || !getenv("PORT_LAYOUTDUMP")) return;
    static const int W[3] = { 400, 320, 400 };
    for (int p = 0; p < (withRight ? 3 : 2); p++) {
        char path[320];
        snprintf(path, sizeof path, "%s/f%05u_%s.ppm", sOut, (unsigned)gPortFrame, p == 2 ? "topR" : p ? "bot" : "top");
        FILE *f = fopen(path, "wb");
        if (!f) continue;
        const u16 *img = p == 2 ? LayoutTopR() : p ? LayoutBot() : LayoutTop();
        fprintf(f, "P6\n%d 240\n255\n", W[p]);
        for (int i = 0; i < W[p] * 240; i++) {
            u16 c = img[i];
            u8 rgb[3] = { (c >> 11) << 3 | (c >> 11) >> 2, ((c >> 5) & 63) << 2 | ((c >> 5) & 63) >> 4, (c & 31) << 3 | (c & 31) >> 2 };
            fwrite(rgb, 1, 3, f);
        }
        fclose(f);
    }
}

int PlatDepth(void) { const char *e = getenv("PORT_DEPTH"); return e ? atoi(e) : 0; }

void PlatPresentTall(const u16 *fb, const u16 *right)
{
    DumpPpm(fb, TALL_H);
    LayoutFrame(fb, right);
    DumpPanels(right != NULL);
    if (sTrace) {
        extern int gPortField, gPortBallY;
        int row, moving;
        LayoutFollowInfo(&row, &moving);
        PlatLog("check: tall %u %d %d %d %d", gPortFrame, gPortField, gPortBallY, row, moving);
    }
}

static void DumpPpm(const u16 *fb, int h)
{
    int want = sDumpEvery && gPortFrame % sDumpEvery == 0;
    for (int i = 0; i < sNumDump; i++) if (sDump[i] == gPortFrame) want = 1;
    if (!want) return;
    char path[320];
    snprintf(path, sizeof path, "%s/f%05u.ppm", sOut, (unsigned)gPortFrame);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", GBA_W, h);
    for (int i = 0; i < GBA_W * h; i++) {
        u16 c = fb[i];
        u8 rgb[3] = { (c >> 11) << 3 | (c >> 11) >> 2, ((c >> 5) & 63) << 2 | ((c >> 5) & 63) >> 4, (c & 31) << 3 | (c & 31) >> 2 };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    if (getenv("PORT_DUMPMEM")) {              // raw hardware state, for analysis scripts
        static const struct { const char *n; const u8 *p; size_t len; } m[] = {
            { "io", gGbaIo, 0x400 }, { "vram", gGbaVram, 0x18000 }, { "pltt", gGbaPltt, 0x400 }, { "oam", gGbaOam, 0x400 },
            { "ewram", gGbaEwram, 0 }, };
        {
            extern u8 gBoardBGTileBuffer[], gBoardBGTileBufferAlt[];
            snprintf(path, sizeof path, "%s/f%05u.buf", sOut, (unsigned)gPortFrame);
            FILE *g = fopen(path, "wb");
            if (g) { fwrite(gBoardBGTileBuffer, 1, 32 * 0x400, g); fwrite(gBoardBGTileBufferAlt, 1, 32 * 0x400, g); fclose(g); }
        }
        for (int i = 0; i < 4; i++) {
            snprintf(path, sizeof path, "%s/f%05u.%s", sOut, (unsigned)gPortFrame, m[i].n);
            FILE *g = fopen(path, "wb");
            if (g) { fwrite(m[i].p, 1, m[i].len, g); fclose(g); }
        }
    }
}

void PlatAudioQueue(const s16 *stereo, int frames, int rate)
{
    if (!sWav) return;
    sWavRate = rate; sWavFrames += frames;
    fwrite(stereo, 4, frames, sWav);
}

u32 PlatTicksMs(void) { return 0; }
#include <time.h>
u32 PlatTicksUs(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (u32)(t.tv_sec * 1000000u + t.tv_nsec / 1000); }

int PlatFileRead(const char *name, void *buf, size_t size)
{
    char path[320]; snprintf(path, sizeof path, "%s/%s", sOut, name);
    FILE *f = fopen(path, "rb"); if (!f) return -1;
    int n = fread(buf, 1, size, f); fclose(f); return n;
}

int PlatFileWrite(const char *name, const void *buf, size_t size)
{
    char path[320]; snprintf(path, sizeof path, "%s/%s", sOut, name);
    FILE *f = fopen(path, "wb"); if (!f) return -1;
    int n = fwrite(buf, 1, size, f); fclose(f); return n;
}

void PlatLog(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr);
}

void PlatFatalScreen(const char *text) { fprintf(stderr, "FATAL:\n%s\n", text); FinishWav(); exit(3); }
void PlatShutdown(void) { FinishWav(); }

static void OnSignal(int sig, siginfo_t *si, void *ctx)
{
    ucontext_t *uc = ctx;
    extern char __executable_start[], etext[];
    static char stk[512]; int n = 0;
    unsigned long *sp = (unsigned long *)uc->uc_mcontext.arm_sp;
    for (int i = 0; i < 400 && n < 440; i++)
        if (sp[i] >= (unsigned long)__executable_start && sp[i] < (unsigned long)etext)
            n += snprintf(stk + n, sizeof stk - n, "%lx ", sp[i]);
    CrashFatal("signal %d, fault address %p, pc %08lx lr %08lx\nstack: %s", sig, si->si_addr,
               uc->uc_mcontext.arm_pc, uc->uc_mcontext.arm_lr, stk);
}

void AgbMain(void);
int main(int argc, char **argv)
{
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = OnSignal; sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL); sigaction(SIGILL, &sa, NULL); sigaction(SIGFPE, &sa, NULL);
    PlatInit(argc, argv);
    CrashInit();
    PortSaveInit();
    PortAudioInit();
    AgbMain();
    return 0;
}
