// Screen layouts for the tall table. Everything here draws into plain row-major
// RGB565 images of the two panels (top 400x240, bottom 320x240); the front end
// only has to rotate them onto the console. All art is generated in code.
#include <string.h>
#include "port.h"

#define TW 400
#define BW 320
#define PH 240

static u16 sTop[TW * PH], sTopR[TW * PH], sBot[BW * PH];
int gPortView;                         // 0 = whole table 1:1 with side panels, 1 = zoomed and following the ball
int gPortField;                        // 0 Ruby, 1 Sapphire (set by the game each frame)
int gPortBallY = -1;                   // ball row on the tall board, -1 when unknown

static u16 sTopBg[TW * PH];            // the painted strips without text, so the numbers can be redrawn cheaply
static unsigned sScoreLo, sScoreHi; static int sLives, sCoins;
static unsigned sShown[4] = { 1, 1, 1, 1 };
void LayoutStats(unsigned lo, unsigned hi, int lives, int coins) { sScoreLo = lo; sScoreHi = hi; sLives = lives; sCoins = coins; }
static int sPaintedView = -1, sPaintedField = -1;
static int sMode = 1;                  // 1 = tall table, 0 = a 240x160 screen (menus, bonus stages)
static int sDirty;                     // frames for which the margins still have to be copied to the panels
static int sR0;                        // zoomed view: first board row shown, scaled by 16
static float sCam, sCamVel, sBallVel;  // camera position (board rows), its speed, and the ball's speed
static int sMoving;
static int sLastBall = -1, sSnap = 1;  // sSnap: jump to the target instead of gliding

const u16 *LayoutTop(void) { return sTop; }
const u16 *LayoutBot(void) { return sBot; }
const u16 *LayoutTopR(void) { return sTopR; }
int LayoutMarginsDirty(void) { return sDirty > 0; }
void LayoutRepaint(void) { sDirty = 2; memcpy(sTopR, sTop, sizeof sTop); }
void LayoutMarginsDone(void) { if (sDirty > 0) sDirty--; }

// Column range [x0, x0+w) of each panel that the table covers in the current view.
void LayoutTableCols(int *topX0, int *topW, int *botX0, int *botW)
{
    if (!sMode) { *topX0 = 16; *topW = 368; *botX0 = 0; *botW = 0; }
    else if (gPortView) { *topX0 = 40; *topW = 320; *botX0 = 0; *botW = 320; }
    else           { *topX0 = 80; *topW = 240; *botX0 = 40; *botW = 240; }
}

// ---- tiny 5x7 letters for the wordmark --------------------------------
static const struct { char c; u8 r[7]; } sFont[] = {
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}}, {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}}, {'D', {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}}, {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}}, {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}}, {'J', {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}}, {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}}, {'N', {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}}, {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}}, {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}}, {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}}, {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}}, {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}}, {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}}, {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}}, {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}}, {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}}, {'/', {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10}},
};

static inline u16 Rgb(int r, int g, int b) { return (u16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)); }

static void Text(u16 *img, int w, int x, int y, const char *s, int scale, u16 col)
{
    for (; *s; s++, x += 6 * scale) {
        for (unsigned k = 0; k < sizeof sFont / sizeof sFont[0]; k++) {
            if (sFont[k].c != *s) continue;
            for (int r = 0; r < 7; r++)
                for (int c = 0; c < 5; c++)
                    if (sFont[k].r[r] & (0x10 >> c))
                        for (int dy = 0; dy < scale; dy++)
                            for (int dx = 0; dx < scale; dx++)
                                img[(y + r * scale + dy) * w + x + c * scale + dx] = col;
        }
    }
}

