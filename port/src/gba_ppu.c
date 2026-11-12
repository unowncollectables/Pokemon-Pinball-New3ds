// Software GBA picture unit: mode 0 text backgrounds, regular and affine
// sprites, alpha/brightness blending and mosaic. That is everything this
// game uses (no windows, no bitmap or affine background modes).
//
// Two renderers produce identical pictures (the headless build can check
// that): a fast one that paints layers back to front straight into RGB565,
// and the general one below that composites per pixel. Lines that use alpha
// blending or mosaic take the general one.
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "port.h"

static __thread const u8 *sIo = gGbaIo;     // register file being read: live, or one latched scanline (tall mode)
#define R16(off) (*(const u16 *)(sIo + (off)))

enum { L_BG0, L_BG1, L_BG2, L_BG3, L_OBJ, L_BD };

static u16 sBg[4][GBA_W];          // palette index, 0 = transparent
static u16 sObj[GBA_W];            // 0 = none, else 0x100 | index
static u8  sObjPrio[GBA_W];        // 0-3, 0xFF = none
static u8  sObjSemi[GBA_W];

static inline u16 Pal(u32 idx) { return ((const u16 *)gGbaPltt)[idx] & 0x7FFF; }

static void RenderBg(int n, int y)
{
    u16 cnt = R16(0x08 + n * 2);
    u32 hofs = R16(0x10 + n * 4) & 0x1FF;
    u32 vofs = R16(0x12 + n * 4) & 0x1FF;
    const u8 *chr = gGbaVram + ((cnt >> 2) & 3) * 0x4000;
    const u16 *map = (const u16 *)(gGbaVram + ((cnt >> 8) & 0x1F) * 0x800);
    int size = cnt >> 14;
    u32 wmask = (size & 1) ? 0x1FF : 0xFF;
    u32 hmask = (size & 2) ? 0x1FF : 0xFF;
    int bpp8 = cnt & 0x80;
    u16 *out = sBg[n];

    int mosH = 1;
    if (cnt & 0x40) {
        u16 mos = R16(0x4C);
        mosH = (mos & 0xF) + 1;
        int mosV = ((mos >> 4) & 0xF) + 1;
        y -= y % mosV;
    }

    u32 sy = (y + vofs) & hmask;
    const u16 *row = map;
    if (sy >= 256) row += (size == 3) ? 0x800 : 0x400;
    row += ((sy & 0xFF) >> 3) * 32;
    u32 ty = sy & 7;

    u32 sx = hofs & wmask;
    int x = -(int)(sx & 7);
    sx &= ~7u;
    for (; x < GBA_W; x += 8, sx = (sx + 8) & wmask) {
        u16 e = row[((sx & 0xFF) >> 3) + ((sx >= 256) ? 0x400 : 0)];
        u32 tile = e & 0x3FF;
        u32 line = (e & 0x800) ? 7 - ty : ty;
        if (bpp8) {
            const u8 *p = chr + tile * 64 + line * 8;
            if (p + 8 > gGbaVram + 0x10000) { for (int i = 0; i < 8; i++) if ((unsigned)(x + i) < GBA_W) out[x + i] = 0; continue; }
            for (int i = 0; i < 8; i++) {
                int px = x + i;
                if ((unsigned)px >= GBA_W) continue;
                out[px] = p[(e & 0x400) ? 7 - i : i];
            }
        } else {
            const u8 *p = chr + tile * 32 + line * 4;
            if (p + 4 > gGbaVram + 0x10000) { for (int i = 0; i < 8; i++) if ((unsigned)(x + i) < GBA_W) out[x + i] = 0; continue; }
            u32 bits = p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24);
            u16 pal = (e >> 12) << 4;
            if (!bits) {
                for (int i = 0; i < 8; i++) if ((unsigned)(x + i) < GBA_W) out[x + i] = 0;
                continue;
            }
            for (int i = 0; i < 8; i++) {
                int px = x + i;
                if ((unsigned)px >= GBA_W) continue;
                u32 c = (bits >> (((e & 0x400) ? 7 - i : i) * 4)) & 0xF;
                out[px] = c ? (pal | c) : 0;
            }
        }
    }
    if (mosH > 1)
        for (int px = 0; px < GBA_W; px++) out[px] = out[px - px % mosH];
}

static const u8 sObjW[4][4] = { {8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}, {8, 8, 8, 8} };
static const u8 sObjH[4][4] = { {8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}, {8, 8, 8, 8} };

static void RenderObj(int y)
{
    const u16 *oam = (const u16 *)gGbaOam;
    const u8 *chr = gGbaVram + 0x10000;
    int map1d = R16(0) & 0x40;

    memset(sObj, 0, sizeof sObj);
    memset(sObjPrio, 0xFF, sizeof sObjPrio);
    memset(sObjSemi, 0, sizeof sObjSemi);

    for (int i = 0; i < 128; i++) {
        u16 a0 = oam[i * 4], a1 = oam[i * 4 + 1], a2 = oam[i * 4 + 2];
        int affine = a0 & 0x100;
        if (!affine && (a0 & 0x200)) continue;        // hidden
        int mode = (a0 >> 10) & 3;
        if (mode == 2) continue;                      // OBJ window: unused here
        int shape = a0 >> 14, size = a1 >> 14;
        int w = sObjW[shape][size], h = sObjH[shape][size];
        int bw = w, bh = h;
        if (affine && (a0 & 0x200)) { bw *= 2; bh *= 2; }
        int oy = a0 & 0xFF;
        if (oy >= 160) oy -= 256;
        int ly = y - oy;
        if (ly < 0 || ly >= bh) continue;
        int ox = a1 & 0x1FF;
        if (ox >= 240) ox -= 512;
        if (ox + bw <= 0) continue;

        u32 tile = a2 & 0x3FF;
        u8 prio = (a2 >> 10) & 3;
        int bpp8 = a0 & 0x2000;
        u16 palbase = bpp8 ? 0x100 : (0x100 | ((a2 >> 12) << 4));
        int rowTiles = map1d ? (w >> 3) : 32;
        int tstep = bpp8 ? 2 : 1;                     // 8bpp tiles take two slots

        s32 pa = 0x100, pb = 0, pc = 0, pd = 0x100;
        if (affine) {
            int m = (a1 >> 9) & 0x1F;
            pa = (s16)oam[m * 16 + 3]; pb = (s16)oam[m * 16 + 7];
            pc = (s16)oam[m * 16 + 11]; pd = (s16)oam[m * 16 + 15];
        }
        int hflip = !affine && (a1 & 0x1000), vflip = !affine && (a1 & 0x2000);

        int x0 = ox < 0 ? 0 : ox;
        int x1 = ox + bw > GBA_W ? GBA_W : ox + bw;
        for (int px = x0; px < x1; px++) {
            if (sObjPrio[px] <= prio) continue;
            int tx, ty;
            if (affine) {
                int dx = px - ox - bw / 2, dy = ly - bh / 2;
                tx = ((pa * dx + pb * dy) >> 8) + w / 2;
                ty = ((pc * dx + pd * dy) >> 8) + h / 2;
                if ((unsigned)tx >= (unsigned)w || (unsigned)ty >= (unsigned)h) continue;
            } else {
                tx = px - ox; ty = ly;
                if (hflip) tx = w - 1 - tx;
                if (vflip) ty = h - 1 - ty;
            }
            u32 t = (tile + (ty >> 3) * rowTiles * (map1d ? tstep : 1) + (tx >> 3) * tstep) & 0x3FF;
            u32 c;
            if (bpp8) {
                c = chr[(t * 32 + (ty & 7) * 8 + (tx & 7)) & 0x7FFF];
            } else {
                u8 b = chr[t * 32 + (ty & 7) * 4 + ((tx & 7) >> 1)];
                c = (tx & 1) ? b >> 4 : b & 0xF;
            }
            if (!c) continue;
            sObj[px] = palbase + c;
            sObjPrio[px] = prio;
            sObjSemi[px] = mode == 1;
        }
    }
}

static inline u16 Blend(u16 a, u16 b, int eva, int evb)
{
    int r = ((a & 0x1F) * eva + (b & 0x1F) * evb) >> 4;
    int g = (((a >> 5) & 0x1F) * eva + ((b >> 5) & 0x1F) * evb) >> 4;
    int bl = (((a >> 10) & 0x1F) * eva + ((b >> 10) & 0x1F) * evb) >> 4;
    if (r > 31) r = 31;
    if (g > 31) g = 31;
    if (bl > 31) bl = 31;
    return r | (g << 5) | (bl << 10);
}

static inline u16 Fade(u16 a, int evy, int up)
{
    int r = a & 0x1F, g = (a >> 5) & 0x1F, b = (a >> 10) & 0x1F;
    if (up) { r += ((31 - r) * evy) >> 4; g += ((31 - g) * evy) >> 4; b += ((31 - b) * evy) >> 4; }
    else { r -= (r * evy) >> 4; g -= (g * evy) >> 4; b -= (b * evy) >> 4; }
    return r | (g << 5) | (b << 10);
}

