// GPU presenter for the tall table on the 3DS (written for this port; uses libctru's citro3d).
//
// The CPU still composes the 240x480 table picture (and the 240x240 right-eye picture) and the margin art;
// the GPU only puts them on the screens: textures plus a few textured rectangles per screen, drawn on render
// targets bound to the console's top (left and right eye) and bottom screens. That replaces the CPU zoom in
// layout.c and the CPU column-rotate copy in ctr.c.
#ifndef GPU3DS_H
#define GPU3DS_H

#include "port.h"

// Sets the presenter up on first use. 1 = ready; 0 = not available, the caller stays on the CPU presenter
// (also when sdmc:/3ds/pinball-rs/gpu.cfg starts with '0').
int  GpuInit(void);
// Draws both screens for the tall table. LayoutPrepare() must have run for this frame.
void GpuPresentTall(const u16 *tall, const u16 *right);
// Waits until the last GPU frame has reached the screens, so the CPU presenter may write them again.
void GpuIdle(void);
void GpuShutdown(void);

#endif
