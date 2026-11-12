// GPU compositor for the tall table (3DS, citro3d). Written for this port; the design (an exact tile cache feeding textured
// rectangles drawn in priority order) follows the approach of the SoulSilver port's GPU compositor.
//
// gba_ppu.c describes a frame (PpuGpuFrame): BG1-BG3 as cached layer textures, the sprite list, the fixed band. This
// module draws BG1-BG3 and the sprites as textured rectangles into two render-target textures (left eye / flat: the tall
// 240x480 picture, right eye: rows 0-239), which the presenter in gpu3ds.c then samples instead of the CPU picture.
#ifndef GPUCOMP3DS_H
#define GPUCOMP3DS_H

#include <citro3d.h>
#include "port.h"

int  GpucInit(int uProj);                       // uProj: the projection uniform of the shader that is bound
int  GpucCompose(const PpuGpuFrame *f);         // draws the frame; 1 = the picture is in GpucTall() / GpucRight()
C3D_Tex *GpucTall(void);                        // 256x512 RGB565, the picture is rows 0-479, columns 0-239
C3D_Tex *GpucRight(void);                       // 256x256 RGB565, rows 0-239
// Compare mode: reads the render targets back (call after the frame that drew them has finished) and counts the pixels that
// differ from the CPU picture of that frame; logs a summary every 120 frames and dumps the worst frame to the SD card.
void GpucCompare(const u16 *cpuTall, const u16 *cpuRight, int stereo);
void GpucStats(void);                           // logs the compose statistics (once per perf window)
void GpucShutdown(void);

#endif
