// Pinball 3DS port layer: GBA hardware model + platform interface.
// Everything in port/ is original port code; nothing here comes from the game.
#ifndef PORT_H
#define PORT_H

#include <stdint.h>
#include <stddef.h>

#if defined(GUARD_GBA_TYPES_H)
// game-side file: the game's own integer types are already in scope
#elif defined(__3DS__)
#include <3ds/types.h>
#else
typedef uint8_t u8;  typedef uint16_t u16; typedef unsigned int u32;
typedef int8_t s8;   typedef int16_t s16;  typedef int s32;
#endif

#define GBA_W 240
#define GBA_H 160

// ---- GBA memory ------------------------------------------------------
extern u8 gGbaPltt[0x400];
extern u8 gGbaVram[0x18000];
extern u8 gGbaOam[0x400];
extern u8 gGbaIo[0x400];
extern u8 gGbaSram[0x10000];
void PpuNoteTileWrite(const void *dest, const void *src, u32 bytes);
void PpuLightReset(void);
extern u8 gGbaEwram[], gGbaEwramEnd[], gGbaIwram[], gGbaIwramEnd[];

#define IO16(off) (*(volatile u16 *)(gGbaIo + (off)))
#define IO32(off) (*(volatile u32 *)(gGbaIo + (off)))

void *GbaXlat(const void *p);          // raw GBA bus address -> host pointer (identity otherwise)
void GbaRaiseIrq(u16 flag);
void GbaRunFrame(void);                // one full video frame, with interrupts
extern u32 gPortFrame;                 // frames presented so far

// ---- Tall table ----------------------------------------------------------
#define PORT_OAM_N 512                 // sprites the game may use (the GBA has 128)
#define PORT_OAM_BYTES (PORT_OAM_N * 8)
#define PORT_WB 2048                   // Y bias the patched game adds to world sprites
#define TALL_H 480                     // two 240-row screens, one above the other
#define TALL_BAND 280                  // default first row of the fixed-on-screen band (menus, score): centred on the bottom screen
extern u8 gPortTall, gSnapTall;
extern u8 gOamSnap[];
extern s16 gOamYWSnap[];
extern u8 gOamYWvSnap[];

// ---- PPU -------------------------------------------------------------
// Renders one scanline of the current register/VRAM state.
void PpuRenderLine(int y, u16 *dst);   // RGB565
void PpuLatchLine(int y);              // remembers this scanline's registers (tall mode renders after the frame)
void PpuRenderTall(u16 *dst, u16 *right, int depth);          // GBA_W x TALL_H RGB565, from the latched registers
extern u8 gPpuForceSlow;               // test switch: use the general renderer only

// ---- GPU composition of the tall table (3DS: platform/gpucomp3ds.c; the CPU compose stays the reference) ----
typedef struct { s16 x, y; u8 w, h, prio, flags, aff, ui; u16 tile, palbase; } Spr;   // one sprite of the tall frame
enum { SF_AFFINE = 1, SF_DOUBLE = 2, SF_H = 4, SF_V = 8, SF_8BPP = 16, SF_SEMI = 32 };
#define LTEXW 512                      // row length of a layer texture: 256 map columns and the 240-pixel wrap-around copy, rounded up
#define GPU_SHEET 512                  // side of the texture that holds the pictures of affine sprites
#define BAND_ROWS 144                  // rows of the fixed band that BG0 covers
typedef struct {
    int ui0;                           // tall row where the fixed band (BG0 and the UI sprites) sits
    u16 back;                          // backdrop colour, RGB565
    u8 bgOn[4], bgPrio[4];             // BG0 (the band) to BG3
    u16 scroll[4];                     // BG1-BG3: first column of the 240-pixel window in the layer texture
    int nSpr; const Spr *spr;          // sprites in OAM order; of the same priority the last one is drawn first
    int map1d;                         // OBJ tiles in 1D mapping
    int stereo;                        // a right-eye picture is wanted
    int dx[2][4];                      // eye shift of BG1-BG3 and the world sprites: [0] left eye rows 0-239, [1] right eye
    const u8 *objChr;                  // the sprite tile memory (0x8000 bytes) as the frame saw it: the game rewrites it in its VBlank handler, before the GPU draws
    s16 affPar[32][4];                 // affine matrices (PA PB PC PD) as the frame saw them
    u16 sheetX[PORT_OAM_N], sheetY[PORT_OAM_N];   // where each affine sprite's picture sits in the sheet texture (its padding starts here)
    int fade, evy;                     // brightness fade over everything: 0 none, 1 towards white, 2 towards black; evy 1-16
    int eva, evb;                      // blend weights (0-16) of semi-transparent sprites
    int bandChanged;                   // the band picture differs from the last frame's
} PpuGpuFrame;
extern int gPpuGpuCompose;             // set by the platform: 0 off, 1 describe frames for the GPU and skip the CPU compose, 2 also compose on the CPU (to compare)
void PpuGpuSetStage(u16 *layer[3], u16 *band);   // linear-memory RGBA5551 staging: three layers of LTEXW x 512, the band 256 x 256
const PpuGpuFrame *PpuGpuFrameGet(void);         // NULL: this frame is not eligible, the CPU picture is the frame
const u16 *PpuGpuLayerStage(int n, int *rowLo, int *rowHi);   // layer n (1-3): NULL when unchanged, else rows [rowLo,rowHi) (multiples of 8) are new
const u16 *PpuPal565(void);

