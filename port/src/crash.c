// Crash logger: a breadcrumb ring plus a fatal path that writes crash.txt and
// puts the report on screen. Platform exception hooks funnel into CrashFatal.
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "port.h"

#ifndef PORT_BUILD_ID
#define PORT_BUILD_ID "dev"
#endif

#define CRUMBS 16
static char sCrumb[CRUMBS][96];
static u32 sCrumbFrame[CRUMBS];
static int sCrumbHead;
static int sInFatal;

void CrashInit(void) { CrashNote("boot, build %s", PORT_BUILD_ID); }

void CrashNote(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vsnprintf(sCrumb[sCrumbHead], sizeof sCrumb[0], fmt, ap);
    va_end(ap);
    sCrumbFrame[sCrumbHead] = gPortFrame;
    sCrumbHead = (sCrumbHead + 1) % CRUMBS;
}

// Game state worth having in every report; weak so the logger works before
// the game is linked in.
extern u8 gMain[] __attribute__((weak));

void CrashFatal(const char *fmt, ...)
{
    static char text[4096];
    if (sInFatal) PlatFatalScreen("crash inside the crash logger");
    sInFatal = 1;
    int n = snprintf(text, sizeof text, "Pinball 3DS crash report\nbuild: %s\nframe: %u\n", PORT_BUILD_ID, (unsigned)gPortFrame);
    if (gMain) n += snprintf(text + n, sizeof text - n, "state: %d.%d\n", gMain[2], gMain[3]);
    n += snprintf(text + n, sizeof text - n, "what:  ");
    va_list ap; va_start(ap, fmt);
    n += vsnprintf(text + n, sizeof text - n, fmt, ap);
    va_end(ap);
    n += snprintf(text + n, sizeof text - n, "\n\nlast events:\n");
    for (int i = 0; i < CRUMBS; i++) {
        int k = (sCrumbHead + i) % CRUMBS;
        if (sCrumb[k][0] && n < (int)sizeof text - 120)
            n += snprintf(text + n, sizeof text - n, "  [%u] %s\n", (unsigned)sCrumbFrame[k], sCrumb[k]);
    }
    PlatFileWrite("crash.txt", text, strlen(text));
    PlatFatalScreen(text);
}
