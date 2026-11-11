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
#define TALL_BAND 320                  // first row of the fixed-on-screen band (menus, score)
extern u8 gPortTall, gSnapTall;
extern u8 gOamSnap[];
extern s16 gOamYWSnap[];
extern u8 gOamYWvSnap[];

// ---- PPU -------------------------------------------------------------
// Renders one scanline of the current register/VRAM state.
void PpuRenderLine(int y, u16 *dst);   // RGB565
void PpuLatchLine(int y);              // remembers this scanline's registers (tall mode renders after the frame)
void PpuRenderTall(u16 *dst);          // GBA_W x TALL_H RGB565, from the latched registers
extern u8 gPpuForceSlow;               // test switch: use the general renderer only

// ---- Frame timing (perf.c) ---------------------------------------------
enum { PERF_PPU, PERF_BLIT, PERF_VSYNC, PERF_SOUND, PERF_N };
#define PERF_WINDOW 120
void PerfAdd(int section, u32 us);
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
void PlatPresentTall(const u16 *fb);   // GBA_W x TALL_H RGB565

// Screen layouts (layout.c): the tall picture is arranged into the two panel images.
extern int gPortView;                 // 0 = 1:1 with side panels, 1 = zoomed and following the ball
const u16 *LayoutTop(void);           // 400x240, row-major
const u16 *LayoutBot(void);           // 320x240, row-major
void LayoutFrame(const u16 *tall);
void LayoutTableCols(int *topX0, int *topW, int *botX0, int *botW);
int  LayoutMarginsDirty(void);
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