static void PpuRenderLineSlow(int y, u16 *dst)
{
    u16 dispcnt = R16(0);
    if (dispcnt & 0x80) {                              // forced blank shows white
        for (int x = 0; x < GBA_W; x++) dst[x] = 0x7FFF;
        return;
    }

    // Back-to-front order: for each priority, sprites sit above backgrounds,
    // and a lower-numbered background sits above a higher one.
    int order[4], nbg = 0;
    for (int p = 0; p < 4; p++)
        for (int n = 0; n < 4; n++)
            if ((dispcnt & (0x100 << n)) && (R16(0x08 + n * 2) & 3) == p) order[nbg++] = n;
    u8 bgPrio[4];
    for (int i = 0; i < nbg; i++) { bgPrio[i] = R16(0x08 + order[i] * 2) & 3; RenderBg(order[i], y); }

    int objOn = dispcnt & 0x1000;
    if (objOn) RenderObj(y);

    u16 bld = R16(0x50);
    int effect = (bld >> 6) & 3;
    int eva = R16(0x52) & 0x1F, evb = (R16(0x52) >> 8) & 0x1F, evy = R16(0x54) & 0x1F;
    if (eva > 16) eva = 16;
    if (evb > 16) evb = 16;
    if (evy > 16) evy = 16;
    u16 backdrop = Pal(0);

    for (int x = 0; x < GBA_W; x++) {
        u16 topC = 0, botC = 0;
        int topL = -1, botL = -1;
        int objP = objOn ? sObjPrio[x] : 0xFF;
        int objDone = objP == 0xFF;
        for (int i = 0; i <= nbg && botL < 0; i++) {
            int p = i < nbg ? bgPrio[i] : 4;
            if (!objDone && objP <= p) {
                objDone = 1;
                if (topL < 0) { topL = L_OBJ; topC = Pal(sObj[x]); }
                else { botL = L_OBJ; botC = Pal(sObj[x]); break; }
            }
            if (i < nbg) {
                u16 c = sBg[order[i]][x];
                if (c) {
                    if (topL < 0) { topL = order[i]; topC = Pal(c); }
                    else { botL = order[i]; botC = Pal(c); }
                }
            }
        }
        if (topL < 0) { topL = L_BD; topC = backdrop; }
        else if (botL < 0) { botL = L_BD; botC = backdrop; }

        u16 c = topC;
        int t1 = bld & (1 << topL);
        int t2 = botL >= 0 && (bld & (0x100 << botL));
        if (topL == L_OBJ && sObjSemi[x] && t2) c = Blend(topC, botC, eva, evb);
        else if (effect == 1) { if (t1 && t2) c = Blend(topC, botC, eva, evb); }
        else if (effect == 2) { if (t1) c = Fade(topC, evy, 1); }
        else if (effect == 3) { if (t1) c = Fade(topC, evy, 0); }
        dst[x] = c;
    }
}


// ======================================================================
// Fast renderer
// ======================================================================
u8 gPpuForceSlow;                      // test switch: always use the general renderer

static u16 sPal565[512];
static u16 sPalShadow[512];
static u16 sOamShadow[512];
static int sCacheValid;

static Spr sSpr[128];
static u8 sSprLive[128];               // OAM slot -> index in sSpr, or 0xFF
static u32 sLineMask[GBA_H][4];        // sprites touching each line, by OAM slot
static u8 sLineSemi[GBA_H];
static int sNumSpr;

static inline u16 To565(u16 c)
{
    u32 r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    return (u16)((r << 11) | (((g << 1) | (g >> 4)) << 5) | b);
}

static int sBandPalDirty = 1;          // the palette changed since the band picture was drawn
static u32 sPalChg[8];                 // background palette entries whose colour changed since the cached layers last looked

// Reads the palette again and notes which background entries now show a different colour.
static void UpdatePal565(void)
{
    const u16 *pl = (const u16 *)gGbaPltt;
    for (int i = 0; i < 512; i++) {
        u16 c = To565(pl[i] & 0x7FFF);
        if (i < 256 && c != sPal565[i]) sPalChg[i >> 5] |= 1u << (i & 31);
        sPal565[i] = c;
    }
    memcpy(sPalShadow, gGbaPltt, sizeof sPalShadow);
}

static void RebuildSprites(void)
{
    const u16 *oam = (const u16 *)gGbaOam;
    memset(sLineMask, 0, sizeof sLineMask);
    memset(sLineSemi, 0, sizeof sLineSemi);
    memset(sSprLive, 0xFF, sizeof sSprLive);
    sNumSpr = 0;
    for (int i = 0; i < 128; i++) {
        u16 a0 = oam[i * 4], a1 = oam[i * 4 + 1], a2 = oam[i * 4 + 2];
        int affine = a0 & 0x100;
        if (!affine && (a0 & 0x200)) continue;
        int mode = (a0 >> 10) & 3;
        if (mode == 2) continue;
        int shape = a0 >> 14, size = a1 >> 14;
        int w = sObjW[shape][size], h = sObjH[shape][size];
        int bw = w, bh = h;
        if (affine && (a0 & 0x200)) { bw *= 2; bh *= 2; }
        int oy = a0 & 0xFF; if (oy >= 160) oy -= 256;
        int ox = a1 & 0x1FF; if (ox >= 240) ox -= 512;
        if (ox + bw <= 0 || ox >= GBA_W || oy + bh <= 0 || oy >= GBA_H) continue;
        int bpp8 = a0 & 0x2000;
        Spr *sp = &sSpr[sNumSpr];
        sp->x = ox; sp->y = oy; sp->ui = 0; sp->w = w; sp->h = h;
        sp->prio = (a2 >> 10) & 3;
        sp->flags = (affine ? SF_AFFINE : 0) | ((affine && (a0 & 0x200)) ? SF_DOUBLE : 0) |
                    (!affine && (a1 & 0x1000) ? SF_H : 0) | (!affine && (a1 & 0x2000) ? SF_V : 0) |
                    (bpp8 ? SF_8BPP : 0) | (mode == 1 ? SF_SEMI : 0);
        sp->aff = (a1 >> 9) & 0x1F;
        sp->tile = a2 & 0x3FF;
        sp->palbase = bpp8 ? 0x100 : (0x100 | ((a2 >> 12) << 4));
        int y0 = oy < 0 ? 0 : oy, y1 = oy + bh > GBA_H ? GBA_H : oy + bh;
        for (int y = y0; y < y1; y++) {
            sLineMask[y][i >> 5] |= 1u << (i & 31);
            if (mode == 1) sLineSemi[y] = 1;
        }
        sSprLive[i] = sNumSpr++;
    }
}

static inline u32 SwapNibbles(u32 b)
{
    b = __builtin_bswap32(b);
    return ((b >> 4) & 0x0F0F0F0Fu) | ((b & 0x0F0F0F0Fu) << 4);
}

// Where painted pixels go. A plain line keeps one colour per pixel. A line
// that blends keeps the top two visible layers per pixel, which is all the
// blending hardware looks at, and resolves them at the end.
typedef struct {
    u16 *lb;
    u16 topC[GBA_W + 16], botC[GBA_W + 16];
    u8 topL[GBA_W + 16], botL[GBA_W + 16];
} Ctx;
enum { LY_BG0, LY_BG1, LY_BG2, LY_BG3, LY_OBJ, LY_BD, LY_SEMI, LY_NONE = 0xFF };

#define INL static inline __attribute__((always_inline))

INL void Px(Ctx *c, int x, u16 col, int layer, int track)
{
    if (!track) { c->lb[x] = col; return; }
    u8 tl = c->topL[x];
    if (!((layer == LY_OBJ || layer == LY_SEMI) && (tl == LY_OBJ || tl == LY_SEMI))) {
        c->botC[x] = c->topC[x];
        c->botL[x] = tl;
    }
    c->topC[x] = col;
    c->topL[x] = (u8)layer;
}

// Paints one 8-pixel row of a 4-bit tile; colour 0 is transparent.
INL void Put4(Ctx *c, int x, u32 bits, const u16 *pal, int layer, int track)
{
    if (!bits) return;
    u32 v;
    if ((v = bits & 15)) Px(c, x, pal[v], layer, track);
    if ((v = (bits >> 4) & 15)) Px(c, x + 1, pal[v], layer, track);
    if ((v = (bits >> 8) & 15)) Px(c, x + 2, pal[v], layer, track);
    if ((v = (bits >> 12) & 15)) Px(c, x + 3, pal[v], layer, track);
    if ((v = (bits >> 16) & 15)) Px(c, x + 4, pal[v], layer, track);
    if ((v = (bits >> 20) & 15)) Px(c, x + 5, pal[v], layer, track);
    if ((v = (bits >> 24) & 15)) Px(c, x + 6, pal[v], layer, track);
    if ((v = bits >> 28)) Px(c, x + 7, pal[v], layer, track);
}

INL void FastBg(Ctx *c, int n, int y, int track)
{
    u16 cnt = R16(0x08 + n * 2);
    u32 hofs = R16(0x10 + n * 4) & 0x1FF, vofs = R16(0x12 + n * 4) & 0x1FF;
    u32 chrBase = ((cnt >> 2) & 3) * 0x4000;
    const u16 *map = (const u16 *)(gGbaVram + ((cnt >> 8) & 0x1F) * 0x800);
    int size = cnt >> 14, bpp8 = cnt & 0x80;
    u32 wmask = (size & 1) ? 0x1FF : 0xFF, hmask = (size & 2) ? 0x1FF : 0xFF;

    u32 sy = (y + vofs) & hmask;
    const u16 *row = map;
    if (sy >= 256) row += (size == 3) ? 0x800 : 0x400;
    row += ((sy & 0xFF) >> 3) * 32;
    u32 ty = sy & 7;
    u32 sx = hofs & wmask;
    int x = -(int)(sx & 7);
    sx &= ~7u;
    for (; x < GBA_W; x += 8, sx = (sx + 8) & wmask) {
        u16 e = row[((sx & 0xFF) >> 3) + ((sx >= 256) ? 0x400 : 0)];
        u32 line = (e & 0x800) ? 7 - ty : ty;
        u32 tile = e & 0x3FF;
        if (bpp8) {
            u32 off = chrBase + tile * 64 + line * 8;
            if (off + 8 > 0x10000) continue;
            const u8 *p = gGbaVram + off;
            for (int i = 0; i < 8; i++) {
                u8 v = p[(e & 0x400) ? 7 - i : i];
                if (v) Px(c, 8 + x + i, sPal565[v], n, track);
            }
        } else {
            u32 off = chrBase + tile * 32 + line * 4;
            if (off + 4 > 0x10000) continue;
            u32 bits = *(const u32 *)(gGbaVram + off);
            if (e & 0x400) bits = SwapNibbles(bits);
            Put4(c, 8 + x, bits, sPal565 + ((e >> 12) << 4), n, track);
        }
    }
}

