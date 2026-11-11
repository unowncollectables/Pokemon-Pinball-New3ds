// GPU presenter for the tall table: see gpu3ds.h.
#include <3ds.h>
#include <citro3d.h>
#include <string.h>
#include "gpu3ds.h"

extern const unsigned char blit_shbin[];        // the vertex shader, assembled and embedded by game.mk
extern const unsigned int blit_shbin_size;

#define TEX_TALL_W 256
#define TEX_TALL_H 512
#define TEX_RIGHT_H 256
#define TEX_ART_W 512
#define TEX_ART_H 256

// Render target -> screen: our screens are RGB565, so the targets are too.
#define SCREEN_FLAGS \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB565) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))
// Row-major RGB565 in linear memory -> the tiled layout the GPU samples.
#ifndef GPU_UPLOAD_FLIP
#define GPU_UPLOAD_FLIP 1
#endif
#define UPLOAD_FLAGS \
    (GX_TRANSFER_FLIP_VERT(GPU_UPLOAD_FLIP) | GX_TRANSFER_OUT_TILED(1) | GX_TRANSFER_RAW_COPY(0) | \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB565) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

typedef struct { float x, y, z, u, v; } Vtx;
#define MAX_VERTS 96

static int sState;                              // 0 not tried yet, 1 ready, -1 unavailable
static C3D_RenderTarget *sTopL, *sTopR, *sBot;
static C3D_Tex sTall, sRight, sArtTop, sArtBot;
static u16 *sStageTall, *sStageRight, *sStageTop, *sStageBot;   // row-major copies with the texture's row length
static Vtx *sVtx;
static int sNV;
static DVLB_s *sDvlb;
static shaderProgram_s sProg;
static int sUProj;
static C3D_Mtx sProjTop, sProjBot;
static int sArtUploaded;                        // the margin art textures hold real art

static void Teardown(void)
{
    if (sTopL) C3D_RenderTargetDelete(sTopL);
    if (sTopR) C3D_RenderTargetDelete(sTopR);
    if (sBot) C3D_RenderTargetDelete(sBot);
    sTopL = sTopR = sBot = NULL;
    C3D_TexDelete(&sTall); C3D_TexDelete(&sRight); C3D_TexDelete(&sArtTop); C3D_TexDelete(&sArtBot);
    memset(&sTall, 0, sizeof sTall); memset(&sRight, 0, sizeof sRight);
    memset(&sArtTop, 0, sizeof sArtTop); memset(&sArtBot, 0, sizeof sArtBot);
    linearFree(sStageTall); linearFree(sStageRight); linearFree(sStageTop); linearFree(sStageBot); linearFree(sVtx);
    sStageTall = sStageRight = sStageTop = sStageBot = NULL; sVtx = NULL;
    if (sDvlb) { shaderProgramFree(&sProg); DVLB_Free(sDvlb); sDvlb = NULL; }
}

static int TexOk(C3D_Tex *t, int w, int h)
{
    if (!C3D_TexInit(t, (u16)w, (u16)h, GPU_RGB565)) return 0;
    C3D_TexSetWrap(t, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    return 1;
}

int GpuInit(void)
{
    if (sState) return sState > 0;
    sState = -1;
    char c = '1';
    if (PlatFileRead("gpu.cfg", &c, 1) == 1 && c == '0') { PlatLog("gpu: off (gpu.cfg)"); return 0; }
    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE)) { PlatLog("gpu: C3D_Init failed, using the CPU presenter"); return 0; }

    sTopL = C3D_RenderTargetCreate(240, 400, GPU_RB_RGB565, -1);
    sTopR = C3D_RenderTargetCreate(240, 400, GPU_RB_RGB565, -1);
    sBot = C3D_RenderTargetCreate(240, 320, GPU_RB_RGB565, -1);
    if (!sTopL || !sTopR || !sBot) { PlatLog("gpu: render targets failed"); goto fail; }
    C3D_RenderTargetSetOutput(sTopL, GFX_TOP, GFX_LEFT, SCREEN_FLAGS);
    C3D_RenderTargetSetOutput(sTopR, GFX_TOP, GFX_RIGHT, SCREEN_FLAGS);
    C3D_RenderTargetSetOutput(sBot, GFX_BOTTOM, GFX_LEFT, SCREEN_FLAGS);

    if (!TexOk(&sTall, TEX_TALL_W, TEX_TALL_H) || !TexOk(&sRight, TEX_TALL_W, TEX_RIGHT_H) ||
        !TexOk(&sArtTop, TEX_ART_W, TEX_ART_H) || !TexOk(&sArtBot, TEX_ART_W, TEX_ART_H)) { PlatLog("gpu: textures failed"); goto fail; }
    sStageTall = linearAlloc(TEX_TALL_W * TEX_TALL_H * 2);
    sStageRight = linearAlloc(TEX_TALL_W * TEX_RIGHT_H * 2);
    sStageTop = linearAlloc(TEX_ART_W * TEX_ART_H * 2);
    sStageBot = linearAlloc(TEX_ART_W * TEX_ART_H * 2);
    sVtx = linearAlloc(MAX_VERTS * sizeof(Vtx));
    if (!sStageTall || !sStageRight || !sStageTop || !sStageBot || !sVtx) { PlatLog("gpu: out of linear memory"); goto fail; }
    memset(sStageTall, 0, TEX_TALL_W * TEX_TALL_H * 2);
    memset(sStageRight, 0, TEX_TALL_W * TEX_RIGHT_H * 2);
    memset(sStageTop, 0, TEX_ART_W * TEX_ART_H * 2);
    memset(sStageBot, 0, TEX_ART_W * TEX_ART_H * 2);

    sDvlb = DVLB_ParseFile((u32 *)blit_shbin, blit_shbin_size);
    if (!sDvlb) { PlatLog("gpu: shader did not parse"); goto fail; }
    shaderProgramInit(&sProg);
    shaderProgramSetVsh(&sProg, &sDvlb->DVLE[0]);
    C3D_BindProgram(&sProg);
    sUProj = shaderInstanceGetUniformLocation(sProg.vertexShader, "projection");

    C3D_AttrInfo *ai = C3D_GetAttrInfo();
    AttrInfo_Init(ai);
    AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 3);      // position, in screen pixels
    AttrInfo_AddLoader(ai, 1, GPU_FLOAT, 2);      // texture coordinate
    C3D_BufInfo *bi = C3D_GetBufInfo();
    BufInfo_Init(bi);
    BufInfo_Add(bi, sVtx, sizeof(Vtx), 2, 0x10);

    C3D_TexEnv *env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, 0, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_CullFace(GPU_CULL_NONE);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);

    Mtx_OrthoTilt(&sProjTop, 0.0f, 400.0f, 240.0f, 0.0f, 0.0f, 1.0f, true);
    Mtx_OrthoTilt(&sProjBot, 0.0f, 320.0f, 240.0f, 0.0f, 0.0f, 1.0f, true);

    sState = 1;
    PlatLog("gpu: citro3d presenter ready");
    return 1;