// One side strip, x0..x0+sw of an image w wide. inner = the column next to the table.
// Drawn like the side of an arcade table: dark cabinet panel, a chrome rail against the playfield,
// a neon tube in the table's colour and a column of lit bulbs.
static void Strip(u16 *img, int w, int x0, int sw, int inner, int field)
{
    const int ruby = field == 0;
    const int narrow = sw <= 40;                                // the 40 px margins keep their room for the readout
    const int ar = ruby ? 255 : 90, ag = ruby ? 70 : 170, ab = ruby ? 60 : 255;     // accent
    for (int y = 0; y < PH; y++) {
        int t = y * 255 / (PH - 1);
        int r = ruby ? 46 - t * 22 / 255 : 8 + t * 4 / 255;
        int g = ruby ? 8 + t * 4 / 255   : 22 - t * 10 / 255;
        int b = ruby ? 14 + t * 8 / 255  : 56 - t * 24 / 255;
        for (int x = x0; x < x0 + sw; x++) {
            int d = inner ? (x0 + sw - 1 - x) : (x - x0);       // distance from the table edge
            int fo = inner ? (x - x0) : (x0 + sw - 1 - x);       // distance from the far (screen) edge
            int cr = r, cg = g, cb = b;
            int tex = ((x * 3 + y) % 8 == 0) ? 5 : 0;           // fine diagonal grain
            cr += tex; cg += tex; cb += tex;
            if (d < 2) { cr = 235; cg = 238; cb = 245; }                       // chrome rail: highlight,
            else if (d < 4) { cr = 150; cg = 156; cb = 170; }                  // body,
            else if (d < 6) { cr = 60; cg = 64; cb = 78; }                     // shadow
            else if (narrow ? (d >= 6 && d < 8) : (d >= 7 && d < 10)) {                // neon tube with a hot core
                int core = narrow ? d == 6 : d == 8;
                cr = core ? 255 : ar; cg = core ? 255 : ag; cb = core ? 255 : ab;
                if (core) { cr = (ar + 255 * 3) / 4; cg = (ag + 255 * 3) / 4; cb = (ab + 255 * 3) / 4; }
            } else if (!narrow && d >= 10 && d < 16) {                              // its glow on the panel
                int k = (16 - d) * 5;
                cr += ar * k / 255; cg += ag * k / 255; cb += ab * k / 255;
            }
            if (fo < 2) { cr = cr / 3; cg = cg / 3; cb = cb / 3; }              // dark edge against the bezel
            img[y * w + x] = Rgb(cr > 255 ? 255 : cr, cg > 255 ? 255 : cg, cb > 255 ? 255 : cb);
        }
    }
    // a column of bulbs, every second one lit brighter, set between the glow and the screen edge
    int bd = narrow ? -1 : sw - 8;       // against the screen edge, clear of the readout
    if (bd < 0) return;
    int bx = inner ? x0 + sw - 1 - bd : x0 + bd;
    for (int cy = 14, k = 0; cy < PH - 8; cy += 18, k++) {
        int lit = k & 1;
        for (int dy = -4; dy <= 4; dy++)
            for (int dx = -4; dx <= 4; dx++) {
                int q = dx * dx + dy * dy;
                if (q > 17) continue;
                int x = bx + dx, y = cy + dy;
                if (x < x0 || x >= x0 + sw) continue;
                int cr, cg, cb;
                if (q > 11) { cr = 24; cg = 24; cb = 30; }                      // socket ring
                else {
                    int hl = (dx <= 0 && dy <= 0) ? 70 : 0;                     // glint, top left
                    cr = (lit ? ar : ar / 3) + hl; cg = (lit ? ag : ag / 3) + hl; cb = (lit ? ab : ab / 3) + hl;
                    if (lit && q < 4) { cr = 255; cg = (ag + 255) / 2 + 40; cb = (ab + 255) / 2 + 40; }
                }
                img[y * w + x] = Rgb(cr > 255 ? 255 : cr, cg > 255 ? 255 : cg, cb > 255 ? 255 : cb);
            }
    }
}

static void Paint(void)
{
    int tx, tw, bx, bw;
    LayoutTableCols(&tx, &tw, &bx, &bw);
    // left and right strips on both panels
    Strip(sTop, TW, 0, tx, 1, gPortField);
    Strip(sTop, TW, tx + tw, TW - tx - tw, 0, gPortField);
    if (bx) {
        Strip(sBot, BW, 0, bx, 1, gPortField);
        Strip(sBot, BW, bx + bw, BW - bx - bw, 0, gPortField);
    }
    // wordmark on the wide top strips, as large as fits
    const char *name = gPortField == 0 ? "RUBY" : "SAPPHIRE";
    int len = (int)strlen(name), sw = tx;
    int sc = (sw - 12) / (len * 6);
    if (sc < 1) sc = 1;
    if (sc > 3) sc = 3;
    if (sw < len * 6 + 4) goto done;
    int x = (sw - len * 6 * sc + sc) / 2;
    u16 col = gPortField == 0 ? Rgb(255, 214, 190) : Rgb(205, 232, 255);
    Text(sTop, TW, x, 20, name, sc, col);
    Text(sTop, TW, tx + tw + x, 20, name, sc, col);
done:
    memcpy(sTopBg, sTop, sizeof sTop);
    sShown[0] = ~0u;                   // numbers must be drawn again
}