static __thread int sEyeDX[4];                  // pixels to move BG1, BG2, BG3 and the world sprites in the eye being drawn
static __thread int sObjDX;

INL void FastObj(Ctx *c, const Spr *sp, int y, int track)
{
    const u8 *chr = gGbaVram + 0x10000;
    int map1d = R16(0) & 0x40;
    int w = sp->w, h = sp->h, bw = w, bh = h;
    if (sp->flags & SF_DOUBLE) { bw *= 2; bh *= 2; }
    int oy = sp->y;
    int ly = y - oy, ox = sp->x + sObjDX;
    int bpp8 = sp->flags & SF_8BPP;
    int tstep = bpp8 ? 2 : 1;
    int rowTiles = map1d ? (w >> 3) : 32;
    const u16 *pal = sPal565 + sp->palbase;
    int layer = (sp->flags & SF_SEMI) ? LY_SEMI : LY_OBJ;

    if (sp->flags & SF_AFFINE) {
        const u16 *oam = (const u16 *)gGbaOam;
        int m = sp->aff;
        s32 pa = (s16)oam[m * 16 + 3], pb = (s16)oam[m * 16 + 7], pc = (s16)oam[m * 16 + 11], pd = (s16)oam[m * 16 + 15];
        int x0 = ox < 0 ? 0 : ox, x1 = ox + bw > GBA_W ? GBA_W : ox + bw;
        for (int px = x0; px < x1; px++) {
            int dx = px - ox - bw / 2, dy = ly - bh / 2;
            int tx = ((pa * dx + pb * dy) >> 8) + w / 2, ty = ((pc * dx + pd * dy) >> 8) + h / 2;
            if ((unsigned)tx >= (unsigned)w || (unsigned)ty >= (unsigned)h) continue;
            u32 t = (sp->tile + (ty >> 3) * rowTiles * (map1d ? tstep : 1) + (tx >> 3) * tstep) & 0x3FF;
            u32 v;
            if (bpp8) v = chr[(t * 32 + (ty & 7) * 8 + (tx & 7)) & 0x7FFF];
            else { u8 b = chr[t * 32 + (ty & 7) * 4 + ((tx & 7) >> 1)]; v = (tx & 1) ? b >> 4 : b & 0xF; }
            if (v) Px(c, 8 + px, pal[v], layer, track);
        }
        return;
    }

    int ty = (sp->flags & SF_V) ? h - 1 - ly : ly;
    int hf = sp->flags & SF_H;
    int cols = w >> 3;
    for (int tc = 0; tc < cols; tc++) {
        int x = ox + tc * 8;
        if (x + 8 <= 0 || x >= GBA_W) continue;
        int col = hf ? cols - 1 - tc : tc;
        u32 t = (sp->tile + (ty >> 3) * rowTiles * (map1d ? tstep : 1) + col * tstep) & 0x3FF;
        if (bpp8) {
            const u8 *p = chr + ((t * 32 + (ty & 7) * 8) & 0x7FFF);
            for (int i = 0; i < 8; i++) {
                u8 v = p[hf ? 7 - i : i];
                if (v) Px(c, 8 + x + i, pal[v], layer, track);
            }
        } else {
            u32 bits = *(const u32 *)(chr + t * 32 + (ty & 7) * 4);
            if (hf) bits = SwapNibbles(bits);
            Put4(c, 8 + x, bits, pal, layer, track);
        }
    }
}

INL void PaintLine(Ctx *c, int y, int dispcnt, int track)
{
    int bgOn[4], bgPrio[4];
    for (int n = 0; n < 4; n++) {
        bgOn[n] = (dispcnt & (0x100 << n)) != 0;
        bgPrio[n] = R16(0x08 + n * 2) & 3;
    }
    int objOn = (dispcnt & 0x1000) != 0;
    const u32 *mask = sLineMask[y];
    for (int p = 3; p >= 0; p--) {
        for (int n = 3; n >= 0; n--)
            if (bgOn[n] && bgPrio[n] == p) FastBg(c, n, y, track);
        if (!objOn) continue;
        for (int w = 3; w >= 0; w--) {
            u32 m = mask[w];
            while (m) {                                 // highest slot first: lower slots end up on top
                int b = 31 - __builtin_clz(m);
                m &= ~(1u << b);
                const Spr *sp = &sSpr[sSprLive[w * 32 + b]];
                if (sp->prio == p) FastObj(c, sp, y, track);
            }
        }
    }
}

static inline void Split(u16 c, int *r, int *g, int *b) { *r = c >> 11; *g = (c >> 6) & 31; *b = c & 31; }
static inline u16 Join(int r, int g, int b) { return (u16)((r << 11) | (((g << 1) | (g >> 4)) << 5) | b); }

static __thread Ctx sCtx;

// The game's own code only runs at VBlank, so palette and OAM can change inside the visible frame only through a VCount or
// HBlank interrupt handler or an HBlank DMA. Without one of those they are compared with their shadow copies once per frame
// (line 0) instead of on all 160 lines (about a third of the menu screens' time).
static int sLineWatch;

void PpuRenderLine(int y, u16 *dst)
{
    sIo = gGbaIo;
    u16 dispcnt = R16(0);
    if (y == 0) {
        sLineWatch = (R16(4) & 0x30) != 0;                                   // VCount or HBlank interrupt enabled
        for (int ch = 0; ch < 4; ch++) {
            u16 cnt = R16(0xBA + 12 * ch);
            if ((cnt & 0x8000) && ((cnt >> 12) & 3) == 2) sLineWatch = 1;    // DMA that starts at HBlank
        }
    }
    if (!sCacheValid || y == 0 || sLineWatch) {
        if (!sCacheValid || memcmp(sPalShadow, gGbaPltt, sizeof sPalShadow)) UpdatePal565();
        if (!sCacheValid || memcmp(sOamShadow, gGbaOam, sizeof sOamShadow)) {
            RebuildSprites();
            memcpy(sOamShadow, gGbaOam, sizeof sOamShadow);
        }
        sCacheValid = 1;
    }
    if (dispcnt & 0x80) {                              // forced blank shows white
        for (int x = 0; x < GBA_W; x++) dst[x] = 0xFFFF;
        return;
    }

    u16 bld = R16(0x50);
    int effect = (bld >> 6) & 3, tgt1 = bld & 0x3F, tgt2 = (bld >> 8) & 0x3F;
    // Mosaic only does anything with a non-zero size; the game sets the BG
    // flag far more often than that.
    int mosaicBg = 0;
    if (R16(0x4C) & 0xFF)
        for (int n = 0; n < 4; n++) if ((dispcnt & (0x100 << n)) && (R16(0x08 + n * 2) & 0x40)) mosaicBg = 1;
    if (gPpuForceSlow || mosaicBg) {
        u16 tmp[GBA_W];
        PpuRenderLineSlow(y, tmp);
        for (int x = 0; x < GBA_W; x++) dst[x] = To565(tmp[x]);
        return;
    }

    Ctx *c = &sCtx;
    u16 back = sPal565[0];
    // Does anything on this line blend? Semi-transparent sprites need a second target; alpha
    // needs both; fades need a first target. Only a fade of everything is cheap.
    int semiActive = sLineSemi[y] && tgt2;
    int blends = semiActive || (effect == 1 && tgt1 && tgt2) || ((effect & 2) && tgt1 && tgt1 != 0x3F);
    int fade = (effect & 2) && tgt1 == 0x3F;

    u16 plain[GBA_W + 16];
    if (!blends) {
        c->lb = plain;
        u32 b2 = back * 0x10001u;
        for (int i = 0; i < (GBA_W + 16) / 2; i++) ((u32 *)plain)[i] = b2;
        PaintLine(c, y, dispcnt, 0);
        u16 *lb = plain + 8;
        if (fade) {
            int evy = R16(0x54) & 0x1F;
            if (evy > 16) evy = 16;
            if (evy) {
                int up = effect == 2;
                u16 fr[32], fg[32], fb[32];
                for (int v = 0; v < 32; v++) {
                    int o = up ? v + (((31 - v) * evy) >> 4) : v - ((v * evy) >> 4);
                    fr[v] = (u16)(o << 11); fb[v] = (u16)o;
                    fg[v] = (u16)((((o << 1) | (o >> 4))) << 5);
                }
                for (int x = 0; x < GBA_W; x++) {
                    u16 px = lb[x];
                    lb[x] = fr[px >> 11] | fg[(px >> 6) & 31] | fb[px & 31];
                }
            }
        }
        memcpy(dst, lb, GBA_W * 2);
        return;
    }

    for (int i = 0; i < GBA_W + 16; i++) { c->topC[i] = back; c->topL[i] = LY_BD; c->botL[i] = LY_NONE; }
    PaintLine(c, y, dispcnt, 1);
    int eva = R16(0x52) & 0x1F, evb = (R16(0x52) >> 8) & 0x1F, evy = R16(0x54) & 0x1F;
    if (eva > 16) eva = 16;
    if (evb > 16) evb = 16;
    if (evy > 16) evy = 16;
    for (int x = 0; x < GBA_W; x++) {
        int i = x + 8;
        u8 tl = c->topL[i], bl = c->botL[i];
        int semi = tl == LY_SEMI;
        if (semi) tl = LY_OBJ;
        int t1 = tgt1 & (1 << tl);
        int t2 = bl != LY_NONE && (tgt2 & (1 << bl));
        u16 col = c->topC[i];
        int blend = 0, fadeIt = 0;
        if (semi && t2) blend = 1;
        else if (effect == 1) blend = t1 && t2;
        else if (effect >= 2) fadeIt = t1;
        if (blend || fadeIt) {
            int r, g, b;
            Split(col, &r, &g, &b);
            if (blend) {
                int r2, g2, b2;
                Split(c->botC[i], &r2, &g2, &b2);
                r = (r * eva + r2 * evb) >> 4; g = (g * eva + g2 * evb) >> 4; b = (b * eva + b2 * evb) >> 4;
                if (r > 31) r = 31;
                if (g > 31) g = 31;
                if (b > 31) b = 31;
            } else if (effect == 2) {
                r += ((31 - r) * evy) >> 4; g += ((31 - g) * evy) >> 4; b += ((31 - b) * evy) >> 4;
            } else {
                r -= (r * evy) >> 4; g -= (g * evy) >> 4; b -= (b * evy) >> 4;
            }
            col = Join(r, g, b);
        }
        dst[x] = col;
    }
}


