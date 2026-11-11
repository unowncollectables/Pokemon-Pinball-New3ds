// Screen layouts for the tall table. Everything here draws into plain row-major
// RGB565 images of the two panels (top 400x240, bottom 320x240); the front end
// only has to rotate them onto the console. All art is generated in code.
#include <string.h>
#include "port.h"

#define TW 400
#define BW 320
#define PH 240

static u16 sTop[TW * PH], sBot[BW * PH];
int gPortView;                         // 0 = whole table 1:1 with side panels, 1 = zoomed and following the ball
int gPortField;                        // 0 Ruby, 1 Sapphire (set by the game each frame)
int gPortBallY = -1;                   // ball row on the tall board, -1 when unknown

static u16 sTopBg[TW * PH];            // the painted strips without text, so the numbers can be redrawn cheaply
static unsigned sScoreLo, sScoreHi; static int sLives, sCoins;
static unsigned sShown[4] = { 1, 1, 1, 1 };
void LayoutStats(unsigned lo, unsigned hi, int lives, int coins) { sScoreLo = lo; sScoreHi = hi; sLives = lives; sCoins = coins; }
static int sPaintedView = -1, sPaintedField = -1;
static int sDirty;                     // frames for which the margins still have to be copied to the panels
static int sR0;                        // zoomed view: first board row shown, scaled by 16

const u16 *LayoutTop(void) { return sTop; }
const u16 *LayoutBot(void) { return sBot; }
int LayoutMarginsDirty(void) { return sDirty > 0; }
void LayoutMarginsDone(void) { if (sDirty > 0) sDirty--; }

// Column range [x0, x0+w) of each panel that the table covers in the current view.
void LayoutTableCols(int *topX0, int *topW, int *botX0, int *botW)
{
    if (gPortView) { *topX0 = 40; *topW = 320; *botX0 = 0; *botW = 320; }
    else           { *topX0 = 80; *topW = 240; *botX0 = 40; *botW = 240; }
}

// ---- tiny 5x7 letters for the wordmark --------------------------------
static const struct { char c; u8 r[7]; } sFont[] = {
    {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'Y', {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'N', {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}},
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
    {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
    {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
    {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
    {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
    {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
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
}

// Draws the table into the panel images and returns nothing; the margins are
// repainted only when the view or the table changes.
void LayoutFrame(const u16 *tall)
{
    if (sPaintedView != gPortView || sPaintedField != gPortField) {
        sPaintedView = gPortView;
        sPaintedField = gPortField;
        Paint();
        sDirty = 2;                    // the top screen is double buffered
    }
    {
        int tx, tw, bx, bw;
        LayoutTableCols(&tx, &tw, &bx, &bw);
        DrawStats(tx);
    }
    if (!gPortView) {
        for (int y = 0; y < PH; y++) {
            memcpy(sTop + y * TW + 80, tall + y * GBA_W, GBA_W * 2);
            memcpy(sBot + y * BW + 40, tall + (PH + y) * GBA_W, GBA_W * 2);
        }
        return;
    }
    // Zoomed: 4/3 scale, 180 board rows per panel, window follows the ball.
    int maxr0 = TALL_H - 360;
    int want = gPortBallY < 0 ? maxr0 : gPortBallY - 180;
    if (want < 0) want = 0;
    if (want > maxr0) want = maxr0;
    sR0 += (want * 16 - sR0) / 6;
    int r0 = sR0 >> 4;
    static u16 xmap[320];
    if (!xmap[319]) for (int x = 0; x < 320; x++) xmap[x] = (u16)(x * 3 / 4);
    for (int y = 0; y < PH; y++) {
        int sy = y * 3 / 4;
        const u16 *a = tall + (r0 + sy) * GBA_W, *b = tall + (r0 + 180 + sy) * GBA_W;
        u16 *dt = sTop + y * TW + 40, *db = sBot + y * BW;
        for (int x = 0; x < 320; x++) { dt[x] = a[xmap[x]]; db[x] = b[xmap[x]]; }
    }
}