// Number panel on the left top strip (1:1 view only): score, balls and coins, drawn in code.
static void StatText(const char *label, const char *val, int y, int sw, int field, int x0)
{
    int ln = (int)strlen(label), vn = (int)strlen(val);
    u16 lc = field == 0 ? Rgb(255, 190, 160) : Rgb(170, 215, 255), vc = Rgb(255, 255, 255);
    int ls = 1;
    int vs = (sw - 10) / (vn * 6);
    if (vs < 1) vs = 1;
    if (vs > 3) vs = 3;
    Text(sTop, TW, x0 + (sw - ln * 6 * ls) / 2, y, label, ls, lc);
    Text(sTop, TW, x0 + (sw - vn * 6 * vs) / 2, y + 12, val, vs, vc);
}

static void DrawStats(int tx)
{
    unsigned cur[4] = { sScoreLo, sScoreHi, (unsigned)sLives, (unsigned)sCoins };
    if (!memcmp(cur, sShown, sizeof cur)) return;
    memcpy(sShown, cur, sizeof cur);
    for (int y = 50; y < 200; y++) {
        memcpy(sTop + y * TW, sTopBg + y * TW, tx * 2);
        if (gPortView) memcpy(sTop + y * TW + TW - tx, sTopBg + y * TW + TW - tx, tx * 2);
    }
    char b[24];
    unsigned long long sc = (unsigned long long)sScoreHi * 100000000ull + sScoreLo;
    int n = 0; char rev[24];
    do { rev[n++] = (char)('0' + sc % 10); sc /= 10; } while (sc);
    for (int i = 0; i < n; i++) b[i] = rev[n - 1 - i];
    b[n] = 0;
    char bb[2] = { (char)('0' + (sLives < 0 ? 0 : sLives % 10)), 0 };
    char cc[4]; int cn = 0, c = sCoins < 0 ? 0 : sCoins;
    if (c >= 100) cc[cn++] = (char)('0' + c / 100 % 10);
    if (c >= 10) cc[cn++] = (char)('0' + c / 10 % 10);
    cc[cn++] = (char)('0' + c % 10); cc[cn] = 0;
    if (!gPortView) {
        StatText("SCORE", b, 56, tx, gPortField, 0);
        StatText("BALLS", bb, 108, tx, gPortField, 0);
        StatText("COINS", cc, 160, tx, gPortField, 0);
    } else {
        // the zoomed view keeps a slim readout in the 40 px margins: balls and coins left, score right
        StatText("BALLS", bb, 56, tx - 8, gPortField, 0);
        StatText("COINS", cc, 108, tx - 8, gPortField, 0);
        u16 lc = gPortField == 0 ? Rgb(255, 190, 160) : Rgb(170, 215, 255);
        int rx = TW - tx + 8, rw = tx - 8;
        Text(sTop, TW, rx + (rw - 30) / 2, 56, "SCORE", 1, lc);
        int groups = (n + 2) / 3, first = n - (groups - 1) * 3;   // digits in the leading group
        int pos = 0;
        for (int g = 0; g < groups; g++) {
            char t[4]; int k = g ? 3 : first;
            memcpy(t, b + pos, k); t[k] = 0; pos += k;
            Text(sTop, TW, rx + (rw - k * 6) / 2, 70 + g * 10, t, 1, Rgb(255, 255, 255));
        }
    }
    sDirty = 2;
    memcpy(sTopR, sTop, sizeof sTop);
}

// ---- smooth scaling ----------------------------------------------------
static inline u16 Avg(u16 a, u16 b) { return (u16)((a & b) + (((a ^ b) & 0xF7DE) >> 1)); }
static inline u32 Avg2(u32 a, u32 b) { return (a & b) + (((a ^ b) & 0xF7DEF7DEu) >> 1); }

static void MixRows(const u16 *a, const u16 *b, u16 *d, int n, int heavyB)
{
    const u32 *pa = (const u32 *)a, *pb = (const u32 *)b;
    u32 *pd = (u32 *)d;
    if (heavyB) for (int i = 0; i < n / 2; i++) pd[i] = Avg2(Avg2(pa[i], pb[i]), pb[i]);
    else        for (int i = 0; i < n / 2; i++) pd[i] = Avg2(pa[i], pb[i]);
}