// ======================================================================
// Tall table renderer
//
// The picture is GBA_W x TALL_H: world layers (BG1-BG3 and the sprites that
// belong to the table) cover every row from the top of the board down; the
// fixed layer (BG0: score and pop-ups) and the sprites that sit on screen
// (menus) fill the original 160-row picture, placed at the bottom. The
// game runs a normal frame while this is drawn afterwards from the
// registers each scanline saw, so its mid-frame register changes still work.
// ======================================================================
static Spr sTSpr[PORT_OAM_N];
static u16 sTRow[TALL_H][PORT_OAM_N];   // per tall row: the sprites (indices into sTSpr, lowest OAM slot first) that touch it
static u16 sTRowN[TALL_H];
static u8 sTSemi[TALL_H];
static unsigned sStSlow, sStRebuilt, sStSprites, sStFrames;   // diagnostics for the perf log
static u16 sStBld, sStDisp;
static u32 sStMain, sStWait;           // microseconds: this core's share of the drawing, and the wait for the other core
static int sUi0 = TALL_BAND;            // first tall row of the fixed-on-screen band, set per frame by the layout
static u8 sLineRegs[GBA_H][0x58];      // DISPCNT .. BLDY as each scanline saw them

extern u8 gBoardBGTileBuffer[], gBoardBGTileBufferAlt[];   // the board picture, 32 tile rows each

void PpuLatchLine(int y) { memcpy(sLineRegs[y], gGbaIo, sizeof sLineRegs[y]); }

static int sNTSpr;                      // sprites in sTSpr this frame
static void RebuildSpritesTall(void)
{
    const u16 *oam = (const u16 *)gOamSnap;
    memset(sTRowN, 0, sizeof sTRowN);
    memset(sTSemi, 0, sizeof sTSemi);
    int n = 0;
    for (int i = 0; i < PORT_OAM_N; i++) {
        u16 a0 = oam[i * 4], a1 = oam[i * 4 + 1], a2 = oam[i * 4 + 2];
        int affine = a0 & 0x100;
        if (!affine && (a0 & 0x200)) continue;
        int mode = (a0 >> 10) & 3;
        if (mode == 2) continue;
        int shape = a0 >> 14, size = a1 >> 14;
        int w = sObjW[shape][size], h = sObjH[shape][size];
        int bw = w, bh = h;
        if (affine && (a0 & 0x200)) { bw *= 2; bh *= 2; }
        int ui = 1, oy, y8 = a0 & 0xFF;
        if (gOamYWvSnap[i] && gOamYWSnap[i] >= PORT_WB / 2) { ui = 0; oy = gOamYWSnap[i] - PORT_WB; }
        else oy = y8 >= 160 ? y8 - 256 : y8;
        int ox = a1 & 0x1FF; if (ox >= 240) ox -= 512;
        if (ox + bw <= 0 || ox >= GBA_W) continue;
        int base = ui ? sUi0 : 0, limit = ui ? GBA_H : TALL_H;
        int y0 = oy < 0 ? 0 : oy, y1 = oy + bh > limit ? limit : oy + bh;
        if (y0 >= y1) continue;
        int bpp8 = a0 & 0x2000;
        Spr *sp = &sTSpr[n];
        sp->x = ox; sp->y = oy; sp->ui = ui; sp->w = w; sp->h = h;
        sp->prio = (a2 >> 10) & 3;
        sp->flags = (affine ? SF_AFFINE : 0) | ((affine && (a0 & 0x200)) ? SF_DOUBLE : 0) |
                    (!affine && (a1 & 0x1000) ? SF_H : 0) | (!affine && (a1 & 0x2000) ? SF_V : 0) |
                    (bpp8 ? SF_8BPP : 0) | (mode == 1 ? SF_SEMI : 0);
        sp->aff = (a1 >> 9) & 0x1F;
        sp->tile = a2 & 0x3FF;
        sp->palbase = bpp8 ? 0x100 : (0x100 | ((a2 >> 12) << 4));
        for (int y = y0; y < y1; y++) {
            sTRow[base + y][sTRowN[base + y]++] = (u16)n;
            if (mode == 1) sTSemi[base + y] = 1;
        }
        n++;
    }
    sNTSpr = n;
}

// One text background row. world: scrolled by row alone (the camera is gone);
// otherwise as the hardware would, at that scanline's scroll.
INL void FastBgT(Ctx *c, int n, int y, int track, int world)
{
    u16 cnt = R16(0x08 + n * 2);
    u32 hofs = R16(0x10 + n * 4) & 0x1FF, vofs = R16(0x12 + n * 4) & 0x1FF;
    u32 chrBase = ((cnt >> 2) & 3) * 0x4000;
    const u16 *map = (const u16 *)(gGbaVram + ((cnt >> 8) & 0x1F) * 0x800);
    int size = cnt >> 14, bpp8 = cnt & 0x80;
    u32 wmask = (size & 1) ? 0x1FF : 0xFF, hmask = (size & 2) ? 0x1FF : 0xFF;

    u32 sy = world ? ((u32)y & hmask) : ((y + vofs) & hmask);
    const u16 *row = map;
    if (sy >= 256) row += (size == 3) ? 0x800 : 0x400;
    row += ((sy & 0xFF) >> 3) * 32;
    u32 ty = sy & 7, trow = sy >> 3;
    int board = world && n == 3;       // the board picture is streamed through a small window in video memory
    u32 sx = hofs & wmask;
    int x = -(int)(sx & 7);
    sx &= ~7u;
    for (; x < GBA_W; x += 8, sx = (sx + 8) & wmask) {
        u32 col = (sx & 0xFF) >> 3;
        u16 e = row[col + ((sx >= 256) ? 0x400 : 0)];
        u32 line = (e & 0x800) ? 7 - ty : ty;
        u32 tile = e & 0x3FF;
        if (bpp8) {
            u32 off = chrBase + tile * 64 + line * 8;
            if (off + 8 > 0x10000) continue;
            const u8 *p = gGbaVram + off;
            for (int i = 0; i < 8; i++) {
                u8 v = p[(e & 0x400) ? 7 - i : i];
                if (v) Px(c, 8 + x + i, sPal565[v], n, track);
            }
        } else {
            const u8 *src;
            if (board && tile < 704) {
                // Streamed tile: window slot (row + 10) % 22 holds board row `row`; take it from the full picture.
                if ((tile >> 5) != (trow + 10) % 22 || (tile & 31) != col) continue;
                src = (trow < 32 ? gBoardBGTileBuffer + trow * 0x400 : gBoardBGTileBufferAlt + (trow - 32) * 0x400) + col * 32 + line * 4;
            } else {
                u32 off = chrBase + tile * 32 + line * 4;
                if (off + 4 > 0x10000) continue;
                src = gGbaVram + off;
            }
            u32 bits;
            memcpy(&bits, src, 4);
            if (e & 0x400) bits = SwapNibbles(bits);
            Put4(c, 8 + x, bits, sPal565 + ((e >> 12) << 4), n, track);
        }
    }
}

// ---- Board lights ----------------------------------------------------------
// The game lights its lamps, arrows and letters by copying small tile pictures
// into the board's streaming window in video memory. That window holds only 22
// of the board's tile rows (a row's slot is shared by rows 22 apart), but the
// tall view draws the whole board from the picture buffers, so those copies
// would never be seen. Every such copy is therefore noted here, assigned to the
// board row whose own picture it resembles most, and used in place of the
// buffer's tile when the layer is drawn.
#define LIGHT_ROWS 64
static struct { u8 on; u8 data[32]; } sLight[LIGHT_ROWS][32];
static s8 sSlotRow[22] = { -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1 };   // board row the streamer last copied into each window slot (exact)
static unsigned sStLampExact, sStLampGuess, sStLampDrop;   // diagnostics for the perf log
static s8 sLightRow[704];              // per window tile: the board row it was assigned to (-1: none yet)
static u32 sLightVer;
static u8 sLightDirty[LIGHT_ROWS];     // board tile rows whose lamp pictures changed since the layer was last drawn
static int sLightFull = 1;             // the whole layer has to be drawn again (after a reset)

