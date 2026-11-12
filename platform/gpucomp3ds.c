// GPU compositor for the tall table: see gpucomp3ds.h.
//
// Textures (all tiled, uploaded or written with the same convention as the presenter: image row i is memory row H-1-i,
// sampled with v = i / H):
//   layer 1-3   RGBA5551, LTEXW x 512: the cached BG rows from gba_ppu.c (colour, alpha 1 where the layer has a pixel)
//   band        RGBA5551, 256 x 256:  BG0 in its fixed band
//   atlas       RGBA5551, 512 x 1024: every 8x8 OBJ tile that is visible, decoded with its palette; exact cache keyed by
//               (tile, palette bank) and checked against the tile's bytes and the bank's colours, so a changed tile is redrawn
// Everything is drawn as textured rectangles in the priority order of the CPU compose: for p = 3..0, BG layers of that
// priority (BG3 first), the band if it is BG0's priority, then the sprites of that priority, highest OAM slot first.
#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpucomp3ds.h"

#define LTEX_H 512
#define AW 512
#define AH 1024
#define SLOTS ((AW / 8) * (AH / 8))
#define MAXQ 4096                       // sprite tiles per frame
#define MAXOPS 40
#define MAXV (6 * (MAXQ * 3 + 64))

#ifndef GPUC_RT_YFLIP
#define GPUC_RT_YFLIP 1
#endif
#ifndef GPUC_RB_FLIP
#define GPUC_RB_FLIP 1
#endif

#define UPLOAD_FLAGS \
    (GX_TRANSFER_FLIP_VERT(1) | GX_TRANSFER_OUT_TILED(1) | GX_TRANSFER_RAW_COPY(0) | \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB5A1) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB5A1) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))
#define READBACK_FLAGS \
    (GX_TRANSFER_FLIP_VERT(GPUC_RB_FLIP) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB565) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

typedef struct { float x, y, z, u, v; } Vtx;
typedef struct { s16 x, y; u16 slot; u8 flip, ui; } SQ;          // one 8x8 sprite tile, position before the eye shift
typedef struct { u8 used; u8 bank; u16 slot; u16 tile; u16 pad; u32 raw[8]; u16 pal[15]; u16 pad2; } AEnt;
// mode 0: drawn over what is there. 2: semi sprite tiles in a sprite buffer (they clear it: they hide the sprites under them).
// 3: switch to the sprite buffer and clear it. 4: switch back to the picture.
typedef struct { C3D_Tex *tex; int first, count; int mode; } Op;
static float sZ = 0.5f;

static int sReady, sUProj;
static C3D_Tex sLayerTex[3], sBandTex, sAtlas, sRtTexL, sRtTexR, sBackTex, sSprTex;
static C3D_RenderTarget *sRtS;           // the sprite buffer: one priority level of sprites, RGBA, for levels that hold semi sprites
static int sBackKnown; static u16 sBackCol;
static C3D_RenderTarget *sRtL, *sRtR;
static u16 *sStageLayer[3], *sStageBand, *sRb;
static Vtx *sVb;
static int sNv;
static SQ *sSq;
static int sNq;
static int sSprFirst[PORT_OAM_N], sSprCount[PORT_OAM_N];
static AEnt *sTab;
static unsigned sNextSlot;
static u8 sMort[64];
static C3D_Mtx sProjL, sProjR;
static int sBandUp;                      // the band texture holds a picture
static unsigned sDirtyLo = 0xFFFFFFFFu, sDirtyHi;

// statistics for the perf window
static unsigned sStFrames, sStQuads, sStDecoded, sStResets, sStUploadRows;
static char sDbg = '0';                  // gpuc.dbg: debug switches for bring-up (see GpucCompose)