// 240 source pixels to 320: every 3 become 4, the last one doubled. No blending, so every
// pixel of the table stays a hard-edged pixel.
static void Row4of3(const u16 *s, u16 *d)
{
    for (int i = 0; i < 80; i++, s += 3, d += 4) { u16 a = s[0], b = s[1], c = s[2]; d[0] = a; d[1] = b; d[2] = c; d[3] = c; }
}

// One slice of a panel of the following view: output rows y0..y1 of 320 columns. Output row y
// shows source position s16 + 12 y (1/16 source rows, 4/3 scale). At rest (crisp) each output row
// is one whole source row; while the camera moves they are blended in quarters so the table scrolls
// smoothly instead of jumping a whole source row at a time.
typedef struct { const u16 *src; int s16, maxRow; u16 *dst; int stride, y0, y1, crisp; } ZoomJob;
static inline void MixQ(const u16 *a, const u16 *b, u16 *d, int q)    // q/4 of b
{
    const u32 *pa = (const u32 *)a, *pb = (const u32 *)b;
    u32 *pd = (u32 *)d;
    switch (q) {
    case 0: memcpy(d, a, 640); break;
    case 1: for (int i = 0; i < 160; i++) pd[i] = Avg2(pa[i], Avg2(pa[i], pb[i])); break;
    case 2: for (int i = 0; i < 160; i++) pd[i] = Avg2(pa[i], pb[i]); break;
    default: for (int i = 0; i < 160; i++) pd[i] = Avg2(pb[i], Avg2(pa[i], pb[i])); break;
    }
}
static void ZoomPanel(void *arg)
{
    const ZoomJob *j = arg;
    u16 h[2][320] __attribute__((aligned(4)));
    int have[2] = { -1, -1 };
    for (int y = j->y0; y < j->y1; y++) {
        int p = j->s16 + y * 12;
        if (j->crisp) {
            int i = (p + 8) >> 4;
            if (i > j->maxRow) i = j->maxRow;
            Row4of3(j->src + i * GBA_W, j->dst + y * j->stride);
            continue;
        }
        int i = p >> 4, q = (p >> 2) & 3;
        if (i > j->maxRow) i = j->maxRow;
        int i1 = i + 1 > j->maxRow ? j->maxRow : i + 1;
        if (have[i & 1] != i) { Row4of3(j->src + i * GBA_W, h[i & 1]); have[i & 1] = i; }
        if (have[i1 & 1] != i1) { Row4of3(j->src + i1 * GBA_W, h[i1 & 1]); have[i1 & 1] = i1; }
        MixQ(h[i & 1], h[i1 & 1], j->dst + y * j->stride, i1 == i ? 0 : q);
    }
}

// ---- camera of the following view -------------------------------------
#define SEAM 8      // output rows of the table hidden behind the gap between the two screens (the table runs on past it)
#define VIEW_ROWS 366     // source rows the two panels span, gap included
int LayoutCamera(void)
{
    if (!gPortView || sMode != 1 || sPaintedView != gPortView) {
        if (gPortView) sSnap = 1;
        return TALL_BAND;
    }
    // The window only moves when the ball leaves the middle band (shifted toward where the ball is
    // heading); then a critically damped spring carries it there, so the picture rests most of the
    // time and never jerks.
    const float maxr0 = (float)(TALL_H - VIEW_ROWS - 1);
    float by = (float)gPortBallY;
    if (gPortBallY >= 0 && sLastBall >= 0) sBallVel += ((float)(gPortBallY - sLastBall) - sBallVel) * 0.25f;
    else sBallVel = 0;
    sLastBall = gPortBallY;
    float want = sCam;
    if (gPortBallY < 0) want = maxr0;
    else {
        by += sBallVel * 10.0f;                    // look ahead
        if (by < sCam + 110) want = by - 110;
        else if (by > sCam + 250) want = by - 250;
    }
    if (want < 0) want = 0;
    if (want > maxr0) want = maxr0;
    float d = want - sCam;
    if (sSnap || d > 220 || d < -220) { sCam = want; sCamVel = 0; sSnap = 0; }   // view switch or a teleport: no glide
    else {
        float w = 0.14f;
        sCamVel += w * w * d - 2 * w * sCamVel;
        if (sCamVel > 7) sCamVel = 7;
        if (sCamVel < -7) sCamVel = -7;
        sCam += sCamVel;
        if (sCam < 0) { sCam = 0; sCamVel = 0; }
        if (sCam > maxr0) { sCam = maxr0; sCamVel = 0; }
        if (sCamVel < 0.04f && sCamVel > -0.04f && d < 0.5f && d > -0.5f) { sCam = want; sCamVel = 0; }   // come to rest on a whole row
    }
    sMoving = sCamVel != 0 || sCam != (float)(int)sCam;
    sR0 = sMoving ? (int)(sCam * 16.0f) : (int)sCam * 16;
    // The fixed-on-screen band (banners, shop, menus) is drawn centred on the bottom screen.
    int ui = (sR0 >> 4) + 186 + 10;
    return ui > TALL_H - GBA_H ? TALL_H - GBA_H : ui;
}