fail:
    Teardown();
    C3D_Fini();
    return 0;
}

// Copies w pixels of each of h rows into rows of `stride` pixels, then repeats the last pixel of every row and
// the last row once so that linear filtering at the edge never reads padding.
static void StageRows(u16 *dst, int stride, const u16 *src, int w, int h)
{
    for (int y = 0; y < h; y++) {
        u16 *d = dst + y * stride;
        memcpy(d, src + y * w, (size_t)w * 2);
        d[w] = d[w - 1];
    }
    memcpy(dst + h * stride, dst + (h - 1) * stride, (size_t)stride * 2);
}

static void Upload(C3D_Tex *tex, void *stage, int w, int h)
{
    GSPGPU_FlushDataCache(stage, (u32)(w * h * 2));
    C3D_SyncDisplayTransfer((u32 *)stage, GX_BUFFER_DIM(w, h), (u32 *)tex->data, GX_BUFFER_DIM(w, h), UPLOAD_FLAGS);
}

static void Quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1)
{
    Vtx *v = &sVtx[sNV];
    v[0] = (Vtx){ x0, y0, 0.5f, u0, v0 }; v[1] = (Vtx){ x1, y0, 0.5f, u1, v0 }; v[2] = (Vtx){ x1, y1, 0.5f, u1, v1 };
    v[3] = (Vtx){ x0, y0, 0.5f, u0, v0 }; v[4] = (Vtx){ x1, y1, 0.5f, u1, v1 }; v[5] = (Vtx){ x0, y1, 0.5f, u0, v1 };
    sNV += 6;
}

typedef struct { C3D_Tex *tex; int linear, first, count; } Draw;

static void Run(const Draw *d, int n)
{
    for (int i = 0; i < n; i++) {
        GPU_TEXTURE_FILTER_PARAM f = d[i].linear ? GPU_LINEAR : GPU_NEAREST;
        C3D_TexSetFilter(d[i].tex, f, f);
        C3D_TexBind(0, d[i].tex);
        C3D_DrawArrays(GPU_TRIANGLES, d[i].first, d[i].count);
    }
}

