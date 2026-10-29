// TahVia UI - boot, idle/pairing, navigation HUD and firmware update screens.
// Design language (from TahVia_UI.html): pure black on white, Inter Tight type, big numbers,
// a solid black turn banner, a thin status bar, 2 px rules and tracked small caps.
// Everything goes through MonoTileRenderer: text/icons are 1-bpp sprites blitted as bitmaps.
#include "ui.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "tv_fonts.h"

using namespace mtr;

namespace ui {
namespace {

using BF = fonts::Font;
using namespace fonts;

// ---- Layout (480x320) ---------------------------------------------------------
const int BAN_H = 96;                                   // turn banner
const int BOT_H = 36, BOT_Y = SCREEN_H - BOT_H;         // status bar
const int ANCHOR_X = SCREEN_W / 2, ANCHOR_Y = 215;      // vehicle marker: low on screen, more road ahead
const int ROUTE_W = 10, ROUTE_W2 = 6;                   // route up to the next turn / after it
const float CULL_R = 300.f;                             // px around the marker
const int BACK_MAX = 24, FWD_MAX = 200;

const int IC = 72;                                      // banner icon size
const int LOGO_BIG = 88, LOGO_SMALL = 26;
const int BAR_X = 120, BAR_Y = 238, BAR_W = 240;        // boot progress

const Mode M_SET = SET, M_CLR = CLEAR;

TFT_eSPI* tft = nullptr;
Renderer* R   = nullptr;

// ---- Animations ----------------------------------------------------------------
#ifndef UI_ANIMATIONS
#define UI_ANIMATIONS 1
#endif
// Map redraw throttle. A base redraw dirties every tile and, in heading-up mode, a rotation changes most of the
// map's pixels, so the map is only redrawn when the view moved/rotated by a visible step AND at most every
// UI_MAP_MIN_MS. Raise the three values for fewer tile updates, lower them for a smoother map.
#ifndef UI_MAP_POS_Q_PX
#define UI_MAP_POS_Q_PX  2.0f   // snap map position to this many pixels
#endif
#ifndef UI_MAP_HDG_Q_DEG
#define UI_MAP_HDG_Q_DEG 3.0f   // snap map heading to this many degrees
#endif
#ifndef UI_MAP_MIN_MS
#define UI_MAP_MIN_MS    100    // max 10 map redraws per second
#endif
const uint32_t REVEAL_MS  = 320;       // screen change: curtain wipe
const uint32_t BURST_MS   = 700;       // idle: ring burst on connect
const uint32_t POP_MS     = 260;       // idle: check badge pops in
const uint32_t ICON_IN_MS = 260;       // nav: icon slides in on maneuver change
const uint16_t TURN_NEAR_M = 120;      // nav: icon bobs when the turn is this close
const uint16_t TURN_BAR_M  = 400;      // nav: proximity bar fills over the last 400 m

inline float easeOut(float p) { p = p < 0 ? 0 : (p > 1 ? 1 : p); return 1.f - (1.f - p) * (1.f - p); }
inline float tri(uint32_t now, uint32_t period) {
  float p = (now % period) / (float)period;
  return p < 0.5f ? p * 2.f : (1.f - p) * 2.f;
}

bool     revealPending = false;
uint32_t revealStart   = 0;

// Curtain: call LAST in a frame (it covers everything emitted before it).
void emitReveal(uint32_t now) {
#if UI_ANIMATIONS
  if (revealPending) { revealPending = false; revealStart = now; }
  uint32_t el = now - revealStart;
  if (el >= REVEAL_MS) return;
  int x = (int)(SCREEN_W * easeOut(el / (float)REVEAL_MS));
  R->ovFillRect(x, 0, SCREEN_W - x, SCREEN_H, M_CLR);
  R->ovFillRect(x - 3, 0, 3, SCREEN_H, M_SET);
#else
  (void)now;
#endif
}

// ---- Raw 1-bpp helpers (TFT_eSprite 1 bpp = MSB first, rows padded to bytes) ----------------
inline uint8_t* rawBits(TFT_eSprite* s) { return s ? (uint8_t*)s->getPointer() : nullptr; }
inline const uint8_t* ptr(TFT_eSprite* s) { return rawBits(s); }
inline void setBit(uint8_t* p, int w, int h, int x, int y) {
  if ((unsigned)x >= (unsigned)w || (unsigned)y >= (unsigned)h) return;
  p[y * ((w + 7) >> 3) + (x >> 3)] |= (uint8_t)(0x80 >> (x & 7));
}
inline bool getBit(const uint8_t* p, int w, int h, int x, int y) {
  if ((unsigned)x >= (unsigned)w || (unsigned)y >= (unsigned)h) return false;
  return (p[y * ((w + 7) >> 3) + (x >> 3)] >> (7 - (x & 7))) & 1;
}

void mkSprite(TFT_eSprite*& sp, int w, int h) {
  sp = new TFT_eSprite(tft);
  sp->setColorDepth(1);
  sp->createSprite(w, h);
  sp->fillSprite(0);
}

// ---- Text sprites ---------------------------------------------------------------------------
struct TextSpr {
  TFT_eSprite* sp = nullptr;
  TFT_eSprite* halo = nullptr;       // optional white halo (text dilated by 2 px)
  int x = 0, y = 0, w = 0, h = 0;
  char cache[64] = {0};
};
enum Al : uint8_t { AL_L, AL_C, AL_R };

TextSpr tBootT, tBootL, tBootV;                              // boot
TextSpr tIdleT, tStat, tHint;                                // idle
TextSpr tPTitle, tPTag, tPLbl, tPDig, tPH1, tPH2;            // pairing
TextSpr tBatt, tBtLbl, tVerLbl, tFps;                        // status bar
TextSpr tDist, tStreet, tBig, tSpeed, tUnit, tDir, tPill;    // navigation
TextSpr tUpLbl, tUpPct, tUpKB, tUpVer, tUpHint, tFailT, tFailE, tFailH;   // firmware update

TFT_eSprite* spLogoBig = nullptr;
TFT_eSprite* spLogoSmall = nullptr;
TFT_eSprite* spIcon[11];
TFT_eSprite* spSpin[8];
TFT_eSprite* spChev = nullptr;       // vehicle chevron (rotated on demand)
TFT_eSprite* spCompass = nullptr;
TFT_eSprite* spCheck = nullptr;      // white disc with check (arrived)
TFT_eSprite* spDash = nullptr;       // dashed accuracy ring (no GPS)
TFT_eSprite* spHatch = nullptr;      // hatched strip under the banner
TFT_eSprite* spLock = nullptr;

void mk(TextSpr& t, int x, int y, int w, int h) { mkSprite(t.sp, w, h); t.x = x; t.y = y; t.w = w; t.h = h; }
void mkHalo(TextSpr& t) { mkSprite(t.halo, t.w, t.h); }

const Glyph* gly(const BF& f, unsigned char c) {
  if (c < f.first || c > f.last) return nullptr;
  const Glyph* g = &f.g[c - f.first];
  return (g->adv || g->w) ? g : nullptr;
}
// Width of `s`, tracking applied between glyphs only.
int textW(const BF& f, const char* s, int tr) {
  int w = 0, n = 0;
  for (; *s; s++) { const Glyph* g = gly(f, (unsigned char)*s); if (!g) continue; w += g->adv + tr; n++; }
  return n ? w - tr : 0;
}
// Draw one run into the sprite; returns the x after the last glyph (tracking included).
int put(TextSpr& t, const BF& f, const char* s, int x, int base, int tr) {
  uint8_t* p = rawBits(t.sp);
  if (!p) return x;
  for (; *s; s++) {
    const Glyph* g = gly(f, (unsigned char)*s);
    if (!g) continue;
    const uint8_t* b = f.bmp + g->off;
    int bit = 0;
    for (int r = 0; r < g->h; r++)
      for (int c = 0; c < g->w; c++, bit++)
        if (b[bit >> 3] & (0x80 >> (bit & 7))) setBit(p, t.w, t.h, x + g->xo + c, base + g->yo + r);
    x += g->adv + tr;
  }
  return x;
}

void rebuildHalo(TextSpr& t) {
  if (!t.halo) return;
  t.halo->fillSprite(0);
  const uint8_t* s = rawBits(t.sp);
  uint8_t* d = rawBits(t.halo);
  for (int y = 0; y < t.h; y++)
    for (int x = 0; x < t.w; x++)
      if (getBit(s, t.w, t.h, x, y))
        for (int dy = -2; dy <= 2; dy++)
          for (int dx = -2; dx <= 2; dx++) setBit(d, t.w, t.h, x + dx, y + dy);
}

// Start / finish a re-render. The renderer compares commands, not bitmap contents, so a changed
// sprite has to be flagged by hand.
bool begin(TextSpr& t, const char* key) {
  if (!t.sp || strncmp(t.cache, key, sizeof(t.cache) - 1) == 0) return false;
  strncpy(t.cache, key, sizeof(t.cache) - 1); t.cache[sizeof(t.cache) - 1] = 0;
  t.sp->fillSprite(0);
  return true;
}
void finish(TextSpr& t) {
  rebuildHalo(t);
  R->markDirtyRect(t.x, t.y, t.x + t.w - 1, t.y + t.h - 1);
}

// Single-run text. `ox` is a margin from the aligned edge.
bool setText(TextSpr& t, const char* s, const BF& f, Al a, int base, int tr = 0, int ox = 0) {
  if (!begin(t, s)) return false;
  int w = textW(f, s, tr);
  int x = a == AL_L ? ox : (a == AL_C ? (t.w - w) / 2 : t.w - w - ox);
  put(t, f, s, x, base, tr);
  finish(t);
  return true;
}

// Never overflows the sprite: big font, then small, then "..." truncation (small).
bool setFit(TextSpr& t, const char* s, const BF& big, const BF& small, int base) {
  if (!begin(t, s)) return false;
  char buf[64];
  strncpy(buf, s, sizeof(buf) - 4); buf[sizeof(buf) - 4] = 0;
  const BF* f = &big;
  if (textW(big, buf, 0) > t.w) {
    f = &small;
    if (textW(small, buf, 0) > t.w) {
      size_t n = strlen(buf);
      while (n > 1) {
        n--;
        char tmp[64];
        memcpy(tmp, buf, n); strcpy(tmp + n, "...");
        if (textW(small, tmp, 0) <= t.w) { strcpy(buf, tmp); break; }
      }
    }
  }
  put(t, *f, buf, 0, base, 0);
  finish(t);
  return true;
}

void blit(const TextSpr& t, Mode m) {
  const uint8_t* p = ptr(t.sp);
  if (p) R->ovBitmap(t.x, t.y, t.w, t.h, p, m);
}
void blitHalo(const TextSpr& t) {          // white halo, then black text
  const uint8_t* ph = ptr(t.halo);
  if (ph) R->ovBitmap(t.x, t.y, t.w, t.h, ph, M_CLR);
  blit(t, M_SET);
}

// ---- Rounded rect (rect + 4 corner discs) --------------------------------------------------------
void fillRR(int x, int y, int w, int h, int r, Mode m) {
  if (r > w / 2) r = w / 2;
  if (r > h / 2) r = h / 2;
  if (r < 1) { R->ovFillRect(x, y, w, h, m); return; }
  R->ovFillRect(x + r, y, w - 2 * r, h, m);
  R->ovFillRect(x, y + r, w, h - 2 * r, m);
  R->ovFillCircle(x + r, y + r, r, m);
  R->ovFillCircle(x + w - 1 - r, y + r, r, m);
  R->ovFillCircle(x + r, y + h - 1 - r, r, m);
  R->ovFillCircle(x + w - 1 - r, y + h - 1 - r, r, m);
}

// ---- Icon drawing (sprites, authored on a 72 grid, stroke 7 px, round caps) ----------------------
void thickLine(TFT_eSprite* s, float x0, float y0, float x1, float y1, int r) {
  float dx = x1 - x0, dy = y1 - y0;
  int n = (int)ceilf(fmaxf(fabsf(dx), fabsf(dy)));
  if (n < 1) n = 1;
  for (int i = 0; i <= n; i++) {
    float t = (float)i / n;
    s->fillCircle(lroundf(x0 + dx * t), lroundf(y0 + dy * t), r, 1);
  }
}

struct IconCtx {
  TFT_eSprite* s; float ca, sa; float px[96], py[96]; int n; float lx, ly;
  IconCtx(TFT_eSprite* sp, float rotDeg = 0) : s(sp), n(0), lx(0), ly(0) {
    float a = rotDeg * 0.017453292f; ca = cosf(a); sa = sinf(a);
  }
  void add(float x, float y) {
    lx = x; ly = y;
    if (n >= 96) return;
    float dx = x - 36, dy = y - 36;
    px[n] = 36 + dx * ca - dy * sa; py[n] = 36 + dx * sa + dy * ca; n++;
  }
  void mv(float x, float y) { stroke(); add(x, y); }
  void ln(float x, float y) { add(x, y); }
  void quad(float cx, float cy, float x, float y) {
    float x0 = lx, y0 = ly;
    for (int i = 1; i <= 8; i++) {
      float t = i / 8.f, u = 1 - t;
      add(u * u * x0 + 2 * u * t * cx + t * t * x, u * u * y0 + 2 * u * t * cy + t * t * y);
    }
  }
  void cubic(float c1x, float c1y, float c2x, float c2y, float x, float y) {
    float x0 = lx, y0 = ly;
    for (int i = 1; i <= 10; i++) {
      float t = i / 10.f, u = 1 - t;
      add(u*u*u*x0 + 3*u*u*t*c1x + 3*u*t*t*c2x + t*t*t*x, u*u*u*y0 + 3*u*u*t*c1y + 3*u*t*t*c2y + t*t*t*y);
    }
  }
  void arc(float cx, float cy, float r, float a0, float a1) {   // degrees, 0 = right, 90 = up
    int steps = (int)fabsf(a1 - a0) / 8 + 2;
    for (int i = 0; i <= steps; i++) {
      float a = (a0 + (a1 - a0) * i / steps) * 0.017453292f;
      add(cx + r * cosf(a), cy - r * sinf(a));
    }
  }
  void stroke() {
    if (n == 1) s->fillCircle(lroundf(px[0]), lroundf(py[0]), 3, 1);
    for (int i = 0; i + 1 < n; i++) thickLine(s, px[i], py[i], px[i + 1], py[i + 1], 3);
    n = 0;
  }
  void head(float tx, float ty, float dx, float dy) {          // open chevron head at tip, direction d
    const float L = 12;
    float bx = tx - dx * L, by = ty - dy * L, qx = -dy, qy = dx;
    mv(bx + qx * L, by + qy * L); ln(tx, ty); ln(bx - qx * L, by - qy * L); stroke();
  }
};

void iconPaths(IconCtx& c, int kind) {
  const float S2 = 0.70711f;
  switch (kind) {
    case 1:  c.mv(36, 64); c.ln(36, 12); c.stroke(); c.head(36, 12, 0, -1); break;                    // straight
    case 2:  c.mv(46, 64); c.ln(46, 40); c.quad(46, 28, 34, 28); c.ln(16, 28); c.stroke();            // left
             c.head(14, 28, -1, 0); break;
    case 3:  c.mv(26, 64); c.ln(26, 40); c.quad(26, 28, 38, 28); c.ln(56, 28); c.stroke();            // right
             c.head(58, 28, 1, 0); break;
    case 4:  c.mv(42, 64); c.ln(42, 42); c.ln(24, 20); c.stroke(); c.head(22, 18, -0.6f, -0.8f); break;  // slight left
    case 5:  c.mv(30, 64); c.ln(30, 42); c.ln(48, 20); c.stroke(); c.head(50, 18, 0.6f, -0.8f); break;   // slight right
    case 6:  c.mv(46, 64); c.ln(46, 28); c.ln(22, 52); c.stroke(); c.head(20, 54, -S2, S2); break;       // sharp left
    case 7:  c.mv(26, 64); c.ln(26, 28); c.ln(50, 52); c.stroke(); c.head(52, 54, S2, S2); break;        // sharp right
    case 8:  c.mv(52, 64); c.ln(52, 28); c.arc(36, 28, 16, 0, 180); c.ln(20, 50); c.stroke();            // u-turn
             c.head(20, 52, 0, 1); break;
    case 9:  c.mv(36 + 15, 40); c.arc(36, 40, 15, 0, -360); c.stroke();                                  // roundabout
             c.mv(36, 66); c.ln(36, 56); c.stroke();
             c.mv(36, 25); c.ln(36, 10); c.stroke(); c.head(36, 8, 0, -1); break;
    case 10: c.mv(36, 66); c.cubic(22, 50, 14, 40, 14, 30); c.arc(36, 30, 22, 180, 0);                   // arrive: pin
             c.cubic(58, 40, 50, 50, 36, 66); c.stroke(); break;
  }
}

void buildIcons() {
  for (int i = 0; i < 11; i++) {
    mkSprite(spIcon[i], IC, IC);
    if (i == 0) continue;
    IconCtx c(spIcon[i]);
    iconPaths(c, i);
    if (i == 10) { spIcon[i]->fillCircle(36, 30, 11, 1); spIcon[i]->fillCircle(36, 30, 5, 0); }
  }
  for (int k = 0; k < 8; k++) {                              // reroute: two arrows chasing each other
    mkSprite(spSpin[k], IC, IC);
    IconCtx c(spSpin[k], k * 22.5f);
    c.mv(14, 36); c.arc(36, 36, 22, 180, 0); c.stroke();
    c.mv(51, 29); c.ln(58, 37); c.ln(65, 29); c.stroke();
    c.mv(58, 36); c.arc(36, 36, 22, 0, -180); c.stroke();
    c.mv(7, 43); c.ln(14, 35); c.ln(21, 43); c.stroke();
  }
}

void buildLogo(TFT_eSprite*& sp, int S) {
  mkSprite(sp, S, S);
  const float k = S / 96.f;
  sp->fillCircle(S / 2, S / 2, lroundf(46 * k), 1);
  auto P = [&](float x, float y, int& ox, int& oy) { ox = lroundf(x * k); oy = lroundf(y * k); };
  int ax, ay, bx, by, cx, cy, dx, dy;
  P(48, 16, ax, ay); P(72, 78, bx, by); P(48, 63, cx, cy); P(24, 78, dx, dy);
  sp->fillTriangle(ax, ay, bx, by, cx, cy, 0);
  sp->fillTriangle(ax, ay, cx, cy, dx, dy, 0);
}

void buildMisc() {
  mkSprite(spChev, 28, 28);
  mkSprite(spCompass, 44, 44);
  // white disc with the check carved out (arrived)
  mkSprite(spCheck, 44, 44);
  spCheck->fillCircle(22, 22, 20, 1);
  {
    const float pts[3][2] = {{12, 23}, {19, 30}, {33, 14}};
    for (int q = 0; q < 2; q++) {
      float x0 = pts[q][0], y0 = pts[q][1], x1 = pts[q + 1][0], y1 = pts[q + 1][1];
      int n = (int)ceilf(fmaxf(fabsf(x1 - x0), fabsf(y1 - y0))) + 1;
      for (int i = 0; i <= n; i++) {
        float t = (float)i / n;
        spCheck->fillCircle(lroundf(x0 + (x1 - x0) * t), lroundf(y0 + (y1 - y0) * t), 2, 0);
      }
    }
  }
  // dashed accuracy ring 48x48 (r 22, 2 px, dash 4 / gap 4)
  mkSprite(spDash, 48, 48);
  {
    uint8_t* p = rawBits(spDash);
    for (int a = 0; a < 1440; a++) {
      float ang = a * 0.25f * 0.017453292f, s = 22.f * ang;
      if (((int)(s / 4.f)) & 1) continue;
      for (int rr = 21; rr <= 22; rr++)
        setBit(p, 48, 48, lroundf(24 + rr * cosf(ang)), lroundf(24 + rr * sinf(ang)));
    }
  }
  // hatch strip 480x30: 135-degree 1 px lines
  mkSprite(spHatch, SCREEN_W, 30);
  {
    uint8_t* p = rawBits(spHatch);
    for (int y = 0; y < 30; y++)
      for (int x = 0; x < SCREEN_W; x++)
        if (((x + y) % 6) == 0) setBit(p, SCREEN_W, 30, x, y);
  }
  // padlock 14x16
  mkSprite(spLock, 14, 16);
  {
    uint8_t* p = rawBits(spLock);
    for (int y = 7; y <= 14; y++) for (int x = 1; x <= 12; x++) setBit(p, 14, 16, x, y);
    for (int y = 1; y <= 6; y++)
      for (int x = 0; x < 14; x++) {
        float d = hypotf(x + 0.5f - 7.f, y + 0.5f - 5.f);
        bool arc = y <= 5 && d >= 2.f && d <= 4.2f;
        bool leg = y >= 5 && (x == 3 || x == 4 || x == 9 || x == 10);
        if (arc || leg) setBit(p, 14, 16, x, y);
      }
  }
}

// Vehicle chevron, pointing `deg` clockwise from up (quantized; rebuilt only when it changes).
int chevQ = -999;
void updateChevron(float deg) {
  int q = (int)lroundf(deg / 6.f);
  if (q == chevQ) return;
  chevQ = q;
  float a = q * 6.f * 0.017453292f, ca = cosf(a), sa = sinf(a);
  spChev->fillSprite(0);
  const float P[4][2] = {{0, -12}, {9, 7}, {0, 1}, {-9, 7}};
  int X[4], Y[4];
  for (int i = 0; i < 4; i++) { X[i] = lroundf(14 + P[i][0] * ca - P[i][1] * sa); Y[i] = lroundf(14 + P[i][0] * sa + P[i][1] * ca); }
  spChev->fillTriangle(X[0], Y[0], X[1], Y[1], X[2], Y[2], 1);
  spChev->fillTriangle(X[0], Y[0], X[3], Y[3], X[2], Y[2], 1);
  R->markDirtyRect(ANCHOR_X - 14, ANCHOR_Y - 14, ANCHOR_X + 13, ANCHOR_Y + 13);
}

// Compass needle: white north needle + south tick, rotated so the needle points to north on screen.
int compQ = -999;
void updateCompass(float headingDeg, int bx, int by) {
  int q = (int)lroundf(headingDeg / 4.f);
  if (q == compQ) return;
  compQ = q;
  float a = -q * 4.f * 0.017453292f, ca = cosf(a), sa = sinf(a);
  spCompass->fillSprite(0);
  spCompass->fillCircle(22, 22, 21, 1);
  spCompass->fillCircle(22, 22, 19, 0);
  auto X = [&](float x, float y) { return lroundf(22 + x * ca - y * sa); };
  auto Y = [&](float x, float y) { return lroundf(22 + x * sa + y * ca); };
  spCompass->fillTriangle(X(0, -16), Y(0, -16), X(5.5f, 1), Y(5.5f, 1), X(0, -2.5f), Y(0, -2.5f), 1);
  spCompass->fillTriangle(X(0, -16), Y(0, -16), X(-5.5f, 1), Y(-5.5f, 1), X(0, -2.5f), Y(0, -2.5f), 1);
  thickLine(spCompass, X(0, 4), Y(0, 4), X(0, 12), Y(0, 12), 1);
  R->markDirtyRect(bx, by, bx + 43, by + 43);
}

// ---- Number formatting -----------------------------------------------------------------------------
// Splits into number + unit ("120", "m") so they get two type sizes.
void fmtDist(uint16_t m, bool imp, char* num, char* unit, size_t n) {
  num[0] = unit[0] = 0;
  if (m == 0xFFFF) return;
  if (!imp) {
    if (m < 995)       { snprintf(num, n, "%u", (unsigned)(((m + 5) / 10) * 10)); snprintf(unit, n, "m"); }
    else if (m < 9950) { snprintf(num, n, "%.1f", m / 1000.0f);                   snprintf(unit, n, "km"); }
    else               { snprintf(num, n, "%u", (unsigned)((m + 500) / 1000));   snprintf(unit, n, "km"); }
  } else {
    float ft = m * 3.28084f;
    if (ft < 528.f) { snprintf(num, n, "%d", ((int)(ft + 25) / 50) * 50); snprintf(unit, n, "ft"); }
    else {
      float mi = m / 1609.344f;
      if (mi < 10.f) snprintf(num, n, "%.1f", mi); else snprintf(num, n, "%d", (int)(mi + 0.5f));
      snprintf(unit, n, "mi");
    }
  }
}

// ---- Shared pieces (OVERLAY) ---------------------------------------------------------------------------
// Battery 32x14: 28x14 body (2 px outline), nub, 4 segments.
void emitBattery(int bx, int by, int pct, uint32_t now) {
  R->ovRect(bx, by, 28, 14, M_SET);
  R->ovRect(bx + 1, by + 1, 26, 12, M_SET);
  R->ovFillRect(bx + 29, by + 4, 3, 6, M_SET);
  bool lowBlink = UI_ANIMATIONS && pct >= 0 && pct <= 15 && ((now / 500) & 1);
  if (lowBlink || pct <= 0) return;
  int seg = (pct + 24) / 25; if (seg > 4) seg = 4;
  static const int8_t SX[4] = {4, 9, 15, 20};
  for (int i = 0; i < seg; i++) R->ovFillRect(bx + SX[i], by + 4, 4, 6, M_SET);
}

void emitBarFrame() {
  R->ovFillRect(0, BOT_Y, SCREEN_W, BOT_H, M_CLR);
  R->ovFillRect(0, BOT_Y, SCREEN_W, 2, M_SET);
}

void setBattText(const Hud& h) {
  char b[12];
  if (h.battPct < 0) snprintf(b, sizeof(b), "--"); else snprintf(b, sizeof(b), "%d%%", h.battPct);
  setText(tBatt, b, B14, AL_L, 17);
}

// Bluetooth dot: ring while waiting, solid when linked.
void emitBtDot(int cx, int cy, bool linked) {
  R->ovFillCircle(cx, cy, 5, M_SET);
  if (!linked) R->ovFillCircle(cx, cy, 3, M_CLR);
}

// Idle / pairing / update status bar. `withVer`: add the divider + version on the right.
void emitStatusBar(const Hud& h, bool withVer) {
  emitBarFrame();
  emitBattery(20, BOT_Y + 12, h.battPct, h.nowMs);
  setBattText(h);
  tBatt.x = 62; tBatt.y = BOT_Y + 7;
  blit(tBatt, M_SET);

  const int by = BOT_Y + 9;                                  // text sprites' top (baseline at BOT_Y+23)
  char v[24]; snprintf(v, sizeof(v), "V%s", FW_VERSION_STRING);
  setText(tBtLbl, "BLUETOOTH", FB11, AL_L, 14, 2);
  setText(tVerLbl, v, FB11, AL_L, 14, 2);
  int wB = textW(FB11, "BLUETOOTH", 2) + 2;
  int cx, labelEnd;
  if (withVer) {
    int wV = textW(FB11, v, 2) + 2;
    int verX = 460 - wV;
    int divX = verX - 11;
    tVerLbl.x = verX; tVerLbl.y = by;
    blit(tVerLbl, M_SET);
    R->ovFillRect(divX, BOT_Y + 12, 1, 14, M_SET);
    cx = divX - 15;
    labelEnd = divX - 30;
  } else {
    cx = 455;
    labelEnd = 440;
  }
  emitBtDot(cx, BOT_Y + 19, h.connected);
  tBtLbl.x = labelEnd - wB; tBtLbl.y = by;
  blit(tBtLbl, M_SET);
}

void emitFps(const Hud& h, bool show, int x, int y, bool halo) {
  if (!show) return;
  char b[40]; snprintf(b, sizeof(b), "%.0f fps  %d kpx", h.fps, h.kpx);
  tFps.x = x; tFps.y = y;
  setText(tFps, b, M14, AL_C, 14);
  if (halo) blitHalo(tFps); else blit(tFps, M_SET);
}

// ---- Map -----------------------------------------------------------------------------------------------
bool clipLine(float& x0, float& y0, float& x1, float& y1, float xmin, float ymin, float xmax, float ymax) {
  float dx = x1 - x0, dy = y1 - y0, t0 = 0, t1 = 1;
  struct T { static bool test(float p, float q, float& a, float& b) {
    if (p == 0) return q >= 0;
    float t = q / p;
    if (p < 0) { if (t > b) return false; if (t > a) a = t; }
    else       { if (t < a) return false; if (t < b) b = t; }
    return true; } };
  if (!T::test(-dx, x0 - xmin, t0, t1) || !T::test(dx, xmax - x0, t0, t1) ||
      !T::test(-dy, y0 - ymin, t0, t1) || !T::test(dy, ymax - y0, t0, t1)) return false;
  float nx0 = x0 + t0 * dx, ny0 = y0 + t0 * dy, nx1 = x0 + t1 * dx, ny1 = y0 + t1 * dy;
  x0 = nx0; y0 = ny0; x1 = nx1; y1 = ny1;
  return true;
}

void clearMap() { R->baseFillRect(0, 0, SCREEN_W, SCREEN_H, M_CLR); }

void baseSeg(float x0, float y0, float x1, float y1, int w) {
  if (!clipLine(x0, y0, x1, y1, -8, -8, SCREEN_W + 7, SCREEN_H + 7)) return;
  R->baseLine(lroundf(x0), lroundf(y0), lroundf(x1), lroundf(y1), w, M_SET);
}

// Round dots every 9 px (`next` carries the phase across segments).
void dottedSeg(float x0, float y0, float x1, float y1, float& next) {
  float dx = x1 - x0, dy = y1 - y0, L = sqrtf(dx * dx + dy * dy);
  if (L < 0.01f) return;
  float d = next;
  while (d <= L) {
    float x = x0 + dx * d / L, y = y0 + dy * d / L;
    if (x > -6 && x < SCREEN_W + 6 && y > -6 && y < SCREEN_H + 6) R->baseFillCircle(lroundf(x), lroundf(y), 2, M_SET);
    d += 9.f;
  }
  next = d - L;
}

// The next maneuver is PINNED to a point on the route (global segment index + fraction), not recomputed from
// distToTurn every frame. distToTurn is only used to pick that point, and to re-pick it when it disagrees by more
// than TURN_RESYNC_M (maneuver advanced, reroute, new route). If the point lies within TURN_SNAP_M of a waypoint it
// snaps onto it (normal case: the turn IS a waypoint); otherwise it is interpolated on the segment.
const float    TURN_RESYNC_M = 30.f;
const float    TURN_SNAP_M   = 12.f;
const uint32_t TURN_WAIT_MS  = 3000;
uint32_t turnIdx = 0;  float turnT = 0;  uint8_t turnRoute = 255;  bool turnOk = false;
// After the vehicle passes the pinned point the phone still reports the old (tiny) distance for a moment: hold
// (no node, thin road) until the maneuver or the distance changes, or TURN_WAIT_MS passes.
bool     turnWait = false;  uint8_t waitIcon = 0, waitRoute = 255;  uint16_t waitD = 0;  uint32_t waitMs = 0;
// Odometer: route arc length (m) at vertex odoIdx. Gives the dotted trail a phase tied to the ROUTE, so the dots
// stay put on the map while you drive instead of crawling whenever the first visible waypoint changes.
double   odoM = 0;  uint32_t odoIdx = 0;  uint8_t odoRoute = 255;  bool odoOk = false;
const float DOT_PX = 9.f;

// Heading-up projection:
//   sx = AX + s*(e*cos(t) - n*sin(t)),  sy = AY - s*(e*sin(t) + n*cos(t))
// Only points near the cursor are projected, so cost does not depend on the route length.
//   - behind the cursor: dotted trail
//   - up to the next turn: 10 px; after it: 6 px; the turn gets a node; the end gets a destination ring
void drawRoute(const NavModel& m, float thetaDeg, float scale, bool arrived) {
  const RouteRing& r = *m.active;
  if (r.count() < 2) return;
  const int32_t ix = (int32_t)lround(m.pose.x()), iy = (int32_t)lround(m.pose.y());
  const float th = thetaDeg * 0.017453292f, c = cosf(th), s = sinf(th), k = scale * 0.1f;   // px per dm

  struct P { static void proj(const Pt& p, int32_t ix, int32_t iy, float k, float c, float s, float& sx, float& sy) {
    float e = (float)(p.x - ix) * k, n = (float)(p.y - iy) * k;
    sx = ANCHOR_X + (e * c - n * s);
    sy = ANCHOR_Y - (e * s + n * c);
  } };
  auto isFar = [](float x, float y) { float dx = x - ANCHOR_X, dy = y - ANCHOR_Y; return dx * dx + dy * dy > CULL_R * CULL_R; };

  uint32_t c0 = m.cursor;
  if (c0 < r.tail()) c0 = r.tail();
  if (c0 >= r.head()) c0 = r.head() - 1;

  uint32_t lo = c0;
  for (int i = 0; i < BACK_MAX && lo > r.tail(); i++) {
    lo--;
    float x, y; P::proj(r.at(lo), ix, iy, k, c, s, x, y);
    if (isFar(x, y)) break;
  }

  static float PX[BACK_MAX + FWD_MAX + 2], PY[BACK_MAX + FWD_MAX + 2];
  int n = 0;
  bool reachedEnd = false;
  for (uint32_t i = lo; i < r.head() && n < BACK_MAX + FWD_MAX + 1; i++) {
    P::proj(r.at(i), ix, iy, k, c, s, PX[n], PY[n]);
    n++;
    if (i + 1 == r.head()) reachedEnd = true;
    if (i > c0 && isFar(PX[n - 1], PY[n - 1])) break;
  }
  const int ic = (int)(c0 - lo);

  if (n < 2) return;

  auto gLen = [&](uint32_t gi) {                             // length (m) of the segment gi -> gi+1
    const Pt& p = r.at(gi); const Pt& q = r.at(gi + 1);
    const float ddx = (float)(q.x - p.x), ddy = (float)(q.y - p.y);
    return sqrtf(ddx * ddx + ddy * ddy) * 0.1f;              // dm -> m
  };
  auto lenM = [&](int a) { return gLen(lo + (uint32_t)a); };

  // Odometer: arc length at vertex `lo`, accumulated incrementally (walks a few vertices per redraw).
  if (!odoOk || odoRoute != r.id() || odoIdx < r.tail() || odoIdx >= r.head()) {
    odoOk = true; odoRoute = r.id(); odoIdx = lo; odoM = 0;
  } else {
    while (odoIdx < lo) { odoM += gLen(odoIdx); odoIdx++; }
    while (odoIdx > lo) { odoIdx--; odoM -= gLen(odoIdx); }
  }

  // Vehicle position ON the route. The cursor is only the nearest waypoint, so starting the line there made it
  // jump from waypoint to waypoint. Project the marker (ANCHOR) onto the neighbouring segments instead:
  // segment sj..sj+1, fraction st. The trail ends and the route starts at S, the point under the marker.
  auto segT = [&](int a) -> float {
    const float ex = PX[a + 1] - PX[a], ey = PY[a + 1] - PY[a], L2 = ex * ex + ey * ey;
    if (L2 < 1e-4f) return 0.f;
    return ((ANCHOR_X - PX[a]) * ex + (ANCHOR_Y - PY[a]) * ey) / L2;
  };
  int sj = ic; float st = 0.f;
  if (ic + 1 < n) {
    st = segT(ic);
    if (st < 0.f && ic >= 1) { sj = ic - 1; st = segT(sj); }
  } else { sj = ic - 1; st = 1.f; }                          // cursor is the last point
  if (sj < 0) sj = 0;
  if (st < 0.f) st = 0.f;
  if (st > 1.f) st = 1.f;
  const float sx = PX[sj] + st * (PX[sj + 1] - PX[sj]);
  const float sy = PY[sj] + st * (PY[sj + 1] - PY[sj]);

  // Dotted trail: oldest visible point -> marker. Dots sit at fixed route distances (multiples of DOT_PX).
  float next = 0;
  {
    const float ph = fmodf((float)fmod(odoM * scale, (double)DOT_PX), DOT_PX);
    next = ph > 0.001f ? DOT_PX - ph : 0.f;
  }
  for (int q = 0; q < sj; q++) dottedSeg(PX[q], PY[q], PX[q + 1], PY[q + 1], next);
  dottedSeg(PX[sj], PY[sj], sx, sy, next);

  // Route distance (m) from the marker to every projected vertex ahead of it.
  static float ACC[BACK_MAX + FWD_MAX + 2];
  ACC[sj + 1] = (1.f - st) * lenM(sj);
  for (int v = sj + 2; v < n; v++) ACC[v] = ACC[v - 1] + lenM(v - 1);
  // Route distance (m) from the marker to the point at fraction t of segment a (a >= sj, a <= n-1).
  auto arcTo = [&](int a, float t) -> float {
    if (a <= sj) return (t - st) * lenM(sj);
    return ACC[a] + (a < n - 1 ? t * lenM(a) : 0.f);
  };

  // ---- next maneuver node ----
  int mode = 0;                                              // 0 all wide (unknown / not visible), 1 node, 2 thin (just passed)
  int na = -1; float nodeX = 0, nodeY = 0;
  if (m.distToTurn == 0xFFFF || arrived) {
    turnOk = false; turnWait = false;
  } else {
    const float   d   = (float)m.distToTurn;
    const uint8_t rid = r.id();

    if (turnOk && turnRoute == rid && !turnWait) {           // vehicle went past the pinned point?
      const int a0 = (int)(int32_t)(turnIdx - lo);
      if (a0 < sj || (a0 == sj && turnT <= st)) {
        turnOk = false; turnWait = true;
        waitIcon = m.icon; waitD = m.distToTurn; waitMs = millis(); waitRoute = rid;
      }
    }
    if (turnWait && (waitRoute != rid || m.icon != waitIcon || d > waitD + TURN_RESYNC_M ||
                     millis() - waitMs > TURN_WAIT_MS)) turnWait = false;

    if (turnWait) {
      mode = 2;
    } else {
      bool have = turnOk && turnRoute == rid;
      if (have) {                                            // still consistent with the phone's distance?
        const int a0 = (int)(int32_t)(turnIdx - lo);
        if (a0 < sj) have = false;
        else if (a0 <= n - 1) { if (fabsf(arcTo(a0, turnT) - d) > TURN_RESYNC_M) have = false; }
        else if (ACC[n - 1] > d + TURN_RESYNC_M) have = false;   // should be visible by now
      }
      if (!have) {
        turnOk = false;
        if (d <= ACC[n - 1]) {                               // inside the visible part: find its segment
          for (int q = sj; q < n - 1; q++) {
            if (d > ACC[q + 1]) continue;
            const float L = lenM(q);
            float f = L < 1e-3f ? 0.f : (q == sj ? st + d / L : (d - ACC[q]) / L);
            if (f < 0.f) f = 0.f;
            if (f > 1.f) f = 1.f;
            int a = q;
            if (q > sj && f * L <= TURN_SNAP_M) f = 0.f;                       // snap to the waypoint behind
            else if ((1.f - f) * L <= TURN_SNAP_M) { a = q + 1; f = 0.f; }    // snap to the waypoint ahead
            turnIdx = lo + (uint32_t)a; turnT = f; turnRoute = rid; turnOk = true; have = true;
            break;
          }
        }
      }
      if (have) {
        const int a = (int)(int32_t)(turnIdx - lo);
        if (a >= sj && a <= n - 1) {
          if (a >= n - 1) { nodeX = PX[n - 1]; nodeY = PY[n - 1]; }
          else { nodeX = PX[a] + turnT * (PX[a + 1] - PX[a]); nodeY = PY[a] + turnT * (PY[a + 1] - PY[a]); }
          na = a; mode = 1;
        }
      }
    }
  }

  // Route ahead: marker -> end. 10 px up to the node, 6 px after it.
  float fx = sx, fy = sy;
  for (int v = sj + 1; v < n; v++) {
    const int a = v - 1;                                     // segment a -> v
    if (mode == 1 && a == na) {
      baseSeg(fx, fy, nodeX, nodeY, ROUTE_W);
      baseSeg(nodeX, nodeY, PX[v], PY[v], ROUTE_W2);
    } else {
      const int w = (mode == 2 || (mode == 1 && a > na)) ? ROUTE_W2 : ROUTE_W;
      baseSeg(fx, fy, PX[v], PY[v], w);
    }
    fx = PX[v]; fy = PY[v];
  }
  if (mode == 1) {                                           // node, fixed on the route
    const int nx = lroundf(nodeX), ny = lroundf(nodeY);
    R->baseFillCircle(nx, ny, 12, M_SET);
    R->baseFillCircle(nx, ny, 8, M_CLR);
    R->baseFillCircle(nx, ny, 3, M_SET);
  }
  if (reachedEnd) {                                          // destination
    int ex = lroundf(PX[n - 1]), ey = lroundf(PY[n - 1]);
    R->baseFillCircle(ex, ey, 18, M_SET);
    R->baseFillCircle(ex, ey, 13, M_CLR);
    R->baseFillCircle(ex, ey, 7, M_SET);
  }
}

void emitMarker(float dx, float dy, uint32_t now, bool stale) {
  const int cx = ANCHOR_X, cy = ANCHOR_Y;
  if (stale) {
    R->ovBitmap(cx - 24, cy - 24, 48, 48, ptr(spDash), M_SET);          // dashed accuracy ring
    if ((now / 500) & 1) return;                                        // marker blinks
    R->ovFillCircle(cx, cy, 19, M_CLR);
    R->ovFillCircle(cx, cy, 12, M_SET);
    R->ovFillCircle(cx, cy, 8, M_CLR);
    R->ovBitmap(cx - 14, cy - 14, 28, 28, ptr(spChev), M_SET);
    return;
  }
  R->ovCircle(cx, cy, 22, M_SET);                                       // accuracy ring
  R->ovCircle(cx, cy, 21, M_SET);
#if UI_ANIMATIONS
  int step = (int)(((now % 2200) * 8) / 2200);                          // sonar ping, 8 steps
  if (step < 6) R->ovCircle(cx, cy, 22 + 3 * (step + 1), M_SET);
#else
  (void)now;
#endif
  R->ovFillCircle(cx, cy, 19, M_CLR);                                   // halo over the route
  R->ovFillCircle(cx, cy, 14, M_SET);
  R->ovBitmap(cx - 14, cy - 14, 28, 28, ptr(spChev), M_CLR);            // white chevron
  (void)dx; (void)dy;
}

struct ViewKey { int32_t qx, qy, qh; uint32_t rev; uint16_t zoom; uint32_t dt; uint8_t hu, fix, st; };
ViewKey lastKey; bool keyValid = false;

const char* cardinal(float h) {
  static const char* N[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  h = fmodf(h, 360.f); if (h < 0) h += 360.f;
  return N[((int)((h + 22.5f) / 45.f)) & 7];
}

}  // namespace

// =============================================================================================
// Boot
// =============================================================================================
void initBoot(TFT_eSPI* t, Renderer* r) {
  tft = t; R = r;
  buildLogo(spLogoBig, LOGO_BIG);
  mk(tBootT, 0, 150, SCREEN_W, 56);
  mk(tBootL, 0, 256, SCREEN_W, 18);
  mk(tBootV, 0, 292, SCREEN_W, 18);
  setText(tBootT, "TahVia", B42, AL_C, 40, -1);
  char v[24]; snprintf(v, sizeof(v), "V%s", FW_VERSION_STRING);
  setText(tBootV, v, FB11, AL_C, 13, 2);
}

void initRest() {
  buildLogo(spLogoSmall, LOGO_SMALL);
  // idle
  mk(tIdleT, 0, 164, SCREEN_W, 44);
  mk(tStat, 0, 214, SCREEN_W, 22);
  mk(tHint, 0, 240, SCREEN_W, 24);
  setText(tIdleT, "TahVia", B32, AL_C, 32, -1);
  // pairing
  mk(tPTitle, 54, 16, 100, 26);
  mk(tPTag, 340, 22, 120, 14);
  mk(tPLbl, 0, 68, SCREEN_W, 20);
  mk(tPDig, 56, 108, 368, 68);
  mk(tPH1, 0, 206, SCREEN_W, 24);
  mk(tPH2, 0, 234, SCREEN_W, 20);
  setText(tPTitle, "TahVia", B17, AL_L, 19);
  setText(tPTag, "PAIRING", FB11, AL_R, 11, 2, 2);
  setText(tPLbl, "PAIRING CODE", B12, AL_C, 14, 2);
  // status bar
  mk(tBatt, 62, BOT_Y + 7, 56, 22);
  mk(tBtLbl, 0, BOT_Y + 9, 100, 16);
  mk(tVerLbl, 0, BOT_Y + 9, 72, 16);
  mk(tFps, 0, 0, 160, 18); mkHalo(tFps);
  // navigation
  mk(tDist, 104, 4, 296, 62);
  mk(tStreet, 104, 64, 296, 26);
  mk(tBig, 104, 6, 296, 58);
  mk(tSpeed, 20, 288, 70, 30);
  mk(tUnit, 0, 300, 52, 16);
  mk(tDir, 0, 293, 36, 16);
  mk(tPill, 0, 100, 160, 18);
  setText(tPill, "NO GPS SIGNAL", FB11, AL_L, 14, 2);
  // firmware update
  mk(tUpLbl, 0, 20, SCREEN_W, 20);
  mk(tUpPct, 60, 52, 360, 88);
  mk(tUpKB, 50, 212, 200, 20);
  mk(tUpVer, 250, 214, 180, 18);
  mk(tUpHint, 0, 244, 360, 22);
  mk(tFailT, 0, 146, SCREEN_W, 44);
  mk(tFailE, 0, 196, SCREEN_W, 26);
  mk(tFailH, 0, 232, SCREEN_W, 20);
  setText(tUpLbl, "FIRMWARE UPDATE", B12, AL_C, 14, 2);
  setText(tFailT, "Update failed", X36, AL_C, 33, -1);
  buildIcons();
  buildMisc();
}

void bootDraw() {
  R->baseClear(M_CLR);
  R->baseBitmap(196, 52, LOGO_BIG, LOGO_BIG, ptr(spLogoBig), M_SET);
  R->baseBitmap(tBootT.x, tBootT.y, tBootT.w, tBootT.h, ptr(tBootT.sp), M_SET);
  R->baseBitmap(tBootV.x, tBootV.y, tBootV.w, tBootV.h, ptr(tBootV.sp), M_SET);
  R->baseFillRect(BAR_X, BAR_Y + 2, BAR_W, 2, M_SET);                  // thin rule; the fill grows over it
}

void bootLabel(const char* s) {
  char u[48]; size_t n = 0;
  for (; s[n] && n < sizeof(u) - 1; n++) u[n] = (s[n] >= 'a' && s[n] <= 'z') ? (char)(s[n] - 32) : s[n];
  u[n] = 0;
  if (!setText(tBootL, u, FB11, AL_C, 13, 2)) return;
  R->baseFillRect(tBootL.x, tBootL.y, tBootL.w, tBootL.h, M_CLR);
  R->baseBitmap(tBootL.x, tBootL.y, tBootL.w, tBootL.h, ptr(tBootL.sp), M_SET);
}

void bootBar(int pct) {
  if (pct > 100) pct = 100;
  int fw = BAR_W * pct / 100;
  if (fw > 0) R->baseFillRect(BAR_X, BAR_Y, fw, 6, M_SET);
}

// =============================================================================================
// Runtime screens
// =============================================================================================
void enter(Screen s) {
  R->baseClear(M_CLR);
  keyValid = false;
  turnOk = false; turnWait = false; odoOk = false;
  compQ = chevQ = -999;
  revealPending = true;
  if (s == SCR_UPDATE) {                                    // static ruler under the progress bar
    R->baseFillRect(49, 182, 382, 2, M_SET);
    for (int i = 0; i <= 20; i++) R->baseFillRect(49 + i * 19, 186, 2, 8, M_SET);
    for (int i = 0; i < 5; i++)   R->baseFillRect(49 + i * 95, 186, 2, 14, M_SET);
  }
}

// ---- Idle / pairing -------------------------------------------------------------------
void idleFrame(const Hud& h) {
  const uint32_t now = h.nowMs;
  char stat[48], hint[56];

  R->overlayBegin();

  if (h.pairing) {
    unsigned long pk = (unsigned long)(h.passkey % 1000000UL);
    char d[8]; snprintf(d, sizeof(d), "%06lu", pk);
    if (begin(tPDig, d)) {                                   // six digits, each centred in a 52 px box
      static const int CX[6] = {26, 86, 146, 222, 282, 342};
      for (int i = 0; i < 6; i++) {
        char one[2] = {d[i], 0};
        put(tPDig, D64, one, CX[i] - textW(D64, one, 0) / 2, 55, 0);
      }
      finish(tPDig);
    }
    setText(tPH1, "Enter this code on your phone", S17, AL_C, 19);
    setText(tPH2, "The code is new every time you connect.", M14, AL_C, 16);

    R->ovBitmap(20, 16, LOGO_SMALL, LOGO_SMALL, ptr(spLogoSmall), M_SET);
    blit(tPTitle, M_SET);
    blit(tPTag, M_SET);
    blit(tPLbl, M_SET);
    blit(tPDig, M_SET);
    static const int UX[6] = {56, 116, 176, 252, 312, 372};
    for (int i = 0; i < 6; i++) R->ovFillRect(UX[i], 178, 52, 4, M_SET);
    blit(tPH1, M_SET);
    blit(tPH2, M_SET);
  } else {
    const int LCX = 240, LCY = 84;
    if (!h.connected) {
      static const char* DOTS[4] = {"", ".", "..", "..."};
#if UI_ANIMATIONS
      const char* dots = DOTS[(now / 350) & 3];
#else
      const char* dots = DOTS[3];
#endif
      snprintf(stat, sizeof(stat), "WAITING FOR PHONE%s", dots);
      if (begin(tStat, stat)) {                              // left aligned at the position of the full text
        int w = textW(B12, "WAITING FOR PHONE...", 2);
        put(tStat, B12, stat, (SCREEN_W - w) / 2, 16, 2);
        finish(tStat);
      }
      snprintf(hint, sizeof(hint), "Open TahviaNav and connect to TahVia");
      for (int i = 0; i < 3; i++) {                          // searching: rings contract and fade in 8 steps
        uint32_t ph = (now + 2400 * 4 - 800 * i) % 2400;
        int step = (int)(ph * 8 / 2400);
        if (step > 4) continue;
        int rr = lroundf(80 * (0.78f + 0.22f * step / 8.f));
        R->ovCircle(LCX, LCY, rr, M_SET);
        R->ovCircle(LCX, LCY, rr - 1, M_SET);
      }
    } else {
      snprintf(stat, sizeof(stat), "PHONE CONNECTED");
      setText(tStat, stat, B12, AL_C, 16, 2);
      snprintf(hint, sizeof(hint), "No route yet. Pick a destination in the app.");
      R->ovCircle(LCX, LCY, 56, M_SET);
      R->ovCircle(LCX, LCY, 55, M_SET);
    }
    setText(tHint, hint, M15, AL_C, 19);
    R->ovBitmap(196, 40, LOGO_BIG, LOGO_BIG, ptr(spLogoBig), M_SET);
    blit(tIdleT, M_SET);
    blit(tStat, M_SET);
    blit(tHint, M_SET);

#if UI_ANIMATIONS
    static bool wasLinked = false, burstOn = false;
    static uint32_t linkT = 0;
    bool linked = h.connected;
    if (linked != wasLinked) { wasLinked = linked; linkT = now; burstOn = true; }
    uint32_t el = now - linkT;
    if (burstOn) {
      if (el >= BURST_MS) burstOn = false;
      else {
        float e = easeOut(el / (float)BURST_MS);
        int r = linked ? 58 + (int)(e * 90) : 148 - (int)(e * 90);
        R->ovCircle(LCX, LCY, r, M_SET);
        R->ovCircle(LCX, LCY, r + 1, M_SET);
      }
    }
    int rb = linked ? (el < POP_MS ? (int)(14 * easeOut(el / (float)POP_MS)) : 14) : 0;
#else
    int rb = h.connected ? 14 : 0;
#endif
    if (rb > 0) {                                            // check badge on the logo
      const int bx = 274, by = 118;
      R->ovFillCircle(bx, by, rb + 4, M_CLR);
      R->ovFillCircle(bx, by, rb, M_SET);
      if (rb >= 11) {
        R->ovLine(bx - 5, by + 1, bx - 1, by + 4, 3, M_CLR);
        R->ovLine(bx - 1, by + 4, bx + 5, by - 4, 3, M_CLR);
      }
    }
  }
  emitStatusBar(h, true);
  emitFps(h, h.showFps, 140, 264, false);
  emitReveal(now);
}

// ---- Navigation -----------------------------------------------------------------------------
void navFrame(NavModel& m, const Hud& h) {
  const NavConfig& cfg = m.cfg;
  const uint32_t now = h.nowMs;
  const bool  fix   = m.pose.hasFix();
  const bool  stale = m.pose.stale();
  const float scale = cfg.zoom / 100.0f;                    // px per metre
  const float theta = cfg.headingUp ? m.pose.heading() : 0.f;
  const bool  arrived   = m.navState == proto::NAV_ARRIVED;
  const bool  rerouting = !arrived && m.navState == proto::NAV_REROUTING;
  const bool  nogps     = !fix || stale || m.navState == proto::NAV_NO_GPS;

  // ---- texts (re-rendered only on change) ----
  char num[16], unit[16], key[40];
  fmtDist(m.distToTurn, cfg.imperial, num, unit, sizeof(num));
  snprintf(key, sizeof(key), "%s|%s", num, unit);
  if (begin(tDist, key)) {
    if (num[0]) {
      int x = put(tDist, D60, num, 0, 56, -2);
      put(tDist, U24, unit, x + 6, 56, 0);
    }
    finish(tDist);
  }
  if (rerouting)    setText(tStreet, "Calculating a new route", S19, AL_L, 17);
  else              setFit(tStreet, m.street, S19, M15, 17);
  if (rerouting) {
#if UI_ANIMATIONS
    static const char* DOTS[4] = {"", ".", "..", "..."};
    snprintf(key, sizeof(key), "Rerouting%s", DOTS[(now / 350) & 3]);
#else
    snprintf(key, sizeof(key), "Rerouting...");
#endif
    setText(tBig, key, B46, AL_L, 44, -1);
  } else if (arrived) {
    setText(tBig, "Arrived", B56, AL_L, 50, -2);
  }

  float kmh = m.pose.speedMs() * (cfg.imperial ? 2.236936f : 3.6f);
  char sp[8];
  if (fix) snprintf(sp, sizeof(sp), "%d", (int)(kmh + 0.5f)); else snprintf(sp, sizeof(sp), "--");
  setText(tSpeed, sp, B24, AL_L, 24);
  const char* unitTxt = cfg.imperial ? "MPH" : "KM/H";
  setText(tUnit, unitTxt, FB11, AL_L, 12, 1);
  const char* dirTxt = fix ? cardinal(m.pose.heading()) : "--";
  setText(tDir, dirTxt, FB11, AL_L, 14, 2);
  setBattText(h);

  // Maneuver change: icon slides in, proximity bar restarts and then eases to its target.
  static uint8_t  lastIcon = 255;
  static uint32_t iconStart = 0;
  static float    barShown = 0;
  if (m.icon != lastIcon) { lastIcon = m.icon; iconStart = now; barShown = 0; }
  float barTarget = arrived ? 1.f
                  : (m.distToTurn == 0xFFFF || m.distToTurn >= TURN_BAR_M) ? 0.f
                  : 1.f - (float)m.distToTurn / TURN_BAR_M;
  barShown += (barTarget - barShown) * 0.25f;
  if (fabsf(barTarget - barShown) < 0.004f) barShown = barTarget;

  // ---- BASE: map. Redrawn only when the quantized view changed, at most every UI_MAP_MIN_MS ----
  ViewKey k; memset(&k, 0, sizeof(k));
  k.qx = (int32_t)lround(m.pose.x() * scale * (0.1 / UI_MAP_POS_Q_PX));     // 1 dm = scale*0.1 px
  k.qy = (int32_t)lround(m.pose.y() * scale * (0.1 / UI_MAP_POS_Q_PX));
  k.qh = (int32_t)lroundf(theta / UI_MAP_HDG_Q_DEG);
  k.rev = m.routeRev; k.zoom = cfg.zoom; k.hu = cfg.headingUp; k.fix = fix;
  k.dt = ((uint32_t)m.icon << 1) | (m.distToTurn == 0xFFFF ? 1u : 0u);   // maneuver changed -> re-pick the turn node
  k.st = (uint8_t)m.navState;
  static uint32_t lastMapMs = 0;
  const bool changed = !keyValid || memcmp(&k, &lastKey, sizeof(k)) != 0;
  const bool urgent  = !keyValid || k.zoom != lastKey.zoom || k.hu != lastKey.hu || k.fix != lastKey.fix;
  if (changed && (urgent || now - lastMapMs >= UI_MAP_MIN_MS)) {
    lastKey = k; keyValid = true; lastMapMs = now;
    clearMap();
    if (fix) drawRoute(m, theta, scale, arrived);
  }

  // ---- OVERLAY ----
  R->overlayBegin();
  if (fix) {
    float dx = 0, dy = -1;
    if (!cfg.headingUp) { float a = m.pose.heading() * 0.017453292f; dx = sinf(a); dy = -cosf(a); }
    updateChevron(cfg.headingUp ? 0.f : m.pose.heading());
    emitMarker(dx, dy, now, stale || nogps);
  }

  // No-GPS strip (hatched, with a pill) under the banner
  if (nogps) {
    R->ovBitmap(0, BAN_H, SCREEN_W, 30, ptr(spHatch), M_SET);
    R->ovFillRect(0, BAN_H + 28, SCREEN_W, 2, M_SET);
    const int tw = textW(FB11, "NO GPS SIGNAL", 2) + 2;
    const int pw = tw + 36, px = (SCREEN_W - pw) / 2;
    fillRR(px, BAN_H + 3, pw, 22, 11, M_SET);
    fillRR(px + 2, BAN_H + 5, pw - 4, 18, 9, M_CLR);
    tPill.x = px + 18; tPill.y = BAN_H + 4;
    blit(tPill, M_SET);
  }

  // Banner: black slab
  R->ovFillRect(0, 0, SCREEN_W, BAN_H, M_SET);
  int ic = m.icon <= 10 ? m.icon : 0;
  int icX = 16, icY = 12;
#if UI_ANIMATIONS
  if (now - iconStart < ICON_IN_MS)
    icX = 16 - (int)((1.f - easeOut((now - iconStart) / (float)ICON_IN_MS)) * 96);
  else if (ic >= 1 && ic <= 9 && m.distToTurn <= TURN_NEAR_M && !arrived && !rerouting)
    icY -= (int)(tri(now, 700) * 4.f);
#endif
  if (arrived) {
    R->ovBitmap(16, 12, IC, IC, ptr(spIcon[10]), M_CLR);
    blit(tBig, M_CLR);
    blit(tStreet, M_CLR);
    R->ovBitmap(416, 20, 44, 44, ptr(spCheck), M_CLR);
  } else if (rerouting) {
    R->ovBitmap(16, 12, IC, IC, ptr(spSpin[(now / 110) & 7]), M_CLR);
    blit(tBig, M_CLR);
    blit(tStreet, M_CLR);
  } else {
    R->ovBitmap(icX, icY, IC, IC, ptr(spIcon[ic]), M_CLR);
    blit(tDist, M_CLR);
    blit(tStreet, M_CLR);
  }
  if (!arrived) {
    updateCompass(theta, 416, 20);
    R->ovBitmap(416, 20, 44, 44, ptr(spCompass), M_CLR);
  }
  R->ovFillRect(16, BAN_H - 8, SCREEN_W - 32, 1, M_CLR);     // rule
  if (rerouting) {                                           // indeterminate sweep
    int sx = 16 + (int)(tri(now, 1200) * (SCREEN_W - 32 - 80));
    R->ovFillRect(sx, BAN_H - 9, 80, 3, M_CLR);
  } else if (barShown > 0.f) {                               // proximity bar
    int fw = (int)((SCREEN_W - 32) * barShown);
    if (fw > 0) R->ovFillRect(16, BAN_H - 9, fw, 3, M_CLR);
  }

  // Status bar: speed | heading | battery
  emitBarFrame();
  const int base = BOT_Y + 28;                               // 312
  tSpeed.x = 20; tSpeed.y = base - 24;
  blit(tSpeed, M_SET);
  const int wSpd = textW(B24, sp, 0);
  const int wUnit = textW(FB11, unitTxt, 1) + 1;
  tUnit.x = 20 + wSpd + 6; tUnit.y = base - 12;
  blit(tUnit, M_SET);
  char pc[12];
  if (h.battPct < 0) snprintf(pc, sizeof(pc), "--"); else snprintf(pc, sizeof(pc), "%d%%", h.battPct);
  const int rightX = 460 - (32 + 8 + textW(B14, pc, 0));
  emitBattery(rightX, BOT_Y + 12, h.battPct, now);
  tBatt.x = rightX + 40; tBatt.y = BOT_Y + 7;
  blit(tBatt, M_SET);
  const int midL = tUnit.x + wUnit;
  tDir.x = (midL + rightX) / 2 - (textW(FB11, dirTxt, 2)) / 2; tDir.y = BOT_Y + 9;
  blit(tDir, M_SET);

  emitFps(h, cfg.showFps, 8, 134, true);
  emitReveal(now);
}

// =============================================================================================
// Firmware update (OTA)
// =============================================================================================
void updateFrame(const ota::Info& o, const Hud& h) {
  const bool err  = o.state == proto::OTA_ERROR;
  const bool done = o.state == proto::OTA_DONE;
  const bool ver  = o.state == proto::OTA_VERIFYING;
  int pct = o.total ? (int)(((uint64_t)o.done * 100) / o.total) : 0;
  if (done) pct = 100;
  if (pct > 100) pct = 100;

  R->overlayBegin();
  blit(tUpLbl, M_SET);

  if (err) {
    setText(tFailE, ota::errorText(o.err), S19, AL_C, 21);
    setText(tFailH, "Old firmware kept. Try again in the app.", M14, AL_C, 14);
    R->ovFillRect(49, 172, 382, 30, M_CLR);                  // hide the ruler
    R->ovFillCircle(240, 92, 36, M_SET);
    R->ovLine(228, 80, 252, 104, 7, M_CLR);
    R->ovLine(252, 80, 228, 104, 7, M_CLR);
    blit(tFailT, M_SET);
    blit(tFailE, M_SET);
    blit(tFailH, M_SET);
  } else {
    char b[48];
    snprintf(b, sizeof(b), "%d", pct);
    if (begin(tUpPct, b)) {                                  // huge percent
      int wD = textW(D104, b, -4), wP = textW(P40, "%", 0);
      int x = (tUpPct.w - (wD + 6 + wP)) / 2;
      int e = put(tUpPct, D104, b, x, 84, -4);
      put(tUpPct, P40, "%", e + 6 + 4, 84, 0);               // +4: undo the trailing negative tracking
      finish(tUpPct);
    }
    blit(tUpPct, M_SET);

    int fw = 382 * pct / 100;
    if (ver) {                                               // verifying: indeterminate sweep
      int sx = (int)(tri(h.nowMs, 1200) * (382 - 80));
      R->ovFillRect(49 + sx, 172, 80, 10, M_SET);
    } else if (fw > 0) {
      R->ovFillRect(49, 172, fw, 10, M_SET);
    }

    snprintf(b, sizeof(b), "%lu / %lu KB", (unsigned long)((o.done + 512) / 1024), (unsigned long)((o.total + 512) / 1024));
    setText(tUpKB, b, B14, AL_L, 16);
    blit(tUpKB, M_SET);
    snprintf(b, sizeof(b), "%u.%u.%u -> %u.%u.%u",
             (unsigned)FW_V_MAJOR(FW_VERSION), (unsigned)FW_V_MINOR(FW_VERSION), (unsigned)FW_V_PATCH(FW_VERSION),
             (unsigned)FW_V_MAJOR(o.version),  (unsigned)FW_V_MINOR(o.version),  (unsigned)FW_V_PATCH(o.version));
    setText(tUpVer, b, FB11, AL_R, 14, 2, 2);
    blit(tUpVer, M_SET);

    const char* msg = done ? "Restarting..." : ver ? "Verifying..." : "Keep the app open. Do not power off.";
    setText(tUpHint, msg, S14, AL_L, 16);
    const bool lock = !done && !ver;
    const int tw = textW(S14, msg, 0);
    const int total = (lock ? 22 : 0) + tw;
    const int x0 = (SCREEN_W - total) / 2;
    if (lock) R->ovBitmap(x0, 245, 14, 16, ptr(spLock), M_SET);
    tUpHint.x = x0 + (lock ? 22 : 0);
    blit(tUpHint, M_SET);
  }

  emitStatusBar(h, false);
  emitFps(h, h.showFps, 140, 264, false);
  emitReveal(h.nowMs);
}

}  // namespace ui