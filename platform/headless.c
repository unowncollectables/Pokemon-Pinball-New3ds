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
static u32 sDump[256]; static int sNumDump; static u32 sDumpEvery;
static struct { u32 frame, len; u16 keys; } sKeys[256]; static int sNumKeys;
static FILE *sWav; static u32 sWavFrames; static int sWavRate;

static u16 KeyBit(const char *n)
{
    static const char *names[] = { "A", "B", "SELECT", "START", "RIGHT", "LEFT", "UP", "DOWN", "R", "L" };
    for (int i = 0; i < 10; i++) if (!strcmp(n, names[i])) return 1 << i;
    return 0;
}

void PlatInit(int argc, char **argv)
{
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

void PlatPresent(const u16 *fb)
{
    int want = sDumpEvery && gPortFrame % sDumpEvery == 0;
    for (int i = 0; i < sNumDump; i++) if (sDump[i] == gPortFrame) want = 1;
    if (!want) return;
    char path[320];
    snprintf(path, sizeof path, "%s/f%05u.ppm", sOut, (unsigned)gPortFrame);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", GBA_W, GBA_H);
    for (int i = 0; i < GBA_W * GBA_H; i++) {
        u16 c = fb[i];
        u8 rgb[3] = { (c >> 11) << 3 | (c >> 11) >> 2, ((c >> 5) & 63) << 2 | ((c >> 5) & 63) >> 4, (c & 31) << 3 | (c & 31) >> 2 };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
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