void GpuPresentTall(const u16 *tall, const u16 *right)
{
    u32 t0 = PlatTicksUs();
    StageRows(sStageTall, TEX_TALL_W, tall, GBA_W, TALL_H);
    if (right) StageRows(sStageRight, TEX_TALL_W, right, GBA_W, TEX_RIGHT_H - 16);   // the right eye picture is 240 rows
    int art = LayoutMarginsDirty() || !sArtUploaded;
    if (art) {
        StageRows(sStageTop, TEX_ART_W, LayoutTop(), 400, 240);
        StageRows(sStageBot, TEX_ART_W, LayoutBot(), 320, 240);
    }
    LayoutMarginsDone();
    PerfAdd(PERF_BLIT, PlatTicksUs() - t0);

    u32 t1 = PlatTicksUs();
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);               // waits for the previous frame and for vblank
    u32 t2 = PlatTicksUs();
    PerfAdd(PERF_VSYNC, t2 - t1);

    Upload(&sTall, sStageTall, TEX_TALL_W, TEX_TALL_H);
    if (right) Upload(&sRight, sStageRight, TEX_TALL_W, TEX_RIGHT_H);
    if (art) { Upload(&sArtTop, sStageTop, TEX_ART_W, TEX_ART_H); Upload(&sArtBot, sStageBot, TEX_ART_W, TEX_ART_H); sArtUploaded = 1; }

    // ---- geometry. Texture coordinates are fractions of the (padded) texture. ----
    const float W = TEX_TALL_W, H = TEX_TALL_H, HR = TEX_RIGHT_H;
    const float eps = 0.03f;                           // keeps a sample off the exact edge between two texels
    Draw top[4], topR[4], bot[4];
    int nt = 0, ntr = 0, nb = 0;
    sNV = 0;
    top[nt++] = (Draw){ &sArtTop, 0, sNV, 6 };  Quad(0, 0, 400, 240, 0, 0, 400.0f / TEX_ART_W, 240.0f / TEX_ART_H);
    bot[nb++] = (Draw){ &sArtBot, 0, sNV, 6 };  Quad(0, 0, 320, 240, 0, 0, 320.0f / TEX_ART_W, 240.0f / TEX_ART_H);
    if (right) topR[ntr++] = (Draw){ &sArtTop, 0, top[0].first, 6 };   // the right eye has the same margin art

    if (!gPortView) {
        // whole table 1:1: rows 0-239 on the top screen at x 80, rows 240-479 on the bottom one at x 40
        top[nt++] = (Draw){ &sTall, 0, sNV, 6 };  Quad(80, 0, 320, 240, 0, 0, 240.0f / W, 240.0f / H);
        bot[nb++] = (Draw){ &sTall, 0, sNV, 6 };  Quad(40, 0, 280, 240, 0, 240.0f / H, 240.0f / W, 480.0f / H);
        if (right) { topR[ntr++] = (Draw){ &sRight, 0, sNV, 6 }; Quad(80, 0, 320, 240, 0, 0, 240.0f / W, 240.0f / HR); }
    } else {
        // following view: 4/3 scale, output row y shows board row (r0x16 + 12 y) / 16. At rest the picture is sampled
        // pixel for pixel (hard edges); while the camera moves it is filtered so the table scrolls smoothly.
        int r0x16, moving;
        LayoutFollowInfo(&r0x16, &moving);
        const float rt = (float)r0x16 / 16.0f;
        const float ru0 = (0.25f + eps) / W, ru1 = (240.25f + eps) / W;       // 3 source pixels -> 4 output pixels, as the CPU zoom does
        // top screen: 240 output rows = 180 board rows
        float v0 = rt + 0.125f + eps;
        top[nt++] = (Draw){ &sTall, moving, sNV, 6 };  Quad(40, 0, 360, 240, ru0, v0 / H, ru1, (v0 + 180.0f) / H);
        // bottom screen starts gPortSeam output rows further on
        float b0 = rt + 180.0f + gPortSeam * 0.75f + 0.125f + eps;
        bot[nb++] = (Draw){ &sTall, moving, sNV, 6 };  Quad(0, 0, 320, 240, ru0, b0 / H, ru1, (b0 + 180.0f) / H);
        if (right) {
            // the right eye exists for board rows 0-239 only; below that both eyes see the flat picture
            int split = (int)((239.5f - rt) / 0.75f + 0.999f);
            if (split < 0) split = 0;
            if (split > 240) split = 240;
            if (split > 0) {
                topR[ntr++] = (Draw){ &sRight, moving, sNV, 6 };
                Quad(40, 0, 360, (float)split, ru0, v0 / HR, ru1, (v0 + 0.75f * split) / HR);
            }
            if (split < 240) {
                topR[ntr++] = (Draw){ &sTall, moving, sNV, 6 };
                Quad(40, (float)split, 360, 240, ru0, (v0 + 0.75f * split) / H, ru1, (v0 + 180.0f) / H);
            }
        }
    }
    GSPGPU_FlushDataCache(sVtx, (u32)(sNV * (int)sizeof(Vtx)));

    C3D_FrameDrawOn(sTopL);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, sUProj, &sProjTop);
    Run(top, nt);
    if (right) {
        C3D_FrameDrawOn(sTopR);
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, sUProj, &sProjTop);
        Run(topR, ntr);
    }
    C3D_FrameDrawOn(sBot);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, sUProj, &sProjBot);
    Run(bot, nb);
    C3D_FrameEnd(0);
    PerfAdd(PERF_GPU, PlatTicksUs() - t2);
}

void GpuIdle(void)
{
    if (sState <= 0) return;
    gspWaitForVBlank();
    gspWaitForVBlank();
}

void GpuShutdown(void)
{
    if (sState <= 0) return;
    GpuIdle();
    Teardown();
    C3D_Fini();
    sState = -1;
}
