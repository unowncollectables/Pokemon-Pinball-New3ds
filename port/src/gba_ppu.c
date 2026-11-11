// Software GBA picture unit: mode 0 text backgrounds, regular and affine
// sprites, alpha/brightness blending and mosaic. That is everything this
// game uses (no windows, no bitmap or affine background modes).
//
// Two renderers produce identical pictures (the headless build can check
// that): a fast one that paints layers back to front straight into RGB565,
// and the general one below that composites per pixel. Lines that use alpha
// blending or mosaic take the general one.
#include <string.h>
#include "port.h"

#define R16(off) (*(const u16 *)(gGbaIo + (off)))

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

typedef struct {
    s16 x;
    u8 y, w, h, prio, flags, aff;
    u16 tile, palbase;
} Spr;
enum { SF_AFFINE = 1, SF_DOUBLE = 2, SF_H = 4, SF_V = 8, SF_8BPP = 16, SF_SEMI = 32 };
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
        sp->x = ox; sp->y = 0; sp->w = w; sp->h = h;
        sp->prio = (a2 >> 10) & 3;
        sp->flags = (affine ? SF_AFFINE : 0) | ((affine && (a0 & 0x200)) ? SF_DOUBLE : 0) |
                    (!affine && (a1 & 0x1000) ? SF_H : 0) | (!affine && (a1 & 0x2000) ? SF_V : 0) |
                    (bpp8 ? SF_8BPP : 0) | (mode == 1 ? SF_SEMI : 0);
        sp->aff = (a1 >> 9) & 0x1F;
        sp->tile = a2 & 0x3FF;
        sp->palbase = bpp8 ? 0x100 : (0x100 | ((a2 >> 12) << 4));
        sp->y = (u8)(oy & 0xFF);
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

INL void FastObj(Ctx *c, const Spr *sp, int y, int track)
{
    const u8 *chr = gGbaVram + 0x10000;
    int map1d = R16(0) & 0x40;
    int w = sp->w, h = sp->h, bw = w, bh = h;
    if (sp->flags & SF_DOUBLE) { bw *= 2; bh *= 2; }
    int oy = sp->y; if (oy >= 160) oy -= 256;
    int ly = y - oy, ox = sp->x;
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

static Ctx sCtx;

void PpuRenderLine(int y, u16 *dst)
{
    u16 dispcnt = R16(0);
    if (dispcnt & 0x80) {                              // forced blank shows white
        for (int x = 0; x < GBA_W; x++) dst[x] = 0xFFFF;
        return;
    }
    if (!sCacheValid || memcmp(sPalShadow, gGbaPltt, sizeof sPalShadow)) {
        const u16 *pl = (const u16 *)gGbaPltt;
        for (int i = 0; i < 512; i++) sPal565[i] = To565(pl[i] & 0x7FFF);
        memcpy(sPalShadow, gGbaPltt, sizeof sPalShadow);
    }
    if (!sCacheValid || memcmp(sOamShadow, gGbaOam, sizeof sOamShadow)) {
        RebuildSprites();
        memcpy(sOamShadow, gGbaOam, sizeof sOamShadow);
    }
    sCacheValid = 1;

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