// ---- Frame timing (perf.c) ---------------------------------------------
enum { PERF_PPU, PERF_BLIT, PERF_VSYNC, PERF_SOUND, PERF_GPU, PERF_GPUC, PERF_N };   // PERF_GPU: building and submitting the presenter's GPU frame; PERF_GPUC: the CPU time of the GPU compose (0 when the CPU composes)
#define PERF_WINDOW 120
void PerfAdd(int section, u32 us);
void PpuStats(unsigned *slowRows, unsigned *rebuilt, unsigned *bld, unsigned *disp, unsigned *mainUs, unsigned *waitUs);   // per-frame averages since the last call
void PerfFrameEnd(void);               // once per presented frame; logs every PERF_WINDOW frames

// ---- Audio -----------------------------------------------------------
void PortAudioInit(void);
void PortAudioFrame(void);             // called once per video frame

// ---- Save ------------------------------------------------------------
void PortSaveInit(void);
void PortSaveMarkDirty(void);
void PortSaveTick(void);               // flushes a dirty save once writes go quiet
void PortSaveFlushNow(void);

// ---- Crash logger ----------------------------------------------------
void CrashInit(void);
void CrashNote(const char *fmt, ...);  // breadcrumb ring, written out on a crash
int PortAudioDump(char *dst, int cap);   // sound-engine state for the crash report
void CrashFatal(const char *fmt, ...) __attribute__((noreturn));

// ---- Platform --------------------------------------------------------
void PlatInit(int argc, char **argv);
int  PlatPump(void);                   // 0 = quit requested
u16  PlatKeys(void);                   // GBA KEYINPUT bit layout, 1 = pressed
void PlatPresent(const u16 *fb);       // GBA_W x GBA_H RGB565
void PlatPresentTall(const u16 *fb, const u16 *right);   // GBA_W x TALL_H RGB565; right = the right eye's rows 0-239, or NULL
int  PlatDepth(void);
// A second thread for splitting the picture work (the New 3DS has a spare core).
// PlatJobStart returns 1 if fn(arg) now runs on that thread (call PlatJobWait before touching the result),
// or 0 if there is no such thread and the caller should run it itself.
int  PlatJobStart(void (*fn)(void *), void *arg);
void PlatJobWait(void);                 // 3D slider, 0 (off) to 256

// Screen layouts (layout.c): the tall picture is arranged into the two panel images.
extern int gPortSeam;                 // following view: table rows hidden behind the gap between the screens (SELECT + up/down), 0-24
extern int gPortView;                 // 0 = 1:1 with side panels, 1 = zoomed and following the ball
const u16 *LayoutTop(void);           // 400x240, row-major
const u16 *LayoutBot(void);           // 320x240, row-major
void LayoutStats(unsigned scoreLo, unsigned scoreHi, int lives, int coins);
int  LayoutCamera(void);              // once per frame before drawing: moves the camera, returns the tall row where the fixed-on-screen band is drawn
void LayoutFrame(const u16 *tall, const u16 *right);
void LayoutPrepare(void);              // margin art and number panels only (what the GPU presenter needs; the table itself is drawn by the GPU)
void LayoutFollowInfo(int *r0x16, int *moving);   // following view: first board row shown, in 1/16 rows, and whether the camera is moving
void LayoutMenuFrame(const u16 *pic);   // a 240x160 picture (menus, bonus stages), scaled to fill the top screen
const u16 *LayoutTopR(void);          // right-eye top panel, valid after LayoutFrame with a right image
void LayoutTableCols(int *topX0, int *topW, int *botX0, int *botW);
int  LayoutMarginsDirty(void);
void LayoutRepaint(void);             // copy the margins to the panels again (a screen mode changed)
void LayoutMarginsDone(void);
void PlatAudioQueue(const s16 *stereo, int frames, int rate);
u32  PlatTicksMs(void);
u32  PlatTicksUs(void);                // wrapping microsecond clock
int  PlatFileRead(const char *name, void *buf, size_t size);    // bytes read or -1
int  PlatFileWrite(const char *name, const void *buf, size_t size);
void PlatLog(const char *fmt, ...);
void PlatFatalScreen(const char *text) __attribute__((noreturn));
void PlatShutdown(void);

#endif
