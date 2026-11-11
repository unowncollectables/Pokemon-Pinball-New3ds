// Tall table support: the game's sprite list is widened to 512 entries and
// keeps the full Y of every sprite, so the whole board can be drawn at once.
#include <string.h>
#include "port.h"

u8 gPortTall;                          // the game is showing a main board (read by patched game code)
u8 gSnapTall;                          // the sprite snapshot below was taken in tall mode
u8 gOamSnap[PORT_OAM_BYTES] __attribute__((aligned(4)));
s16 gOamYWSnap[PORT_OAM_N];
u8 gOamYWvSnap[PORT_OAM_N];

extern u8 gOamBuffer[];                // PORT_OAM_BYTES, laid out by the build
extern const u8 gEmptyOamData[];       // the game's 128 empty sprites
static s16 sYW[PORT_OAM_N];            // full Y written this frame, per sprite
static u8 sYWv[PORT_OAM_N];

#include <stdlib.h>
extern int gPortField, gPortBallY;
void PortUpdateTall(int on, int field, int ballY)
{
    gPortField = field == 1 ? 1 : 0;
    gPortBallY = ballY;
    static int off = -1;
    if (off < 0) off = getenv("PORT_NOTALL") != NULL;      // test switch: always the original picture
    gPortTall = on != 0 && !off;
}

void PortOamClear(void)
{
    for (int i = 0; i < 4; i++) memcpy(gOamBuffer + i * 0x400, gEmptyOamData, 0x400);
    memset(sYWv, 0, sizeof sYWv);
}

static inline int Slot(const void *o) { return (int)(((const u8 *)o - gOamBuffer) >> 3); }

void PortOamYSet(void *o, int v)
{
    int i = Slot(o);
    if ((unsigned)i < PORT_OAM_N) { sYW[i] = (s16)v; sYWv[i] = 1; }
    *(u8 *)o = (u8)v;
}

void PortOamYAdd(void *o, int v)
{
    int i = Slot(o);
    int cur = ((unsigned)i < PORT_OAM_N && sYWv[i]) ? sYW[i] : (s8)*(u8 *)o;
    PortOamYSet(o, cur + v);
}

void PortOamYSub(void *o, int v) { PortOamYAdd(o, -v); }

// The game's once-per-frame copy of its sprite list to the picture unit.
void PortOamCopy(void)
{
    memcpy(gOamSnap, gOamBuffer, PORT_OAM_BYTES);
    memcpy(gGbaOam, gOamBuffer, 0x400);
    memcpy(gOamYWSnap, sYW, sizeof sYW);
    memcpy(gOamYWvSnap, sYWv, sizeof sYWv);
    gSnapTall = gPortTall;
}
