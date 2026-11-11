// Software GBA picture unit: mode 0 text backgrounds, regular and affine
// sprites, alpha/brightness blending and mosaic. That is everything this
// game uses (no windows, no bitmap or affine background modes).
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

void PpuRenderLine(int y, u16 *dst)
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
