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
static void Strip(u16 *img, int w, int x0, int sw, int inner, int field)
{
    const int ruby = field == 0;
    for (int y = 0; y < PH; y++) {
        int t = y * 255 / (PH - 1);
        int r = ruby ? 150 - t * 90 / 255 : 10 + t * 10 / 255;
        int g = ruby ? 14 + t * 6 / 255    : 50 - t * 25 / 255;
        int b = ruby ? 28 + t * 10 / 255   : 140 - t * 70 / 255;
        for (int x = x0; x < x0 + sw; x++) {
            int stripe = ((x + y) / 6) & 1;        // soft diagonal stripes
            int k = stripe ? 12 : 0;
            int d = inner ? (x0 + sw - 1 - x) : (x - x0);      // distance from the table edge
            int glow = d < 10 ? (10 - d) * 6 : 0;
            img[y * w + x] = Rgb(r + k + glow, g + k + glow / 2, b + k + glow);
        }
        // bright rule right against the table
        for (int i = 0; i < 2; i++) {
            int x = inner ? x0 + sw - 1 - i : x0 + i;
            img[y * w + x] = ruby ? Rgb(255, 120, 90) : Rgb(120, 190, 255);
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
static void StatText(const char *label, const char *val, int y, int sw, int field)
{
    int ln = (int)strlen(label), vn = (int)strlen(val);
    u16 lc = field == 0 ? Rgb(255, 190, 160) : Rgb(170, 215, 255), vc = Rgb(255, 255, 255);
    int ls = 1;
    int vs = (sw - 10) / (vn * 6);
    if (vs < 1) vs = 1;
    if (vs > 3) vs = 3;
    Text(sTop, TW, (sw - ln * 6 * ls) / 2, y, label, ls, lc);
    Text(sTop, TW, (sw - vn * 6 * vs) / 2, y + 12, val, vs, vc);
}

static void DrawStats(int tx)
{
    if (gPortView) return;
    unsigned cur[4] = { sScoreLo, sScoreHi, (unsigned)sLives, (unsigned)sCoins };
    if (!memcmp(cur, sShown, sizeof cur)) return;
    memcpy(sShown, cur, sizeof cur);
    for (int y = 50; y < 200; y++) memcpy(sTop + y * TW, sTopBg + y * TW, tx * 2);
    char b[16];
    unsigned long long sc = (unsigned long long)sScoreHi * 100000000ull + sScoreLo;
    int n = 0; char rev[16];
    do { rev[n++] = (char)('0' + sc % 10); sc /= 10; } while (sc);
    for (int i = 0; i < n; i++) b[i] = rev[n - 1 - i];
    b[n] = 0;
    StatText("SCORE", b, 56, tx, gPortField);
    b[0] = (char)('0' + (sLives < 0 ? 0 : sLives % 10)); b[1] = 0;
    StatText("BALLS", b, 108, tx, gPortField);
    int c = sCoins < 0 ? 0 : sCoins;
    b[0] = 0; n = 0;
    if (c >= 100) b[n++] = (char)('0' + c / 100 % 10);
    if (c >= 10) b[n++] = (char)('0' + c / 10 % 10);
    b[n++] = (char)('0' + c % 10); b[n] = 0;
    StatText("COINS", b, 160, tx, gPortField);
    sDirty = 2;
    memcpy(sTopR, sTop, sizeof sTop);
}

// ---- smooth scaling ----------------------------------------------------
static inline u16 Avg(u16 a, u16 b) { return (u16)((a & b) + (((a ^ b) & 0xF7DE) >> 1)); }
static inline u32 Avg2(u32 a, u32 b) { return (a & b) + (((a ^ b) & 0xF7DEF7DEu) >> 1); }

// 240 source pixels to 320: every 3 become 4, the two inner ones blended.
static void Row4of3(const u16 *s, u16 *d)
{
    for (int i = 0; i < 80; i++, s += 3, d += 4) {
        u16 a = s[0], b = s[1], c = s[2];
        d[0] = a; d[1] = Avg(Avg(a, b), b); d[2] = Avg(Avg(b, c), b); d[3] = c;
    }
}

// 240 source pixels to 400: every 3 become 5, the new ones blended.
static void Row5of3(const u16 *s, u16 *d)
{
    for (int i = 0; i < 80; i++, s += 3, d += 5) {
        u16 a = s[0], b = s[1], c = s[2], n = i < 79 ? s[3] : s[2];
        d[0] = a; d[1] = Avg(Avg(a, b), b); d[2] = Avg(Avg(b, c), b); d[3] = Avg(Avg(b, c), c); d[4] = Avg(Avg(c, n), c);
    }
}

static void MixRows(const u16 *a, const u16 *b, u16 *d, int n, int heavyB)
{
    const u32 *pa = (const u32 *)a, *pb = (const u32 *)b;
    u32 *pd = (u32 *)d;
    if (heavyB) for (int i = 0; i < n / 2; i++) pd[i] = Avg2(Avg2(pa[i], pb[i]), pb[i]);
    else        for (int i = 0; i < n / 2; i++) pd[i] = Avg2(pa[i], pb[i]);
}

// One panel of the following view: 180 picture rows from src become 240 rows of 320.
typedef struct { const u16 *src; u16 *dst; int stride; } ZoomJob;
static void ZoomPanel(void *arg)
{
    const ZoomJob *j = arg;
    u16 r[3][320] __attribute__((aligned(4)));
    const u16 *s = j->src;
    u16 *d = j->dst;
    for (int g = 0; g < 60; g++, s += 3 * GBA_W, d += 4 * j->stride) {
        Row4of3(s, r[0]); Row4of3(s + GBA_W, r[1]); Row4of3(s + 2 * GBA_W, r[2]);
        memcpy(d, r[0], 640);
        MixRows(r[0], r[1], d + j->stride, 320, 1);
        MixRows(r[2], r[1], d + 2 * j->stride, 320, 1);
        memcpy(d + 3 * j->stride, r[2], 640);
    }
}

// Draws the table into the panel images and returns nothing; the margins are
// repainted only when the view or the table changes.
void LayoutFrame(const u16 *tall, const u16 *right)
{
    if (sMode != 1 || sPaintedView != gPortView || sPaintedField != gPortField) {
        sMode = 1;
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
    // Following view: 4/3 scale, 180 board rows per panel. The window only moves when the
    // ball leaves the middle band, and then eases after it, so the picture rests most of the time.
    int maxr0 = TALL_H - 360;
    int r0 = sR0 >> 4;
    int want = r0;
    if (gPortBallY < 0) want = maxr0;
    else if (gPortBallY < r0 + 110) want = gPortBallY - 110;
    else if (gPortBallY > r0 + 250) want = gPortBallY - 250;
    if (want < 0) want = 0;
    if (want > maxr0) want = maxr0;
    int step = (want * 16 - sR0) / 8;
    if (!step && want * 16 != sR0) step = want * 16 > sR0 ? 1 : -1;
    sR0 += step;
    r0 = sR0 >> 4;
    ZoomJob top = { tall + r0 * GBA_W, sTop + 40, TW }, bot = { tall + (r0 + 180) * GBA_W, sBot, BW };
    int async = PlatJobStart(ZoomPanel, &bot);
    ZoomPanel(&top);
    if (right) {
        // The right eye exists for rows 0-239 only; lower rows are on the flat part of the picture.
        static u16 mix[GBA_W * 180];
        for (int y = 0; y < 180; y++)
            memcpy(mix + y * GBA_W, (r0 + y < 240 ? right : tall) + (r0 + y) * GBA_W, GBA_W * 2);
        ZoomJob tr = { mix, sTopR + 40, TW };
        ZoomPanel(&tr);
    }
    if (async) PlatJobWait(); else ZoomPanel(&bot);
}

// ---- menus and bonus stages: a 240x160 picture -------------------------
#ifndef PORT_BUILD_ID
#define PORT_BUILD_ID "dev"
#endif

static void PaintMenuPanels(void)
{
    // top margins: dark, with a soft edge toward the picture
    for (int y = 0; y < PH; y++)
        for (int x = 0; x < TW; x++) {
            if (x >= 20 && x < 380) continue;
            int d = x < 20 ? 19 - x : x - 380;
            sTop[y * TW + x] = Rgb(18 - d / 2, 20 - d / 2, 34 - d);
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

// A 240x160 picture stretched to the full 400x240 top screen.
void LayoutMenuFrame(const u16 *src)
{
    if (sMode != 0) {
        sMode = 0;
        sPaintedView = -1;             // the table panels are painted again when play resumes
        PaintMenuPanels();
        sDirty = 2;
    }
    u16 r[2][400] __attribute__((aligned(4)));
    u16 *d = sTop;
    for (int g = 0; g < 80; g++, src += 2 * GBA_W, d += 3 * TW) {
        Row5of3(src, r[0]); Row5of3(src + GBA_W, r[1]);
        memcpy(d, r[0], 800);
        MixRows(r[0], r[1], d + TW, 400, 0);
        memcpy(d + 2 * TW, r[1], 800);
    }
}