void PpuLightReset(void)
{
    memset(sLight, 0, sizeof sLight);
    memset(sLightRow, -1, sizeof sLightRow);
    sLightVer++;
    sLightFull = 1;
}

static const u8 *BoardTile(int trow, int col)
{
    return (trow < 32 ? gBoardBGTileBuffer + trow * 0x400 : gBoardBGTileBufferAlt + (trow - 32) * 0x400) + col * 32;
}

void PpuNoteTileWrite(const void *dest, const void *src, u32 bytes)
{
    static int first = 1;
    if (first) { memset(sLightRow, -1, sizeof sLightRow); first = 0; }
    const u8 *d = dest, *sr = src;
    if (d < gGbaVram + 0x8000 || d >= gGbaVram + 0x8000 + 704 * 32 || bytes > 0x400) return;
    if ((sr >= gBoardBGTileBuffer && sr < gBoardBGTileBuffer + 0x8000) ||
        (sr >= gBoardBGTileBufferAlt && sr < gBoardBGTileBufferAlt + 0x8000)) {          // the streamer itself
        u32 off = (u32)(d - (gGbaVram + 0x8000));
        if (bytes == 0x400 && (off & 0x3FF) == 0 && off / 0x400 < 22) {
            int alt = sr >= gBoardBGTileBufferAlt && sr < gBoardBGTileBufferAlt + 0x8000;
            const u8 *base = alt ? gBoardBGTileBufferAlt : gBoardBGTileBuffer;
            sSlotRow[off / 0x400] = (s8)((u32)(sr - base) / 0x400 + (alt ? 32 : 0));
        }
        return;
    }
    u32 t0 = (u32)(d - (gGbaVram + 0x8000)) / 32;
    for (u32 i = 0; i * 32 < bytes && t0 + i < 704; i++) {
        u32 t = t0 + i, slot = t >> 5, col = t & 31;
        const u8 *w = d + i * 32;
        int row = sSlotRow[slot];            // exact: the row the streamer put in this slot
        if (row >= 0) {
            // still require the picture to resemble that board tile: not every tile write here is a lamp
            const u8 *b = BoardTile(row, col);
            int eq = 0;
            for (int k = 0; k < 32; k++) eq += b[k] == w[k];
            if (eq <= 15) { sStLampDrop++; continue; }
            sStLampExact++;
        } else row = sLightRow[t];
        if (row < 0) {
            int best = -1, bestEq = 15;
            for (int r = ((int)slot + 12) % 22; r < LIGHT_ROWS; r += 22) {
                const u8 *b = BoardTile(r, col);
                int eq = 0;
                for (int k = 0; k < 32; k++) eq += b[k] == w[k];
                if (eq > bestEq) { bestEq = eq; best = r; }
            }
            if (best < 0) { sStLampDrop++; continue; }
            sStLampGuess++;
            sLightRow[t] = (s8)(row = best);
        }
        if (!sLight[row][col].on || memcmp(sLight[row][col].data, w, 32)) {
            memcpy(sLight[row][col].data, w, 32);
            sLight[row][col].on = 1;
            sLightVer++;
            if (row < LIGHT_ROWS) sLightDirty[row] = 1;
        }
    }
}

// ---- Cached world layers --------------------------------------------------
// BG1-BG3 hold the board itself; they change a few times a minute (palette
// aside), so each is kept as one tall image of palette indices and only
// rebuilt when its map or tile pictures change. The image is stored without the
// scroll (a row is the whole map row, followed by a copy of its first GBA_W
// pixels so the 240-pixel window never has to wrap), so the in-game shake that
// moves BG1-BG3 sideways costs nothing. Next to the indices each row keeps the
// finished RGB565 pixels and the spans of non-empty pixels, so drawing a row is
// a few copies instead of a palette lookup per pixel.
#define LSTRIDE (512 + GBA_W)          // widest map row plus the wrap-around copy
#define LMAXRUNS 16                    // spans kept per row; more than that and the row is drawn pixel by pixel
typedef struct {
    int valid;
    u16 cnt;
    u16 scroll;                        // hofs & width mask of the frame being drawn
    u32 lightVer;
    u8 nruns[TALL_H];                  // number of spans in the row, 0xFF: too many
    u16 runs[TALL_H][LMAXRUNS][2];     // [start, end) of each run of non-empty pixels in the stored row
    u32 used[TALL_H][8];               // which of the 256 background palette entries the row uses
    u8 map[0x2000];
    u8 chr[0x10000];                   // copy of the tile pictures the layer depends on
    u32 chrLen;
} Layer;
static Layer sLayer[3];
static u8 sLayerIdx[3][TALL_H * LSTRIDE];
static u16 sLayerPix[3][TALL_H * LSTRIDE];
static u8 sRowDirty[3][TALL_H];         // rows of the stored layers that changed since the GPU texture last took them

static void LayerSources(int n, u16 cnt, const u8 **chr0, u32 *len0, const u8 **chr1, u32 *len1)
{
    u32 chrBase = ((cnt >> 2) & 3) * 0x4000;
    if (n == 3) {                       // the board picture, streamed into video memory a few rows at a time
        *chr0 = gBoardBGTileBuffer; *len0 = 0x8000;
        *chr1 = gBoardBGTileBufferAlt; *len1 = 0x8000;
    } else {
        // tiles from 704 up are the fixed ones; the lower tiles of that block are the streaming window
        u32 from = chrBase == 0x8000 ? 0x8000 + 704 * 32 : chrBase;
        u32 to = chrBase == 0x8000 ? 0x10000 : chrBase + 0x4000;
        *chr0 = gGbaVram + from; *len0 = to - from;
        *chr1 = NULL; *len1 = 0;
    }
}

// 0 = unchanged, 1 = draw the whole layer again, 2 = only the lamp pictures changed (see sLightDirty)
static int LayerDirty(int n)
{
    Layer *L = &sLayer[n - 1];
    u16 cnt = R16(0x08 + n * 2);
    u32 maplen = (cnt >> 14) == 3 ? 0x2000 : ((cnt >> 14) ? 0x1000 : 0x800);
    const u8 *map = gGbaVram + ((cnt >> 8) & 0x1F) * 0x800;
    const u8 *c0, *c1; u32 l0, l1;
    LayerSources(n, cnt, &c0, &l0, &c1, &l1);
    int lightChange = n == 3 && L->lightVer != sLightVer;
    int dirty = !L->valid || cnt != L->cnt || (cnt & 0x80) || (lightChange && sLightFull) ||
                memcmp(L->map, map, maplen) || memcmp(L->chr, c0, l0) || (l1 && memcmp(L->chr + l0, c1, l1));
    if (dirty) {
        memcpy(L->map, map, maplen); memcpy(L->chr, c0, l0); if (l1) memcpy(L->chr + l0, c1, l1);
        L->cnt = cnt; L->chrLen = l0 + l1; L->lightVer = sLightVer;
        if (n == 3) { sLightFull = 0; memset(sLightDirty, 0, sizeof sLightDirty); }
        return 1;
    }
    if (lightChange) { L->lightVer = sLightVer; return 2; }
    return 0;
}

// Finishes a stored row: RGB565 pixels, spans of non-empty pixels and the palette entries it uses.
static void FinishLayerRow(Layer *L, int n, int R)
{
    const u8 *idx = sLayerIdx[n - 1] + R * LSTRIDE;
    u16 *pix = sLayerPix[n - 1] + R * LSTRIDE;
    u32 W = (((L->cnt >> 14) & 1) ? 512 : 256) + GBA_W;
    u32 *used = L->used[R];
    u16 (*runs)[2] = L->runs[R];
    int nr = 0, start = -1;
    memset(used, 0, sizeof L->used[R]);
    for (u32 j = 0; j < W; j++) {
        u32 v = idx[j];
        if (v) {
            pix[j] = sPal565[v];
            used[v >> 5] |= 1u << (v & 31);
            if (start < 0) start = (int)j;
        } else if (start >= 0) {
            if (nr < LMAXRUNS) { runs[nr][0] = (u16)start; runs[nr][1] = (u16)j; }
            nr++; start = -1;
        }
    }
    if (start >= 0) {
        if (nr < LMAXRUNS) { runs[nr][0] = (u16)start; runs[nr][1] = (u16)W; }
        nr++;
    }
    L->nruns[R] = nr > LMAXRUNS ? 0xFF : (u8)nr;
}

// New palette colours: draw again the stored rows that use an entry that changed.
static void RefreshLayerColours(Layer *L, int n)
{
    const u8 *idxBase = sLayerIdx[n - 1];
    u16 *pixBase = sLayerPix[n - 1];
    u32 W = (((L->cnt >> 14) & 1) ? 512 : 256) + GBA_W;
    for (int R = 0; R < TALL_H; R++) {
        const u32 *u = L->used[R];
        if (!((u[0] & sPalChg[0]) | (u[1] & sPalChg[1]) | (u[2] & sPalChg[2]) | (u[3] & sPalChg[3]) |
              (u[4] & sPalChg[4]) | (u[5] & sPalChg[5]) | (u[6] & sPalChg[6]) | (u[7] & sPalChg[7]))) continue;
        const u8 *idx = idxBase + R * LSTRIDE;
        u16 *pix = pixBase + R * LSTRIDE;
        for (u32 j = 0; j < W; j++) if (idx[j]) pix[j] = sPal565[idx[j]];
        sRowDirty[n - 1][R] = 1;
    }
}