int GpucInit(int uProj)
{
    if (sReady) return 1;
    sUProj = uProj;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            sMort[y * 8 + x] = (u8)((x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3));
    sTab = calloc(16384, sizeof(AEnt));
    sSq = malloc(MAXQ * sizeof(SQ));
    sVb = linearAlloc(MAXV * sizeof(Vtx));
    for (int i = 0; i < 3; i++) { sStageLayer[i] = linearAlloc(LTEXW * LTEX_H * 2); if (sStageLayer[i]) memset(sStageLayer[i], 0, LTEXW * LTEX_H * 2); }
    sStageBand = linearAlloc(256 * 256 * 2);
    if (sStageBand) memset(sStageBand, 0, 256 * 256 * 2);
    if (!sTab || !sSq || !sVb || !sStageLayer[0] || !sStageLayer[1] || !sStageLayer[2] || !sStageBand) { PlatLog("gpuc: out of memory"); return 0; }
    for (int i = 0; i < 3; i++) {
        if (!C3D_TexInit(&sLayerTex[i], LTEXW, LTEX_H, GPU_RGBA5551)) { PlatLog("gpuc: layer texture failed"); return 0; }
        C3D_TexSetFilter(&sLayerTex[i], GPU_NEAREST, GPU_NEAREST);
        C3D_TexSetWrap(&sLayerTex[i], GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        memset(sLayerTex[i].data, 0, LTEXW * LTEX_H * 2);
        GSPGPU_FlushDataCache(sLayerTex[i].data, LTEXW * LTEX_H * 2);
    }
    if (!C3D_TexInit(&sBandTex, 256, 256, GPU_RGBA5551) || !C3D_TexInit(&sAtlas, AW, AH, GPU_RGBA5551)) { PlatLog("gpuc: band/atlas texture failed"); return 0; }
    if (!C3D_TexInit(&sBackTex, 8, 8, GPU_RGBA5551)) { PlatLog("gpuc: backdrop texture failed"); return 0; }
    C3D_TexSetFilter(&sBackTex, GPU_NEAREST, GPU_NEAREST); C3D_TexSetWrap(&sBackTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    C3D_TexSetFilter(&sBandTex, GPU_NEAREST, GPU_NEAREST); C3D_TexSetWrap(&sBandTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    C3D_TexSetFilter(&sAtlas, GPU_NEAREST, GPU_NEAREST);   C3D_TexSetWrap(&sAtlas, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    memset(sBandTex.data, 0, 256 * 256 * 2); memset(sAtlas.data, 0, AW * AH * 2);
    GSPGPU_FlushDataCache(sBandTex.data, 256 * 256 * 2); GSPGPU_FlushDataCache(sAtlas.data, AW * AH * 2);
    if (!C3D_TexInitVRAM(&sRtTexL, 256, 512, GPU_RGB565) || !C3D_TexInitVRAM(&sRtTexR, 256, 256, GPU_RGB565)) { PlatLog("gpuc: render target texture failed"); return 0; }
    C3D_TexSetFilter(&sRtTexL, GPU_NEAREST, GPU_NEAREST); C3D_TexSetWrap(&sRtTexL, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    C3D_TexSetFilter(&sRtTexR, GPU_NEAREST, GPU_NEAREST); C3D_TexSetWrap(&sRtTexR, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    sRtL = C3D_RenderTargetCreateFromTex(&sRtTexL, GPU_TEXFACE_2D, 0, -1);
    sRtR = C3D_RenderTargetCreateFromTex(&sRtTexR, GPU_TEXFACE_2D, 0, -1);
    if (!C3D_TexInitVRAM(&sSprTex, 256, 512, GPU_RGBA5551)) { PlatLog("gpuc: sprite buffer texture failed"); return 0; }
    C3D_TexSetFilter(&sSprTex, GPU_NEAREST, GPU_NEAREST); C3D_TexSetWrap(&sSprTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    sRtS = C3D_RenderTargetCreateFromTex(&sSprTex, GPU_TEXFACE_2D, 0, -1);
    if (!sRtS) { PlatLog("gpuc: sprite buffer target failed"); return 0; }
    if (!sRtL || !sRtR) { PlatLog("gpuc: render targets failed"); return 0; }
    // pixels -> the render-target textures, picture row 0 at the top (memory row H-1), like the uploaded textures
    Mtx_Ortho(&sProjL, 0.0f, 256.0f, GPUC_RT_YFLIP ? 0.0f : 512.0f, GPUC_RT_YFLIP ? 512.0f : 0.0f, 0.0f, 1.0f, true);
    Mtx_Ortho(&sProjR, 0.0f, 256.0f, GPUC_RT_YFLIP ? 0.0f : 256.0f, GPUC_RT_YFLIP ? 256.0f : 0.0f, 0.0f, 1.0f, true);
    PpuGpuSetStage(sStageLayer, sStageBand);
    PlatFileRead("gpuc.dbg", &sDbg, 1);
    sReady = 1;
    PlatLog("gpuc: GPU compositor ready (atlas %dx%d, %d tile slots)", AW, AH, SLOTS);
    return 1;
}

C3D_Tex *GpucTall(void) { return &sRtTexL; }
C3D_Tex *GpucRight(void) { return &sRtTexR; }

// ---- the OBJ tile atlas ------------------------------------------------------------------------------------------
static inline u16 To5551(u16 c) { return (u16)((c & 0xF800) | (c & 0x07C0) | ((c & 0x1F) << 1) | 1); }

static void Decode(unsigned slot, const u8 *src, const u16 *pal)
{
    unsigned tx = slot & 63, ty = slot >> 6;
    unsigned block = (AH / 8 - 1 - ty) * (AW / 8) + tx;         // memory rows are the picture rows upside down
    u16 *d = (u16 *)sAtlas.data + block * 64;
    for (int py = 0; py < 8; py++)
        for (int px = 0; px < 8; px++) {
            u8 b = src[py * 4 + (px >> 1)];
            unsigned v = (px & 1) ? b >> 4 : b & 15;
            d[sMort[(7 - py) * 8 + px]] = v ? To5551(pal[v]) : 0;
        }
    unsigned off = block * 128;
    if (off < sDirtyLo) sDirtyLo = off;
    if (off + 128 > sDirtyHi) sDirtyHi = off + 128;
}

static void AtlasReset(void)
{
    memset(sTab, 0, 16384 * sizeof(AEnt));
    sNextSlot = 0;
    sStResets++;
}

static int AtlasSlot(unsigned tile, unsigned bank)
{
    AEnt *e = &sTab[tile * 16 + bank];
    const u8 *src = gGbaVram + 0x10000 + tile * 32;
    const u16 *pal = PpuPal565() + 0x100 + bank * 16;
    if (e->used && !memcmp(e->raw, src, 32) && !memcmp(e->pal, pal + 1, 30)) return e->slot;
    if (!e->used) { e->used = 1; e->bank = (u8)bank; e->tile = (u16)tile; e->slot = (u16)sNextSlot++; }
    memcpy(e->raw, src, 32);
    memcpy(e->pal, pal + 1, 30);
    Decode(e->slot, src, pal);
    sStDecoded++;
    return e->slot;
}

// Every visible sprite tile of the frame, resolved to its atlas slot (so the atlas is complete before anything is drawn).
static void BuildTiles(const PpuGpuFrame *f)
{
    unsigned need = 0;
    for (int i = 0; i < f->nSpr; i++) need += (f->spr[i].w >> 3) * (f->spr[i].h >> 3);
    if (sNextSlot + need > SLOTS) AtlasReset();
    sNq = 0;
    for (int i = 0; i < f->nSpr; i++) {
        const Spr *sp = &f->spr[i];
        int cols = sp->w >> 3, rows = sp->h >> 3;
        int hf = (sp->flags & SF_H) != 0, vf = (sp->flags & SF_V) != 0;
        int rowTiles = f->map1d ? cols : 32;
        unsigned bank = (sp->palbase >> 4) & 15;
        sSprFirst[i] = sNq;
        for (int tr = 0; tr < rows; tr++)
            for (int tc = 0; tc < cols; tc++) {
                int x = sp->x + tc * 8, y = sp->y + tr * 8;
                if (x + 8 <= -16 || x >= 256) continue;
                if (sp->ui ? (y + 8 <= 0 || y >= 160) : (y + 8 <= 0 || y >= TALL_H)) continue;
                if (sNq >= MAXQ) continue;
                int sr = vf ? rows - 1 - tr : tr, sc = hf ? cols - 1 - tc : tc;
                unsigned t = (sp->tile + sr * rowTiles + sc) & 0x3FF;
                sSq[sNq++] = (SQ){ (s16)x, (s16)y, (u16)AtlasSlot(t, bank), (u8)((hf ? 1 : 0) | (vf ? 2 : 0)), sp->ui };
            }
        sSprCount[i] = sNq - sSprFirst[i];
    }
    if (sDirtyHi > sDirtyLo) {
        GSPGPU_FlushDataCache((u8 *)sAtlas.data + sDirtyLo, sDirtyHi - sDirtyLo);
        sDirtyLo = 0xFFFFFFFFu; sDirtyHi = 0;
    }
}

// ---- textures from the CPU side ------------------------------------------------------------------------------------
static void UploadRows(C3D_Tex *tex, const u16 *stage, int stride, int texH, int lo, int hi)
{
    int rows = hi - lo;
    GSPGPU_FlushDataCache((void *)(stage + lo * stride), (u32)(rows * stride * 2));
    C3D_SyncDisplayTransfer((u32 *)(stage + lo * stride), GX_BUFFER_DIM(stride, rows),
                            (u32 *)((u8 *)tex->data + (texH - hi) * stride * 2), GX_BUFFER_DIM(stride, rows), UPLOAD_FLAGS);
    sStUploadRows += (unsigned)rows;
}

// ---- geometry ------------------------------------------------------------------------------------------------------
// One rectangle clipped to columns 0-239 and rows [rlo, rhi) of the picture, the texture coordinates clipped with it.
static void Rect(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, float rlo, float rhi)
{
    float cx0 = x0 < 0 ? 0 : x0, cx1 = x1 > 240 ? 240 : x1;
    float cy0 = y0 < rlo ? rlo : y0, cy1 = y1 > rhi ? rhi : y1;
    if (cx0 >= cx1 || cy0 >= cy1 || sNv + 6 > MAXV) return;
    float du = (u1 - u0) / (x1 - x0), dv = (v1 - v0) / (y1 - y0);
    float a0 = u0 + (cx0 - x0) * du, a1 = u0 + (cx1 - x0) * du;
    float b0 = v0 + (cy0 - y0) * dv, b1 = v0 + (cy1 - y0) * dv;
    Vtx *v = &sVb[sNv];
    v[0] = (Vtx){ cx0, cy0, sZ, a0, b0 }; v[1] = (Vtx){ cx1, cy0, sZ, a1, b0 }; v[2] = (Vtx){ cx1, cy1, sZ, a1, b1 };
    v[3] = (Vtx){ cx0, cy0, sZ, a0, b0 }; v[4] = (Vtx){ cx1, cy1, sZ, a1, b1 }; v[5] = (Vtx){ cx0, cy1, sZ, a0, b1 };
    sNv += 6;
}

static void PushOp(Op *ops, int *n, C3D_Tex *tex, int first)
{
    if (sNv > first && *n < MAXOPS) ops[(*n)++] = (Op){ tex, first, sNv - first, 0 };
}

// The draw list of one pass: rows [rlo, rhi) of the picture with the eye shifts dx (BG1-BG3, world sprites).
static void Pass(const PpuGpuFrame *f, const int *dx, int rlo, int rhi, Op *ops, int *nops)
{
    *nops = 0;
    { int first = sNv; Rect(0, (float)rlo, 240, (float)rhi, 0.5f, 0.5f, 0.5f, 0.5f, (float)rlo, (float)rhi); PushOp(ops, nops, &sBackTex, first); }   // the backdrop colour
    for (int p = 3; p >= 0; p--) {
        for (int n = 3; n >= 1; n--) {
            if (!f->bgOn[n] || f->bgPrio[n] != p || sDbg == (char)('1' + n) || sDbg == '8') continue;   // dbg 2/3/4: leave out BG1/BG2/BG3
            int first = sNv;
            float s = f->scroll[n], d = (float)dx[n - 1];
            Rect(d, 0, d + 240, TALL_H, s / LTEXW, 0, (s + 240) / LTEXW, (float)TALL_H / LTEX_H, (float)rlo, (float)rhi);
            PushOp(ops, nops, &sLayerTex[n - 1], first);
        }
        if (f->bgOn[0] && f->bgPrio[0] == p && sDbg != '8') {
            int first = sNv;
            Rect(0, (float)f->ui0, 240, (float)(f->ui0 + BAND_ROWS), 0, 0, 240.0f / 256, (float)BAND_ROWS / 256, (float)rlo, (float)rhi);
            PushOp(ops, nops, &sBandTex, first);
        }
        // The sprites of this level. A level that holds semi sprites goes through the sprite buffer: highest OAM slot first,
        // and a semi sprite clears the buffer where it is opaque (it takes the place of the sprites under it, and with
        // blend 0:16 shows what is beneath), then the buffer is laid over the picture.
        int semis = 0, any = 0;
        for (int i = 0; i < f->nSpr; i++) if (f->spr[i].prio == p) { any = 1; if (f->spr[i].flags & SF_SEMI) semis = 1; }
        if (!any || sDbg == '5') continue;
        if (semis) {
            int first = sNv;
            Rect(0, (float)rlo, 240, (float)rhi, 0.5f, 0.5f, 0.5f, 0.5f, (float)rlo, (float)rhi);
            if (sNv > first && *nops < MAXOPS) { ops[*nops] = (Op){ &sBackTex, first, sNv - first, 3 }; (*nops)++; }
        }
        int first = sNv, cur = 0;
        for (int i = f->nSpr - 1; i >= 0; i--) {
            const Spr *sp = &f->spr[i];
            if (sp->prio != p) continue;
            int mode = (sp->flags & SF_SEMI) ? 2 : 0;
            if (mode != cur) {
                if (sNv > first && *nops < MAXOPS) { ops[*nops] = (Op){ &sAtlas, first, sNv - first, cur }; (*nops)++; }
                first = sNv; cur = mode;
            }
            for (int k = 0; k < sSprCount[i]; k++) {
                const SQ *q = &sSq[sSprFirst[i] + k];
                float x = q->x + (q->ui ? 0 : dx[3]);
                float y = q->y + (q->ui ? f->ui0 : 0);
                float lo = (float)rlo, hi = (float)rhi;
                if (q->ui) { if (lo < f->ui0) lo = (float)f->ui0; if (hi > f->ui0 + 160) hi = (float)(f->ui0 + 160); }
                float u0 = (q->slot & 63) / 64.0f, v0 = (q->slot >> 6) / 128.0f, u1 = u0 + 1.0f / 64, v1 = v0 + 1.0f / 128;
                if (q->flip & 1) { float t = u0; u0 = u1; u1 = t; }
                if (q->flip & 2) { float t = v0; v0 = v1; v1 = t; }
                Rect(x, y, x + 8, y + 8, u0, v0, u1, v1, lo, hi);
            }
        }
        if (sNv > first && *nops < MAXOPS) { ops[*nops] = (Op){ &sAtlas, first, sNv - first, cur }; (*nops)++; }
        if (semis) {
            if (*nops < MAXOPS) ops[(*nops)++] = (Op){ NULL, 0, 0, 4 };
            first = sNv;
            Rect(0, (float)rlo, 240, (float)rhi, 0, (float)rlo / 512, 240.0f / 256, (float)rhi / 512, (float)rlo, (float)rhi);
            PushOp(ops, nops, &sSprTex, first);
        }
    }
}

static void Run(const Op *ops, int n, C3D_RenderTarget *rt, const C3D_Mtx *proj)
{
    int blendZero = 0;
    for (int i = 0; i < n; i++) {
        int m = ops[i].mode;
        if (m == 4) {                                    // back to the picture
            C3D_FrameDrawOn(rt);
            C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, sUProj, proj);
            if (blendZero) { C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA); blendZero = 0; }
            continue;
        }
        if (m == 3) {                                    // into the sprite buffer, cleared (every channel 0)
            C3D_FrameDrawOn(sRtS);
            C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, sUProj, &sProjL);
        }
        int wantZero = m == 2 || m == 3;
        if (wantZero != blendZero) {
            blendZero = wantZero;
            if (wantZero) C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ZERO, GPU_ZERO, GPU_ZERO, GPU_ZERO);
            else C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
        }
        C3D_TexBind(0, ops[i].tex);
        C3D_DrawArrays(GPU_TRIANGLES, ops[i].first, ops[i].count);
    }
    if (blendZero) C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
}

static u32 ClearColor(u16 c565)
{
    u32 r = c565 >> 11, g = (c565 >> 5) & 63, b = c565 & 31;
    return ((r << 3 | r >> 2) << 24) | ((g << 2 | g >> 4) << 16) | ((b << 3 | b >> 2) << 8) | 0xFF;
}

int GpucCompose(const PpuGpuFrame *f)
{
    if (!sReady) return 0;
    // 1. textures that changed
    for (int n = 1; n <= 3; n++) {
        if (!f->bgOn[n]) continue;
        int lo = 0, hi = 0;
        const u16 *st = PpuGpuLayerStage(n, &lo, &hi);
        if (st) UploadRows(&sLayerTex[n - 1], st, LTEXW, LTEX_H, lo, hi);
    }
    if (f->bgOn[0] && (f->bandChanged || !sBandUp)) { UploadRows(&sBandTex, sStageBand, 256, 256, 0, 256); sBandUp = 1; }
    if (!sBackKnown || sBackCol != f->back) {
        u16 v = To5551(f->back);
        for (int i = 0; i < 64; i++) ((u16 *)sBackTex.data)[i] = v;
        GSPGPU_FlushDataCache(sBackTex.data, 128);
        sBackCol = f->back; sBackKnown = 1;
    }
    // 2. sprite tiles into the atlas
    BuildTiles(f);
    // 3. draw lists: one pass for a mono frame, three for stereo (left eye rows 0-239, flat rows 240-479, right eye rows 0-239)
    static const int zero[4];
    Op opsA[MAXOPS], opsB[MAXOPS], opsC[MAXOPS];
    int nA = 0, nB = 0, nC = 0;
    sNv = 0;
    if (f->stereo) {
        Pass(f, f->dx[0], 0, 240, opsA, &nA);
        Pass(f, zero, 240, TALL_H, opsB, &nB);
        Pass(f, f->dx[1], 0, 240, opsC, &nC);
    } else {
        Pass(f, zero, 0, TALL_H, opsA, &nA);
    }
    GSPGPU_FlushDataCache(sVb, (u32)(sNv * (int)sizeof(Vtx)));
    sStFrames++; sStQuads += (unsigned)(sNv / 6);
    // 4. draw
    C3D_BufInfo *bi = C3D_GetBufInfo();
    BufInfo_Init(bi);
    BufInfo_Add(bi, sVb, sizeof(Vtx), 2, 0x10);
    C3D_AlphaTest(true, GPU_GREATER, 0);
    u32 clr = sDbg == '1' ? 0xFF00FFFFu : ClearColor(f->back);       // '1': magenta, nothing drawn (is the render target read back at all?)
    C3D_RenderTargetClear(sRtL, C3D_CLEAR_COLOR, clr, 0);
    C3D_FrameDrawOn(sRtL);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, sUProj, &sProjL);
    if (sDbg != '1') { Run(opsA, nA, sRtL, &sProjL); Run(opsB, nB, sRtL, &sProjL); }
    if (f->stereo) {
        C3D_RenderTargetClear(sRtR, C3D_CLEAR_COLOR, clr, 0);
        C3D_FrameDrawOn(sRtR);
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, sUProj, &sProjR);
        if (sDbg != '1') Run(opsC, nC, sRtR, &sProjR);
    }
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    return 1;
}

// ---- compare mode --------------------------------------------------------------------------------------------------
static unsigned sCmpFrames, sCmpSame, sCmpSum, sCmpMax, sCmpWorstLo, sCmpWorstHi, sCmpDumps;

static void Count(const u16 *gpu, int gstride, const u16 *cpu, int rows, unsigned *diff, int *lo, int *hi)
{
    for (int y = 0; y < rows; y++)
        for (int x = 0; x < GBA_W; x++)
            if (gpu[y * gstride + x] != cpu[y * GBA_W + x]) { (*diff)++; if (y < *lo) *lo = y; if (y > *hi) *hi = y; }
}

static void Dump(const char *name, const u16 *img, int stride, int rows)
{
    static u16 tmp[GBA_W * TALL_H];
    for (int y = 0; y < rows; y++) memcpy(tmp + y * GBA_W, img + y * stride, GBA_W * 2);
    PlatFileWrite(name, tmp, (size_t)rows * GBA_W * 2);
}

void GpucCompare(const u16 *cpuTall, const u16 *cpuRight, int stereo)
{
    if (!sReady) return;
    if (!sRb) { sRb = linearAlloc(256 * 512 * 2); if (!sRb) return; }
    unsigned diff = 0; int lo = 9999, hi = -1;
    C3D_SyncDisplayTransfer((u32 *)sRtTexL.data, GX_BUFFER_DIM(256, 512), (u32 *)sRb, GX_BUFFER_DIM(256, 512), READBACK_FLAGS);
    GSPGPU_InvalidateDataCache(sRb, 256 * 512 * 2);
    Count(sRb, 256, cpuTall, TALL_H, &diff, &lo, &hi);
    unsigned dumpDiff = diff;
    if (diff && diff > sCmpMax && sCmpDumps < 6) {
        char n1[24], n2[24];
        snprintf(n1, sizeof n1, "cmp%u_cpu.bin", sCmpDumps); snprintf(n2, sizeof n2, "cmp%u_gpu.bin", sCmpDumps);
        Dump(n1, cpuTall, GBA_W, TALL_H); Dump(n2, sRb, 256, TALL_H);
        PlatLog("gpuc: dumped %s / %s (%u pixels differ, rows %d-%d)", n1, n2, diff, lo, hi);
        sCmpDumps++;
    }
    if (stereo && cpuRight) {
        C3D_SyncDisplayTransfer((u32 *)sRtTexR.data, GX_BUFFER_DIM(256, 256), (u32 *)sRb, GX_BUFFER_DIM(256, 256), READBACK_FLAGS);
        GSPGPU_InvalidateDataCache(sRb, 256 * 256 * 2);
        Count(sRb, 256, cpuRight, 240, &diff, &lo, &hi);
    }
    sCmpFrames++; sCmpSum += diff;
    if (!diff) sCmpSame++;
    if (diff > sCmpMax) { sCmpMax = diff; sCmpWorstLo = (unsigned)lo; sCmpWorstHi = (unsigned)hi; }
    (void)dumpDiff;
    if (sCmpFrames >= 120) {
        PlatLog("gpuc: compare %u frames: %u identical, avg %u px differ, worst %u px (rows %u-%u)",
                sCmpFrames, sCmpSame, sCmpSum / sCmpFrames, sCmpMax, sCmpWorstLo, sCmpWorstHi);
        sCmpFrames = sCmpSame = sCmpSum = 0; sCmpMax = 0;
    }
}

void GpucStats(void)
{
    if (!sStFrames) return;
    PlatLog("gpuc: %u frames composed, %u quads/frame, %u tiles decoded/frame, %u rows uploaded/frame, atlas %u/%d slots, %u resets",
            sStFrames, sStQuads / sStFrames, sStDecoded / sStFrames, sStUploadRows / sStFrames, sNextSlot, SLOTS, sStResets);
    sStFrames = sStQuads = sStDecoded = sStUploadRows = 0;
}

void GpucShutdown(void)
{
    if (!sReady) return;
    sReady = 0;
    if (sRtL) C3D_RenderTargetDelete(sRtL);
    if (sRtR) C3D_RenderTargetDelete(sRtR);
    sRtL = sRtR = NULL;
    C3D_TexDelete(&sRtTexL); C3D_TexDelete(&sRtTexR); C3D_TexDelete(&sAtlas); C3D_TexDelete(&sBandTex);
    for (int i = 0; i < 3; i++) { C3D_TexDelete(&sLayerTex[i]); linearFree(sStageLayer[i]); sStageLayer[i] = NULL; }
    linearFree(sStageBand); linearFree(sVb); linearFree(sRb);
    free(sTab); free(sSq);
    sTab = NULL; sSq = NULL; sVb = NULL; sRb = NULL; sStageBand = NULL;
}