typedef struct { ZoomJob b, r; int hasR; } SideArg;
static void SideJob(void *arg)
{
    SideArg *a = arg;
    ZoomPanel(&a->b);
    if (a->hasR) ZoomPanel(&a->r);
}

// Draws the table into the panel images and returns nothing; the margins are
// repainted only when the view or the table changes.
void LayoutFrame(const u16 *tall, const u16 *right)
{
    if (sMode != 1 || sPaintedView != gPortView || sPaintedField != gPortField) {
        sMode = 1;
        if (sPaintedView != gPortView) sSnap = 1;
        sPaintedView = gPortView;
        sPaintedField = gPortField;
        Paint();
        sDirty = 2;                    // the top screen is double buffered
        memcpy(sTopR, sTop, sizeof sTop);
    }
    {
        int tx, tw, bx, bw;
        LayoutTableCols(&tx, &tw, &bx, &bw);
        DrawStats(tx);
    }
    if (!gPortView) {
        for (int y = 0; y < PH; y++) {
            memcpy(sTop + y * TW + 80, tall + y * GBA_W, GBA_W * 2);
            if (right) memcpy(sTopR + y * TW + 80, right + y * GBA_W, GBA_W * 2);
            memcpy(sBot + y * BW + 40, tall + (PH + y) * GBA_W, GBA_W * 2);
        }
        return;
    }
    // Following view: 4/3 scale, the camera was stepped by LayoutCamera before the picture was drawn.
    // Eight output rows are skipped between the screens, so a ball crossing the gap behind the
    // hinge does not appear to jump.
    int r0 = sR0 >> 4, fr = sR0 & 15, last = TALL_H - 1 - r0, crisp = !sMoving;
    ZoomJob top = { tall + r0 * GBA_W, fr, last, sTop + 40, TW, 0, PH, crisp };
    ZoomJob bot = { tall + r0 * GBA_W, fr + (PH + SEAM) * 12, last, sBot, BW, 0, PH, crisp };
    static u16 mix[GBA_W * 184];
    ZoomJob tr1 = { mix, fr, 0, sTopR + 40, TW, 0, 120, crisp }, tr2 = { mix, fr, 0, sTopR + 40, TW, 120, PH, crisp };
    if (right) {
        // The right eye exists for rows 0-239 only; lower rows are on the flat part of the picture.
        int rows = last + 1 < 184 ? last + 1 : 184;
        for (int y = 0; y < rows; y++)
            memcpy(mix + y * GBA_W, (r0 + y < 240 ? right : tall) + (r0 + y) * GBA_W, GBA_W * 2);
        tr1.maxRow = tr2.maxRow = rows - 1;
    }
    // Work is split evenly over both cores: top picture and half the right eye here, bottom picture and the rest there.
    static SideArg side;
    side.b = bot; side.r = tr2; side.hasR = right != NULL;
    int async = PlatJobStart(SideJob, &side);
    ZoomPanel(&top);
    if (right) ZoomPanel(&tr1);
    if (async) PlatJobWait(); else SideJob(&side);
}

// ---- menus and bonus stages: a 240x160 picture -------------------------
#ifndef PORT_BUILD_ID
#define PORT_BUILD_ID "dev"
#endif