// One row of a 4-bit text layer as palette indices (0 = nothing), the whole map row without scroll.
static void BuildLayerRow(int n, int R)
{
    sStRebuilt++;
    Layer *L = &sLayer[n - 1];
    u8 *out = sLayerIdx[n - 1] + R * LSTRIDE;
    u16 cnt = R16(0x08 + n * 2);
    u32 chrBase = ((cnt >> 2) & 3) * 0x4000;
    const u16 *map = (const u16 *)(gGbaVram + ((cnt >> 8) & 0x1F) * 0x800);
    int size = cnt >> 14;
    u32 wmask = (size & 1) ? 0x1FF : 0xFF, hmask = (size & 2) ? 0x1FF : 0xFF;
    u32 sy = (u32)R & hmask;
    const u16 *row = map;
    if (sy >= 256) row += (size == 3) ? 0x800 : 0x400;
    row += ((sy & 0xFF) >> 3) * 32;
    u32 ty = sy & 7, trow = sy >> 3;
    int board = n == 3;
    memset(out, 0, (wmask + 1) + GBA_W);
    for (u32 sx = 0; sx <= wmask; sx += 8) {
        u32 col = (sx & 0xFF) >> 3;
        u16 e = row[col + ((sx >= 256) ? 0x400 : 0)];
        u32 ln = (e & 0x800) ? 7 - ty : ty;
        u32 tile = e & 0x3FF;
        const u8 *src;
        if (board && tile < 704) {
            if ((tile >> 5) != (trow + 10) % 22 || (tile & 31) != col) continue;
            src = (trow < 32 ? gBoardBGTileBuffer + trow * 0x400 : gBoardBGTileBufferAlt + (trow - 32) * 0x400) + col * 32 + ln * 4;
            if (trow < LIGHT_ROWS && sLight[trow][col].on) src = sLight[trow][col].data + ln * 4;
        } else {
            u32 off = chrBase + tile * 32 + ln * 4;
            if (off + 4 > 0x10000) continue;
            src = gGbaVram + off;
        }
        u32 bits;
        memcpy(&bits, src, 4);
        if (!bits) continue;
        if (e & 0x400) bits = SwapNibbles(bits);
        u8 pal = (u8)((e >> 12) << 4);
        for (int i = 0; i < 8; i++) {
            u32 v = (bits >> (i * 4)) & 15;
            if (v) out[sx + i] = pal | v;
        }
    }
    memcpy(out + wmask + 1, out, GBA_W);        // the wrap-around copy
    L->cnt = cnt;
    FinishLayerRow(L, n, R);
    sRowDirty[n - 1][R] = 1;
}

static void RefreshLayers(void)
{
    int palChanged = 0;
    for (int i = 0; i < 8; i++) palChanged |= sPalChg[i] != 0;
    for (int n = 1; n <= 3; n++) {
        sIo = sLineRegs[0];
        static int off = -1;
        if (off < 0) off = getenv("PORT_NOCACHE") != NULL;     // test switch: draw the layers directly
        if (off || !(R16(0) & (0x100 << n)) || (R16(0x08 + n * 2) & 0x80)) { sLayer[n - 1].valid = 0; continue; }
        Layer *L = &sLayer[n - 1];
        L->scroll = (u16)(R16(0x10 + n * 4) & (((R16(0x08 + n * 2) >> 14) & 1) ? 0x1FF : 0xFF));
        int d = LayerDirty(n);
        if (d == 1) {
            for (int R = 0; R < TALL_H; R++) BuildLayerRow(n, R);
            L->valid = 1;
        } else if (d == 2) {           // blinking lamps: redraw only the tile rows they sit in
            for (int tr = 0; tr < LIGHT_ROWS; tr++) {
                if (!sLightDirty[tr]) continue;
                sLightDirty[tr] = 0;
                for (int R = tr * 8; R < tr * 8 + 8 && R < TALL_H; R++) BuildLayerRow(n, R);
            }
        }
        if (palChanged) RefreshLayerColours(L, n);
    }
    sBandPalDirty |= palChanged;
    memset(sPalChg, 0, sizeof sPalChg);
    sIo = gGbaIo;
}

// Stereo 3D: each layer is moved sideways per eye by its own amount (positive = toward the viewer).

INL void OverlayLayer(Ctx *c, int n, int R, int track)
{
    const Layer *L = &sLayer[n - 1];
    int s = L->scroll;                          // the 240-pixel window starts s pixels into the stored row
    const u8 *idx = sLayerIdx[n - 1] + R * LSTRIDE + s;
    const u16 *pix = sLayerPix[n - 1] + R * LSTRIDE + s;
    int dx = sEyeDX[n - 1];
    if (!track) {
        u16 *lb = c->lb + 8 + dx;
        u32 nr = L->nruns[R];
        if (nr != 0xFF) {
            const u16 (*runs)[2] = L->runs[R];
            for (u32 k = 0; k < nr; k++) {
                int lo = runs[k][0] - s, hi = runs[k][1] - s;
                if (lo < 0) lo = 0;
                if (hi > GBA_W) hi = GBA_W;
                if (lo < hi) memcpy(lb + lo, pix + lo, (size_t)(hi - lo) * 2);
            }
        } else {
            for (int x = 0; x < GBA_W; x++) if (idx[x]) lb[x] = pix[x];
        }
    } else {
        for (int x = 0; x < GBA_W; x++) { u32 v = idx[x]; if (v) Px(c, 8 + dx + x, sPal565[v], n, 1); }
    }
}

INL void PaintRowT(Ctx *c, int R, int dispcnt, int track)
{
    int bgOn[4], bgPrio[4];
    for (int n = 0; n < 4; n++) {
        bgOn[n] = (dispcnt & (0x100 << n)) != 0;
        bgPrio[n] = R16(0x08 + n * 2) & 3;
    }
    bgOn[0] &= R >= sUi0 && R < sUi0 + 144;   // the fixed layer lives in its band only; its stock score strip (last 16 rows) is left out, the panels show those numbers
    int objOn = (dispcnt & 0x1000) != 0;
    const u16 *list = sTRow[R];
    int nsp = objOn ? sTRowN[R] : 0;
    for (int p = 3; p >= 0; p--) {
        for (int n = 3; n >= 0; n--)
            if (bgOn[n] && bgPrio[n] == p) {
                if (n == 0) FastBgT(c, 0, R - sUi0, track, 0);
                else if (sLayer[n - 1].valid) OverlayLayer(c, n, R, track);
                else FastBgT(c, n, R, track, 1);
            }
        for (int k = nsp - 1; k >= 0; k--) {      // highest OAM slot first: lower slots end up on top
            const Spr *sp = &sTSpr[list[k]];
            if (sp->prio == p) { sObjDX = sp->ui ? 0 : sEyeDX[3]; FastObj(c, sp, sp->ui ? R - sUi0 : R, track); sObjDX = 0; }
        }
    }
}

static void ComposeRowT(int R, u16 *dst)
{
    sIo = sLineRegs[R >= sUi0 && R < sUi0 + GBA_H ? R - sUi0 : 0];
    u16 dispcnt = R16(0);
    if (dispcnt & 0x80) {
        for (int x = 0; x < GBA_W; x++) dst[x] = 0xFFFF;
        return;
    }
    u16 bld = R16(0x50);
    int effect = (bld >> 6) & 3, tgt1 = bld & 0x3F, tgt2 = (bld >> 8) & 0x3F;
    Ctx *c = &sCtx;
    u16 back = sPal565[0];
    int semiActive = sTSemi[R] && tgt2;
    int blends = semiActive || (effect == 1 && tgt1 && tgt2) || ((effect & 2) && tgt1 && tgt1 != 0x3F);
    int fade = (effect & 2) && tgt1 == 0x3F;

    u16 plain[GBA_W + 16];
    if (!blends) {
        c->lb = plain;
        u32 b2 = back * 0x10001u;
        for (int i = 0; i < (GBA_W + 16) / 2; i++) ((u32 *)plain)[i] = b2;
        PaintRowT(c, R, dispcnt, 0);
        u16 *lb = plain + 8;
        if (fade) {
            int evy = R16(0x54) & 0x1F;
            if (evy > 16) evy = 16;
            if (evy) {
                int up = effect == 2;
                u16 fr[32], fg[32], fb[32];
                for (int v = 0; v < 32; v++) {
                    int o = up ? v + (((31 - v) * evy) >> 4) : v - ((v * evy) >> 4);
                    fr[v] = (u16)(o << 11); fb[v] = (u16)o;
                    fg[v] = (u16)((((o << 1) | (o >> 4))) << 5);
                }
                for (int x = 0; x < GBA_W; x++) {
                    u16 px = lb[x];
                    lb[x] = fr[px >> 11] | fg[(px >> 6) & 31] | fb[px & 31];
                }
            }
        }
        memcpy(dst, lb, GBA_W * 2);
        return;
    }

    __atomic_fetch_add(&sStSlow, 1, __ATOMIC_RELAXED);
    sStBld = bld; sStDisp = dispcnt;
    for (int i = 0; i < GBA_W + 16; i++) { c->topC[i] = back; c->topL[i] = LY_BD; c->botL[i] = LY_NONE; }
    PaintRowT(c, R, dispcnt, 1);
    int eva = R16(0x52) & 0x1F, evb = (R16(0x52) >> 8) & 0x1F, evy = R16(0x54) & 0x1F;
    if (eva > 16) eva = 16;
    if (evb > 16) evb = 16;
    if (evy > 16) evy = 16;
    for (int x = 0; x < GBA_W; x++) {
        int i = x + 8;
        u8 tl = c->topL[i], bl = c->botL[i];
        int semi = tl == LY_SEMI;
        if (semi) tl = LY_OBJ;
        int t1 = tgt1 & (1 << tl);
        int t2 = bl != LY_NONE && (tgt2 & (1 << bl));
        u16 col = c->topC[i];
        int blend = 0, fadeIt = 0;
        if (semi && t2) blend = 1;
        else if (effect == 1) blend = t1 && t2;
        else if (effect >= 2) fadeIt = t1;
        if (blend || fadeIt) {
            int r, g, b;
            Split(col, &r, &g, &b);
            if (blend) {
                int r2, g2, b2;
                Split(c->botC[i], &r2, &g2, &b2);
                r = (r * eva + r2 * evb) >> 4; g = (g * eva + g2 * evb) >> 4; b = (b * eva + b2 * evb) >> 4;
                if (r > 31) r = 31;
                if (g > 31) g = 31;
                if (b > 31) b = 31;
            } else if (effect == 2) {
                r += ((31 - r) * evy) >> 4; g += ((31 - g) * evy) >> 4; b += ((31 - b) * evy) >> 4;
            } else {
                r -= (r * evy) >> 4; g -= (g * evy) >> 4; b -= (b * evy) >> 4;
            }
            col = Join(r, g, b);
        }
        dst[x] = col;
    }
}

typedef struct { u16 *out; int r0, r1, eye; } Task;     // eye: 0 flat, 1 left, 2 right
typedef struct { Task t[2]; int n, depth; } Job;
static const int sLayerZ[4] = { 1, 0, -2, 3 };    // BG1, BG2, BG3, world sprites (pixels per eye at full slider, times 1.5)

static void RunJob(void *arg)
{
    const Job *j = arg;
    for (int k = 0; k < j->n; k++) {
        const Task *t = &j->t[k];
        for (int i = 0; i < 4; i++) {
            int d = t->eye ? (sLayerZ[i] * 3 * j->depth + 256) >> 9 : 0;   // 1.5 * z pixels per eye at full slider
            sEyeDX[i] = t->eye == 2 ? -d : d;
        }
        for (int R = t->r0; R < t->r1; R++) ComposeRowT(R, t->out + R * GBA_W);
    }
    memset(sEyeDX, 0, sizeof sEyeDX);
}

// ---- GPU composition: a description of the frame for platform/gpucomp3ds.c ---------------------------------
// The CPU keeps what is cheap and exact on it (the layer caches, the sprite list, the fixed band) and the platform draws
// BG1-BG3 and the sprites as textured rectangles. Frames outside the simple subset (blending, fades, windows, mosaic,
// affine / semi-transparent / 256-colour sprites, per-line changes other than BG0's scroll) are not described:
// PpuGpuFrameGet() returns NULL and the CPU compose below is the frame.
int gPpuGpuCompose;
static PpuGpuFrame sGpuFrame;
static int sGpuFrameOk, sGpuWasOn;
static u16 *sStageLayer[3], *sStageBand;
static u16 sBandRowA[GBA_W + 16], sBandRowB[GBA_W + 16];

void PpuGpuSetStage(u16 *layer[3], u16 *band)
{
    for (int i = 0; i < 3; i++) sStageLayer[i] = layer[i];
    sStageBand = band;
    sGpuWasOn = 0;
}
const u16 *PpuPal565(void) { return sPal565; }
const PpuGpuFrame *PpuGpuFrameGet(void) { return sGpuFrameOk ? &sGpuFrame : NULL; }

static inline u16 To5551(u16 c) { return (u16)((c & 0xF800) | (c & 0x07C0) | ((c & 0x1F) << 1) | 1); }   // RGB565 (made from 5-bit colour) -> RGBA5551, opaque

const u16 *PpuGpuLayerStage(int n, int *rowLo, int *rowHi)
{
    Layer *L = &sLayer[n - 1];
    u16 *st = sStageLayer[n - 1];
    if (!st || !L->valid) return NULL;
    int lo = TALL_H, hi = -1;
    for (int R = 0; R < TALL_H; R++) {
        if (!sRowDirty[n - 1][R]) continue;
        sRowDirty[n - 1][R] = 0;
        const u8 *idx = sLayerIdx[n - 1] + R * LSTRIDE;
        const u16 *pix = sLayerPix[n - 1] + R * LSTRIDE;
        u16 *d = st + R * LTEXW;
        for (int j = 0; j < 256 + GBA_W; j++) d[j] = idx[j] ? To5551(pix[j]) : 0;
        if (R < lo) lo = R;
        hi = R;
    }
    if (hi < 0) return NULL;
    *rowLo = lo & ~7; *rowHi = (hi | 7) + 1;
    return st;
}

// A semi-transparent sprite takes the place of every sprite pixel under it (the sprite pixels are resolved first, then
// the winner is blended with what is beneath), so it hides the sprites it overlaps that it beats. Within one priority
// level the GPU compositor reproduces that with a sprite buffer (the semi sprite clears it); across levels it
// does not: those frames are left to the CPU path.
static int SemiOverlaps(int g)
{
    const Spr *a = &sTSpr[g];
    for (int j = 0; j < sNTSpr; j++) {
        const Spr *t = &sTSpr[j];
        if (j == g || (t->flags & SF_SEMI) || t->ui != a->ui) continue;
        if (!(t->prio > a->prio)) continue;                                              // sprites of lower-priority levels it beats (same level: the compositor resolves it)
        if (t->x < a->x + a->w && a->x < t->x + t->w && t->y < a->y + a->h && a->y < t->y + t->h) return 1;
    }
    return 0;
}

static u16 sSheetX[PORT_OAM_N], sSheetY[PORT_OAM_N];
static int sShelfX, sShelfY, sShelfH;
// Gives the picture of affine sprite i (plus a transparent margin of 8 on every side) a place in the sheet: shelf packing.
static int SheetFit(int i)
{
    int cw = (sTSpr[i].w + 16 + 7) & ~7, ch = (sTSpr[i].h + 16 + 7) & ~7;
    if (sShelfX + cw > GPU_SHEET) { sShelfX = 0; sShelfY += sShelfH; sShelfH = 0; }
    if (sShelfY + ch > GPU_SHEET) return 0;
    sSheetX[i] = (u16)sShelfX; sSheetY[i] = (u16)sShelfY;
    sShelfX += cw; if (ch > sShelfH) sShelfH = ch;
    return 1;
}

static unsigned sWhy[12];
static int GpuEligible(void)
{
    sIo = sLineRegs[0];
    sShelfX = sShelfY = sShelfH = 0;
    u16 dispcnt = R16(0);
    if (dispcnt & 0xE080) { sWhy[1]++; return 0; }                              // forced blank, windows
    {   // alpha blending is not drawn by the GPU; a brightness fade of everything (all six targets) is, as one blended quad
        u16 bld = R16(0x50); int effect = (bld >> 6) & 3;
        if (effect == 1 || (effect >= 2 && (bld & 0x3F) != 0x3F)) { sWhy[2]++; return 0; }
        if (effect >= 2 && (dispcnt & 0x1000))
            for (int i = 0; i < sNTSpr; i++) if (sTSpr[i].flags & SF_SEMI) { sWhy[2]++; return 0; }   // semi sprites that blend skip the fade on the CPU path
    }
    for (int n = 0; n < 4; n++)
        if ((dispcnt & (0x100 << n)) && (R16(0x08 + n * 2) & 0x40) && (R16(0x4C) & 0xFF)) { sWhy[3]++; return 0; }     // mosaic
    for (int n = 1; n <= 3; n++)
        if (dispcnt & (0x100 << n))
            if (!sStageLayer[n - 1] || !sLayer[n - 1].valid || ((R16(0x08 + n * 2) >> 14) & 1)) { sWhy[4]++; return 0; } // 256-wide cached layers only
    if (!sStageBand) { sWhy[5]++; return 0; }
    if (dispcnt & 0x1000)
        for (int i = 0; i < sNTSpr; i++) {
            u8 fl = sTSpr[i].flags;
            if (fl & SF_8BPP) { sWhy[6]++; sWhy[10]++; return 0; }
            if ((fl & SF_AFFINE) && ((fl & SF_SEMI) || !SheetFit(i))) { sWhy[6]++; sWhy[8]++; return 0; }
            if (fl & SF_SEMI) {
                if ((R16(0x52) & 0x1F1F) != 0x1000 || ((R16(0x50) >> 8) & 0x1C) != 0x1C) { sWhy[9]++; return 0; }      // semi sprites are skipped (blend 0:16 = "show what is beneath"); other weights use the CPU path
                if (SemiOverlaps(i)) { sWhy[11]++; return 0; }                    // a semi sprite replaces the sprites under it (see SemiOverlaps)
            }
        }
    const u8 *a = sLineRegs[0];
    for (int y = 1; y < GBA_H; y++) {                            // only BG0's scroll (0x10-0x13: the band) may change from line to line
        const u8 *b = sLineRegs[y];
        if (memcmp(a, b, 4) || memcmp(a + 8, b + 8, 8) || memcmp(a + 0x14, b + 0x14, 0x58 - 0x14)) { sWhy[7]++; return 0; }
    }
    return 1;
}