#define MENU_X 80
#define MENU_Y 40
static void PaintMenuPanels(int fill)
{
    // top: the 240x160 picture sits at 1:1 in the middle (pixel for pixel, no scaling);
    // everything around it is our own backdrop in the same style as the bottom panel
    for (int y = 0; y < PH; y++) {
        int t = y * 255 / (PH - 1);
        for (int x = 0; x < TW; x++) {
            int k = (((x + y) / 8) & 1) ? 6 : 0;
            sTop[y * TW + x] = Rgb(16 + k, 22 + k + t / 16, 52 + k + t / 6);
        }
    }
    if (fill) {                        // 1.5x: the picture covers 360 of the 400 columns, a rule closes each side
        const u16 red = Rgb(230, 70, 60), blue = Rgb(70, 130, 240);
        for (int y = 0; y < PH; y++) { sTop[y * TW + 19] = sTop[y * TW + 380] = red; sTop[y * TW + 18] = sTop[y * TW + 381] = blue; }
    } else {
        const u16 red = Rgb(230, 70, 60), blue = Rgb(70, 130, 240);
        for (int x = MENU_X - 2; x < MENU_X + GBA_W + 2; x++) {
            sTop[(MENU_Y - 2) * TW + x] = sTop[(MENU_Y + GBA_H + 1) * TW + x] = blue;
            sTop[(MENU_Y - 1) * TW + x] = sTop[(MENU_Y + GBA_H) * TW + x] = red;
        }
        for (int y = MENU_Y - 2; y < MENU_Y + GBA_H + 2; y++) {
            sTop[y * TW + MENU_X - 2] = sTop[y * TW + MENU_X + GBA_W + 1] = blue;
            sTop[y * TW + MENU_X - 1] = sTop[y * TW + MENU_X + GBA_W] = red;
        }
        Text(sTop, TW, (TW - 15 * 12) / 2, 14, "POKEMON PINBALL", 2, Rgb(255, 255, 255));
        Text(sTop, TW, (TW - 17 * 6) / 2, 216, "RUBY AND SAPPHIRE", 1, Rgb(170, 190, 230));
    }
    // bottom: our own information panel
    for (int y = 0; y < PH; y++) {
        int t = y * 255 / (PH - 1);
        for (int x = 0; x < BW; x++) {
            int k = (((x + y) / 8) & 1) ? 6 : 0;
            sBot[y * BW + x] = Rgb(16 + k, 22 + k + t / 16, 52 + k + t / 6);
        }
    }
    for (int x = 0; x < BW; x++) {     // one red and one blue rule under the title
        sBot[58 * BW + x] = sBot[59 * BW + x] = Rgb(230, 70, 60);
        sBot[61 * BW + x] = sBot[62 * BW + x] = Rgb(70, 130, 240);
    }
    const u16 white = Rgb(255, 255, 255), soft = Rgb(170, 190, 230), key = Rgb(255, 214, 120);
    Text(sBot, BW, (BW - 7 * 18) / 2, 14, "PINBALL", 3, white);
    Text(sBot, BW, (BW - 17 * 6) / 2, 42, "RUBY AND SAPPHIRE", 1, soft);
    static const char *const rows[][2] = {
        { "L / R", "FLIPPERS" }, { "A", "LAUNCH AND SELECT" }, { "START", "PAUSE" },
        { "Y", "WHOLE TABLE OR FOLLOW VIEW" }, { "3D SLIDER", "DEPTH" },
    };
    for (int i = 0; i < 5; i++) {
        int y = 80 + i * 24;
        Text(sBot, BW, 24, y, rows[i][0], 2, key);
        Text(sBot, BW, 144, y + 4, rows[i][1], 1, white);
    }
    char b[40] = "BUILD ";
    int n = 6;
    for (const char *p = PORT_BUILD_ID; *p && *p != '-' && n < 38; p++) b[n++] = *p;
    b[n] = 0;
    Text(sBot, BW, BW - 8 - n * 6, PH - 14, b, 1, soft);
}

// 240 source pixels to 360: every 2 become 3, the middle one blended.
static void Row3of2(const u16 *s, u16 *d)
{
    for (int i = 0; i < 120; i++, s += 2, d += 3) { d[0] = s[0]; d[1] = Avg(s[0], s[1]); d[2] = s[1]; }
}

// A 240x160 picture on the top screen at 1.5x, filling the height; our own backdrop fills the sides.
void LayoutMenuFrame(const u16 *src)
{
    if (sMode != 0) {
        sMode = 0;
        sPaintedView = -1;             // the table panels are painted again when play resumes
        PaintMenuPanels(1);
        sDirty = 2;
    }
    u16 r[2][360] __attribute__((aligned(4)));
    u16 *d = sTop + 20;
    for (int g = 0; g < 80; g++, src += 2 * GBA_W, d += 3 * TW) {
        Row3of2(src, r[0]); Row3of2(src + GBA_W, r[1]);
        memcpy(d, r[0], 720);
        MixRows(r[0], r[1], d + TW, 360, 0);
        memcpy(d + 2 * TW, r[1], 720);
    }
}