// BG0 in its band as RGBA5551: drawn twice onto two different fills; the pixels both drawings agree on are BG0's, the rest is empty.
static void RenderBand(void)
{
    Ctx *c = &sCtx;
    int changed = 0;
    // Nothing to draw again when BG0's control, map, tiles, per-line scroll and the palette are what they were.
    {
        static u16 bCnt; static u8 bMap[0x2000], bChr[0x4000]; static u8 bScroll[BAND_ROWS][4]; static int bValid;
        u16 cnt = R16(0x08);
        u32 maplen = (cnt >> 14) == 3 ? 0x2000 : ((cnt >> 14) ? 0x1000 : 0x800);
        const u8 *map = gGbaVram + ((cnt >> 8) & 0x1F) * 0x800;
        const u8 *chr = gGbaVram + ((cnt >> 2) & 3) * 0x4000;
        int same = bValid && !sBandPalDirty && cnt == bCnt && !memcmp(bMap, map, maplen) && !memcmp(bChr, chr, 0x4000);
        for (int y = 0; same && y < BAND_ROWS; y++) if (memcmp(bScroll[y], sLineRegs[y] + 0x10, 4)) same = 0;
        if (same) { sGpuFrame.bandChanged = 0; return; }
        bCnt = cnt; memcpy(bMap, map, maplen); memcpy(bChr, chr, 0x4000);
        for (int y = 0; y < BAND_ROWS; y++) memcpy(bScroll[y], sLineRegs[y] + 0x10, 4);
        bValid = 1; sBandPalDirty = 0;
    }
    for (int y = 0; y < BAND_ROWS; y++) {
        sIo = sLineRegs[y];
        for (int i = 0; i < GBA_W + 16; i++) { sBandRowA[i] = 0x0000; sBandRowB[i] = 0xFFFF; }
        c->lb = sBandRowA; FastBgT(c, 0, y, 0, 0);
        c->lb = sBandRowB; FastBgT(c, 0, y, 0, 0);
        u16 *d = sStageBand + y * 256;
        for (int x = 0; x < GBA_W; x++) {
            u16 a2 = sBandRowA[8 + x], b2 = sBandRowB[8 + x];
            u16 v = a2 == b2 ? To5551(a2) : 0;
            if (d[x] != v) { d[x] = v; changed = 1; }
        }
    }
    sGpuFrame.bandChanged = changed;
}

static void GpuFill(int depth, int stereo)
{
    PpuGpuFrame *f = &sGpuFrame;
    sIo = sLineRegs[0];
    u16 dispcnt = R16(0);
    f->ui0 = sUi0;
    f->back = sPal565[0];
    for (int n = 0; n < 4; n++) { f->bgOn[n] = (dispcnt >> (8 + n)) & 1; f->bgPrio[n] = R16(0x08 + n * 2) & 3; }
    for (int n = 1; n <= 3; n++) f->scroll[n] = sLayer[n - 1].scroll;
    f->nSpr = (dispcnt & 0x1000) ? sNTSpr : 0;
    f->spr = sTSpr;
    f->map1d = (dispcnt & 0x40) != 0;
    f->stereo = stereo;
    for (int i = 0; i < 4; i++) {
        int d = stereo ? (sLayerZ[i] * 3 * depth + 256) >> 9 : 0;
        f->dx[0][i] = d; f->dx[1][i] = -d;
    }
    { int a = R16(0x52) & 0x1F, b = (R16(0x52) >> 8) & 0x1F; f->eva = a > 16 ? 16 : a; f->evb = b > 16 ? 16 : b; }
    { static u8 snap[0x8000]; memcpy(snap, gGbaVram + 0x10000, sizeof snap); f->objChr = snap; }
    { const u16 *oam = (const u16 *)gGbaOam; for (int m = 0; m < 32; m++) { f->affPar[m][0] = (s16)oam[m * 16 + 3]; f->affPar[m][1] = (s16)oam[m * 16 + 7]; f->affPar[m][2] = (s16)oam[m * 16 + 11]; f->affPar[m][3] = (s16)oam[m * 16 + 15]; }
      memcpy(f->sheetX, sSheetX, sizeof f->sheetX); memcpy(f->sheetY, sSheetY, sizeof f->sheetY); }
    { u16 bld = R16(0x50); int effect = (bld >> 6) & 3, evy = R16(0x54) & 0x1F; if (evy > 16) evy = 16;
      f->fade = effect >= 2 && evy ? effect - 1 : 0; f->evy = evy; }
    f->bandChanged = 0;
    if (f->bgOn[0]) RenderBand();
    sIo = gGbaIo;
}

// depth 0..256 is the 3D slider; with depth > 0 the upper 240 rows are also drawn for the right eye.
void PpuRenderTall(u16 *dst, u16 *right, int depth)
{
    sIo = gGbaIo;
    sUi0 = LayoutCamera();
    static int palOk;
    if (!palOk || memcmp(sPalShadow, gGbaPltt, sizeof sPalShadow)) {
        UpdatePal565();
        palOk = 1;
    }
    RebuildSpritesTall();
    if (gPpuGpuCompose && !sGpuWasOn) { sGpuWasOn = 1; for (int i = 0; i < 3; i++) sLayer[i].valid = 0; }   // rebuild every layer so that every row reaches the textures
    RefreshLayers();
    sStFrames++;
    sGpuFrameOk = 0;
    { static unsigned lastBld[8]; unsigned cur[8]; static int logged; const int ln[8] = { 0, 10, 20, 40, 60, 90, 120, 150 };
      for (int k = 0; k < 8; k++) cur[k] = *(const u16 *)(sLineRegs[ln[k]] + 0x50) | ((unsigned)*(const u16 *)(sLineRegs[ln[k]] + 0x52) << 16);
      int diff = 0; for (int k = 1; k < 8; k++) if (cur[k] != cur[0]) diff = 1;
      int chg = memcmp(cur, lastBld, sizeof cur) != 0; memcpy(lastBld, cur, sizeof cur);
      if ((diff || chg) && logged < 60) { logged++; PlatLog("ppu: bld/alpha per line 0/10/20/40/60/90/120/150: %08x %08x %08x %08x %08x %08x %08x %08x frame %u", cur[0], cur[1], cur[2], cur[3], cur[4], cur[5], cur[6], cur[7], (unsigned)gPortFrame); } }
    if (gPpuGpuCompose) { static int n; if (++n % 240 == 0) PlatLog("ppu: gpu-ineligible reasons %u %u %u %u %u %u %u", sWhy[1], sWhy[2], sWhy[3], sWhy[4], sWhy[5], sWhy[6], sWhy[7]); if (n % 240 == 0) PlatLog("ppu: ineligible sprite flags affine %u alpha %u 8bpp %u semi-overlap %u frames %u", sWhy[8], sWhy[9], sWhy[10], sWhy[11], (unsigned)gPortFrame); }
    if (gPpuGpuCompose && GpuEligible()) {
        GpuFill(depth, depth > 0 && right != NULL);
        sGpuFrameOk = 1;
        if (gPpuGpuCompose == 1) { sIo = gGbaIo; return; }      // the GPU draws this frame; mode 2 also draws it here, to compare
    }
    // The rows are drawn by two threads: the lower half on the New 3DS's spare core,
    // or, with 3D on, one eye each. Each task is a run of rows for one eye.
    Job a, w;
    memset(&a, 0, sizeof a); memset(&w, 0, sizeof w);
    if (depth > 0 && right) {
        a.n = 2; a.depth = w.depth = depth;
        a.t[0] = (Task){ dst, 0, 240, 1 };      a.t[1] = (Task){ dst, 240, 360, 0 };
        w.n = 2;
        w.t[0] = (Task){ right, 0, 240, 2 };    w.t[1] = (Task){ dst, 360, TALL_H, 0 };
    } else {
        a.n = 1; a.t[0] = (Task){ dst, 0, 240, 0 };
        w.n = 1; w.t[0] = (Task){ dst, 240, TALL_H, 0 };
    }
    u32 t0 = PlatTicksUs();
    int async = PlatJobStart(RunJob, &w);
    RunJob(&a);
    u32 t1 = PlatTicksUs();
    if (async) PlatJobWait(); else RunJob(&w);
    sStMain += t1 - t0; sStWait += PlatTicksUs() - t1;
    sIo = gGbaIo;
}

// Averages per frame since the last call, for the perf log: rows drawn on the slow blending path,
// layer rows rebuilt, and the last blend and display control values seen on a slow row.
void PpuLampStats(unsigned *exact, unsigned *guess, unsigned *drop)
{
    *exact = sStLampExact; *guess = sStLampGuess; *drop = sStLampDrop;
    sStLampExact = sStLampGuess = sStLampDrop = 0;
}

void PpuStats(unsigned *slowRows, unsigned *rebuilt, unsigned *bld, unsigned *disp, unsigned *mainUs, unsigned *waitUs)
{
    unsigned f = sStFrames ? sStFrames : 1;
    *mainUs = sStMain / f; *waitUs = sStWait / f; sStMain = sStWait = 0;
    *slowRows = sStSlow / f; *rebuilt = sStRebuilt / f; *bld = sStBld; *disp = sStDisp;
    sStSlow = sStRebuilt = sStFrames = 0;
}
