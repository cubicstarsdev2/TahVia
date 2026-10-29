#include "ui.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "config.h"

using namespace mtr;

namespace ui {
namespace {

// ---- Layout -----------------------------------------------------------------
const int TOP_H = 76, BOT_H = 30;
const int MAP_Y = TOP_H, MAP_H = SCREEN_H - TOP_H - BOT_H, BOT_Y = SCREEN_H - BOT_H;
const int ROUTE_W = 9;
const int ANCHOR_X = SCREEN_W / 2, ANCHOR_Y = 215;      // marker sits low: more road ahead
const float CULL_R = 300.f;                             // px around the marker (README 6.7)
const int BACK_MAX = 24, FWD_MAX = 200;

// Shared by boot and idle so the two screens look alike
const int LOGO_X = 192, LOGO_Y = 24, LOGO_S = 96, LOGO_CX = 240, LOGO_CY = 72;
const int TITLE_X = 110, TITLE_Y = 146, TITLE_W = 260, TITLE_H = 56;
const int BAR_X = 120, BAR_Y = 240, BAR_W = 240, BAR_H = 14;
const int INFO_X = 120, INFO_Y = BOT_Y + 4, INFO_W = 344, INFO_H = 24;

const Mode M_SET = SET, M_CLR = CLEAR;

TFT_eSPI* tft = nullptr;
Renderer* R   = nullptr;

// ---- 1-bpp text sprites -------------------------------------------------------
struct TextSpr {
  TFT_eSprite* sp = nullptr;
  int x = 0, y = 0, w = 0, h = 0;
  char cache[48] = {0};
};
TextSpr tTitle, tBoot, tStat, tHint, tDist, tStreet, tBatt, tInfo, tBanner;
TextSpr tUpTitle, tUpVer, tUpPct, tUpDone, tUpHint;     // firmware update screen
TFT_eSprite* spLogo = nullptr;
TFT_eSprite* spIcon[11];

inline const uint8_t* ptr(TFT_eSprite* s) { return s ? (const uint8_t*)s->getPointer() : nullptr; }

void mkSprite(TFT_eSprite*& sp, int w, int h) {
  sp = new TFT_eSprite(tft);
  sp->setColorDepth(1);
  sp->createSprite(w, h);
  sp->fillSprite(0);
}
void mk(TextSpr& t, int x, int y, int w, int h) { mkSprite(t.sp, w, h); t.x = x; t.y = y; t.w = w; t.h = h; }

// Re-render only when the text changed, and tell the renderer by hand (it compares commands,
// not bitmap contents). Returns true if re-rendered.
bool setText(TextSpr& t, const char* s, const GFXfont* f, uint8_t datum, int tx, int ty) {
  if (!t.sp || strncmp(t.cache, s, sizeof(t.cache) - 1) == 0) return false;
  strncpy(t.cache, s, sizeof(t.cache) - 1); t.cache[sizeof(t.cache) - 1] = 0;
  t.sp->fillSprite(0);
  t.sp->setTextColor(1);
  t.sp->setFreeFont(f);
  t.sp->setTextDatum(datum);
  t.sp->drawString(s, tx, ty);
  R->markDirtyRect(t.x, t.y, t.x + t.w - 1, t.y + t.h - 1);
  return true;
}
void blit(const TextSpr& t, Mode m) {
  const uint8_t* p = ptr(t.sp);
  if (p) R->ovBitmap(t.x, t.y, t.w, t.h, p, m);
}

// ---- Drawing helpers for icon sprites --------------------------------------------
void thickLine(TFT_eSprite* s, float x0, float y0, float x1, float y1, int r) {
  float dx = x1 - x0, dy = y1 - y0;
  int n = (int)ceilf(fmaxf(fabsf(dx), fabsf(dy)));
  if (n < 1) n = 1;
  for (int i = 0; i <= n; i++) {
    float t = (float)i / n;
    s->fillCircle(lroundf(x0 + dx * t), lroundf(y0 + dy * t), r, 1);
  }
}
// Polyline p (x,y pairs) with an arrow head at the last point.
void pathArrow(TFT_eSprite* s, const float* p, int n) {
  const int r = 3; const float HL = 15, HW = 12;
  float tx = p[2 * n - 2], ty = p[2 * n - 1], sx = p[2 * n - 4], sy = p[2 * n - 3];
  float dx = tx - sx, dy = ty - sy, L = sqrtf(dx * dx + dy * dy);
  if (L < 1) L = 1;
  float ux = dx / L, uy = dy / L;
  for (int i = 0; i < n - 2; i++) thickLine(s, p[2 * i], p[2 * i + 1], p[2 * i + 2], p[2 * i + 3], r);
  thickLine(s, sx, sy, tx - ux * HL * 0.5f, ty - uy * HL * 0.5f, r);
  float bx = tx - ux * HL, by = ty - uy * HL, px = -uy, py = ux;
  s->fillTriangle(lroundf(tx), lroundf(ty), lroundf(bx + px * HW), lroundf(by + py * HW),
                  lroundf(bx - px * HW), lroundf(by - py * HW), 1);
}

void buildIcons() {
  static const float P_STRAIGHT[] = {24,46, 24,3};
  static const float P_LEFT[]     = {32,46, 32,28, 4,28};
  static const float P_RIGHT[]    = {16,46, 16,28, 44,28};
  static const float P_SLEFT[]    = {30,46, 30,30, 8,8};
  static const float P_SRIGHT[]   = {18,46, 18,30, 40,8};
  static const float P_HLEFT[]    = {32,46, 32,16, 8,40};
  static const float P_HRIGHT[]   = {16,46, 16,16, 40,40};
  static const float P_UTURN[]    = {34,46, 34,22, 31,14, 24,10, 17,14, 14,22, 14,44};
  static const float P_EXIT[]     = {24,19, 24,3};
  for (int i = 0; i < 11; i++) mkSprite(spIcon[i], 48, 48);     // 0 = blank
  pathArrow(spIcon[1], P_STRAIGHT, 2);
  pathArrow(spIcon[2], P_LEFT, 3);
  pathArrow(spIcon[3], P_RIGHT, 3);
  pathArrow(spIcon[4], P_SLEFT, 3);
  pathArrow(spIcon[5], P_SRIGHT, 3);
  pathArrow(spIcon[6], P_HLEFT, 3);
  pathArrow(spIcon[7], P_HRIGHT, 3);
  pathArrow(spIcon[8], P_UTURN, 7);
  // 9 roundabout: ring + entry stem + exit arrow
  spIcon[9]->fillCircle(24, 30, 13, 1);
  spIcon[9]->fillCircle(24, 30, 7, 0);
  thickLine(spIcon[9], 24, 48, 24, 40, 3);
  pathArrow(spIcon[9], P_EXIT, 2);
  // 10 arrive: map pin
  spIcon[10]->fillCircle(24, 17, 13, 1);
  spIcon[10]->fillTriangle(12, 23, 36, 23, 24, 46, 1);
  spIcon[10]->fillCircle(24, 17, 5, 0);
}

void buildLogo() {
  mkSprite(spLogo, LOGO_S, LOGO_S);
  spLogo->fillCircle(48, 48, 46, 1);                       // black disc
  spLogo->fillTriangle(48, 16, 72, 78, 48, 63, 0);         // white navigation arrow
  spLogo->fillTriangle(48, 16, 24, 78, 48, 63, 0);
}

// ---- Number formatting ------------------------------------------------------------
void fmtDist(uint16_t m, bool imp, char* out, size_t n) {
  if (m == 0xFFFF) { out[0] = 0; return; }
  if (!imp) {
    if (m < 995)       snprintf(out, n, "%u m", (unsigned)(((m + 5) / 10) * 10));
    else if (m < 9950) snprintf(out, n, "%.1f km", m / 1000.0f);
    else               snprintf(out, n, "%u km", (unsigned)((m + 500) / 1000));
  } else {
    float ft = m * 3.28084f;
    if (ft < 528.f) snprintf(out, n, "%d ft", ((int)(ft + 25) / 50) * 50);
    else {
      float mi = m / 1609.344f;
      if (mi < 10.f) snprintf(out, n, "%.1f mi", mi);
      else           snprintf(out, n, "%d mi", (int)(mi + 0.5f));
    }
  }
}

// ---- Shared HUD pieces (OVERLAY) -------------------------------------------------------
void emitBottomBar(const Hud& h, const char* info) {
  R->ovFillRect(0, BOT_Y, SCREEN_W, BOT_H, M_CLR);
  R->ovFillRect(0, BOT_Y, SCREEN_W, 2, M_SET);
  const int bx = 16, by = BOT_Y + 8;                       // battery icon
  R->ovRect(bx, by, 32, 16, M_SET);
  R->ovRect(bx + 1, by + 1, 30, 14, M_SET);
  R->ovFillRect(bx + 32, by + 5, 3, 6, M_SET);
  R->ovFillRect(bx + 4, by + 4, (24 * h.battPct) / 100, 8, M_SET);   // ignored if <= 0
  char b[8];
  if (h.battPct < 0) snprintf(b, sizeof(b), "--"); else snprintf(b, sizeof(b), "%d%%", h.battPct);
  setText(tBatt, b, &FreeSansBold9pt7b, ML_DATUM, 0, 12);
  blit(tBatt, M_SET);
  setText(tInfo, info, &FreeSans9pt7b, TR_DATUM, INFO_W - 1, 1);
  blit(tInfo, M_SET);
}

void appendFps(char* buf, size_t n, const Hud& h, bool show) {
  if (!show) return;
  size_t l = strlen(buf);
  snprintf(buf + l, n - l, "%s%.0f fps  %d tiles", l ? "    " : "", h.fps, h.tiles);
}

// ---- Map ------------------------------------------------------------------------------------
// Liang-Barsky clip (the renderer's line loop walks the whole line: always clip first).
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

void clearMap() { R->baseFillRect(0, MAP_Y, SCREEN_W, MAP_H, M_CLR); }

// Heading-up projection (README 6.7):
//   sx = AX + s*(e*cos(t) - n*sin(t)),  sy = AY - s*(e*sin(t) + n*cos(t))
// Only points near the cursor are projected, so cost does not depend on the route length.
void drawRoute(const NavModel& m, float thetaDeg, float scale) {
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

  uint32_t lo = c0;                                          // walk back until a point leaves the cull circle
  for (int i = 0; i < BACK_MAX && lo > r.tail(); i++) {
    lo--;
    float x, y; P::proj(r.at(lo), ix, iy, k, c, s, x, y);
    if (isFar(x, y)) break;
  }

  static float PX[BACK_MAX + FWD_MAX + 2], PY[BACK_MAX + FWD_MAX + 2];
  int n = 0;
  for (uint32_t i = lo; i < r.head() && n < BACK_MAX + FWD_MAX + 1; i++) {
    P::proj(r.at(i), ix, iy, k, c, s, PX[n], PY[n]);
    n++;
    if (i > c0 && isFar(PX[n - 1], PY[n - 1])) break;          // include the segment that leaves, then stop
  }
  for (int i = 0; i + 1 < n; i++) {
    float x0 = PX[i], y0 = PY[i], x1 = PX[i + 1], y1 = PY[i + 1];
    if (!clipLine(x0, y0, x1, y1, -5, MAP_Y - 5, SCREEN_W + 4, BOT_Y + 4)) continue;
    R->baseLine(lroundf(x0), lroundf(y0), lroundf(x1), lroundf(y1), ROUTE_W, M_SET);
  }
}

void emitMarker(int cx, int cy, float dx, float dy) {
  R->ovCircle(cx, cy, 24, M_SET);          // accuracy ring
  R->ovFillCircle(cx, cy, 19, M_CLR);      // halo over the route
  R->ovFillCircle(cx, cy, 15, M_SET);      // disc
  float px = -dy, py = dx;                 // heading chevron (white)
  int   tx = cx + lroundf(12 * dx), ty = cy + lroundf(12 * dy);
  float bx = cx - 7 * dx, by = cy - 7 * dy;
  R->ovLine(tx, ty, lroundf(bx + 9 * px), lroundf(by + 9 * py), 3, M_CLR);
  R->ovLine(tx, ty, lroundf(bx - 9 * px), lroundf(by - 9 * py), 3, M_CLR);
}

// ---- Navigation view-key: redraw BASE only if the picture would change -------------------
struct ViewKey { int32_t qx, qy, qh; uint32_t rev; uint16_t zoom; uint8_t hu, fix; };
ViewKey lastKey; bool keyValid = false;

}  // namespace

// =============================================================================================
// Boot
// =============================================================================================
void initBoot(TFT_eSPI* t, Renderer* r) {
  tft = t; R = r;
  buildLogo();
  mk(tTitle, TITLE_X, TITLE_Y, TITLE_W, TITLE_H);
  mk(tBoot, 10, 262, 460, 24);
  setText(tTitle, "TahVia", &FreeSansBold24pt7b, TC_DATUM, TITLE_W / 2, 0);
}

void initRest() {
  mk(tStat,   10, 204, 460, 28);
  mk(tHint,   10, 234, 460, 24);
  mk(tDist,   80, 6,   140, 44);
  mk(tStreet, 80, 48,  380, 26);
  mk(tBatt,   60, INFO_Y, 52, 24);
  mk(tInfo,   INFO_X, INFO_Y, INFO_W, INFO_H);
  mk(tBanner, 140, MAP_Y + 8, 200, 26);
  mk(tUpTitle, 10,  16, 460, 44);
  mk(tUpVer,   10,  64, 460, 24);
  mk(tUpPct,   140, 98, 200, 56);
  mk(tUpDone,  10, 206, 460, 24);
  mk(tUpHint,  10, 242, 460, 24);
  buildIcons();
}

void bootDraw() {
  R->baseClear(M_CLR);
  R->baseBitmap(LOGO_X, LOGO_Y, LOGO_S, LOGO_S, ptr(spLogo), M_SET);
  R->baseBitmap(TITLE_X, TITLE_Y, TITLE_W, TITLE_H, ptr(tTitle.sp), M_SET);
  R->baseRect(BAR_X, BAR_Y, BAR_W, BAR_H, M_SET);
  R->baseRect(BAR_X + 1, BAR_Y + 1, BAR_W - 2, BAR_H - 2, M_SET);
}

void bootLabel(const char* s) {
  if (!setText(tBoot, s, &FreeSans9pt7b, TC_DATUM, tBoot.w / 2, 0)) return;
  R->baseFillRect(tBoot.x, tBoot.y, tBoot.w, tBoot.h, M_CLR);
  R->baseBitmap(tBoot.x, tBoot.y, tBoot.w, tBoot.h, ptr(tBoot.sp), M_SET);
}

void bootBar(int pct) {
  if (pct > 100) pct = 100;
  int fw = (BAR_W - 8) * pct / 100;
  if (fw > 0) R->baseFillRect(BAR_X + 4, BAR_Y + 4, fw, BAR_H - 8, M_SET);
}

// =============================================================================================
// Runtime screens
// =============================================================================================
void enter(Screen) {
  R->baseClear(M_CLR);
  keyValid = false;
}

void idleFrame(const Hud& h) {
  char stat[48], hint[48], info[64];
  if (h.pairing) {
    snprintf(stat, sizeof(stat), "Pairing code  %06lu", (unsigned long)h.passkey);
    snprintf(hint, sizeof(hint), "Enter this code on your phone");
  } else if (!h.connected) {
    snprintf(stat, sizeof(stat), "Waiting for phone");
    snprintf(hint, sizeof(hint), "Open TahviaNav and connect to TahVia");
  } else {
    snprintf(stat, sizeof(stat), "Phone connected");
    snprintf(hint, sizeof(hint), "No route. Pick a destination in the app.");
  }
  setText(tStat, stat, &FreeSansBold12pt7b, TC_DATUM, tStat.w / 2, 0);
  setText(tHint, hint, &FreeSans9pt7b, TC_DATUM, tHint.w / 2, 0);

  snprintf(info, sizeof(info), "%s", h.connected ? "Bluetooth connected" : "Bluetooth waiting");
  appendFps(info, sizeof(info), h, h.showFps);

  R->overlayBegin();
  R->ovBitmap(LOGO_X, LOGO_Y, LOGO_S, LOGO_S, ptr(spLogo), M_SET);
  if (!h.connected && !h.pairing) {                        // "searching" pulses
    float ph = (h.nowMs % 2000) / 2000.f;
    for (int i = 0; i < 2; i++) {
      float p = ph + 0.5f * i; if (p >= 1.f) p -= 1.f;
      int r = 54 + (int)(p * 14);
      R->ovCircle(LOGO_CX, LOGO_CY, r, M_SET);
      R->ovCircle(LOGO_CX, LOGO_CY, r + 1, M_SET);
    }
  } else {                                                 // linked: steady ring
    R->ovCircle(LOGO_CX, LOGO_CY, 56, M_SET);
    R->ovCircle(LOGO_CX, LOGO_CY, 57, M_SET);
  }
  R->ovBitmap(TITLE_X, TITLE_Y, TITLE_W, TITLE_H, ptr(tTitle.sp), M_SET);
  blit(tStat, M_SET);
  blit(tHint, M_SET);
  emitBottomBar(h, info);
}

void navFrame(NavModel& m, const Hud& h) {
  const NavConfig& cfg = m.cfg;
  const bool  fix   = m.pose.hasFix();
  const float scale = cfg.zoom / 100.0f;                    // px per metre
  const float theta = cfg.headingUp ? m.pose.heading() : 0.f;

  // ---- texts (re-rendered only on change) ----
  char buf[64];
  fmtDist(m.distToTurn, cfg.imperial, buf, sizeof(buf));
  setText(tDist, buf, &FreeSansBold18pt7b, TL_DATUM, 0, 0);
  setText(tStreet, m.street, &FreeSans9pt7b, TL_DATUM, 0, 0);

  const char* ban = "";
  if (m.navState == proto::NAV_ARRIVED)                              ban = "ARRIVED";
  else if (!fix || m.pose.stale() || m.navState == proto::NAV_NO_GPS) ban = "NO GPS";
  else if (m.navState == proto::NAV_REROUTING)                       ban = "REROUTING";
  setText(tBanner, ban, &FreeSansBold9pt7b, MC_DATUM, tBanner.w / 2, 13);

  float kmh = m.pose.speedMs() * (cfg.imperial ? 2.236936f : 3.6f);
  snprintf(buf, sizeof(buf), "%d %s", (int)(kmh + 0.5f), cfg.imperial ? "mph" : "km/h");
  appendFps(buf, sizeof(buf), h, cfg.showFps);

  // ---- BASE: map. Redrawn only when position/heading/zoom/route changed by ~half a pixel ----
  ViewKey k; memset(&k, 0, sizeof(k));
  k.qx = (int32_t)lround(m.pose.x() * scale * 0.2);
  k.qy = (int32_t)lround(m.pose.y() * scale * 0.2);
  k.qh = (int32_t)lroundf(theta * 4.f);
  k.rev = m.routeRev; k.zoom = cfg.zoom; k.hu = cfg.headingUp; k.fix = fix;
  if (!keyValid || memcmp(&k, &lastKey, sizeof(k)) != 0) {
    lastKey = k; keyValid = true;
    clearMap();
    if (fix) drawRoute(m, theta, scale);
  }

  // ---- OVERLAY: marker + HUD ----
  R->overlayBegin();
  if (fix) {
    float dx = 0, dy = -1;                                  // heading-up: marker always points up
    if (!cfg.headingUp) { float a = m.pose.heading() * 0.017453292f; dx = sinf(a); dy = -cosf(a); }
    emitMarker(ANCHOR_X, ANCHOR_Y, dx, dy);
  }
  R->ovFillRect(0, 0, SCREEN_W, TOP_H, M_SET);              // top bar: black, white content
  int ic = m.icon <= 10 ? m.icon : 0;
  R->ovBitmap(16, 14, 48, 48, ptr(spIcon[ic]), M_CLR);
  blit(tDist, M_CLR);
  blit(tStreet, M_CLR);
  if (ban[0]) {
    R->ovFillRect(tBanner.x, tBanner.y, tBanner.w, tBanner.h, M_SET);
    blit(tBanner, M_CLR);
  }
  emitBottomBar(h, buf);
}

// =============================================================================================
// Firmware update (OTA)
// =============================================================================================
void updateFrame(const ota::Info& o, const Hud& h) {
  const int BX = 60, BY = 170, BW = 360, BH = 22;          // progress bar
  const bool err  = o.state == proto::OTA_ERROR;
  const bool done = o.state == proto::OTA_DONE;
  const bool ver  = o.state == proto::OTA_VERIFYING;
  int pct = o.total ? (int)(((uint64_t)o.done * 100) / o.total) : 0;
  if (done) pct = 100;
  if (pct > 100) pct = 100;

  char b[48];
  setText(tUpTitle, err ? "Update failed" : done ? "Update complete" : ver ? "Verifying..." : "Updating firmware",
          &FreeSansBold18pt7b, TC_DATUM, tUpTitle.w / 2, 0);

  if (err) snprintf(b, sizeof(b), "%s", ota::errorText(o.err));
  else     snprintf(b, sizeof(b), "v%u.%u  ->  v%u.%u", (unsigned)(FW_VERSION >> 8), (unsigned)(FW_VERSION & 0xFF),
                    (unsigned)(o.version >> 8), (unsigned)(o.version & 0xFF));
  setText(tUpVer, b, &FreeSans9pt7b, TC_DATUM, tUpVer.w / 2, 0);

  if (err) b[0] = 0; else snprintf(b, sizeof(b), "%d%%", pct);
  setText(tUpPct, b, &FreeSansBold24pt7b, TC_DATUM, tUpPct.w / 2, 0);

  if (err) b[0] = 0;
  else     snprintf(b, sizeof(b), "%lu / %lu KB", (unsigned long)((o.done + 512) / 1024), (unsigned long)((o.total + 512) / 1024));
  setText(tUpDone, b, &FreeSans9pt7b, TC_DATUM, tUpDone.w / 2, 0);

  setText(tUpHint, err ? "Old firmware kept. Try again in the app."
                  : done ? "Restarting..." : "Keep the app open. Do not power off.",
          &FreeSans9pt7b, TC_DATUM, tUpHint.w / 2, 0);

  R->overlayBegin();
  blit(tUpTitle, M_SET);
  blit(tUpVer, M_SET);
  if (!err) {
    blit(tUpPct, M_SET);
    R->ovRect(BX, BY, BW, BH, M_SET);                      // double outline, like the boot bar
    R->ovRect(BX + 1, BY + 1, BW - 2, BH - 2, M_SET);
    int fw = (BW - 8) * pct / 100;
    if (fw > 0) R->ovFillRect(BX + 4, BY + 4, fw, BH - 8, M_SET);
  }
  blit(tUpDone, M_SET);
  blit(tUpHint, M_SET);
  emitBottomBar(h, h.connected ? "Bluetooth connected" : "Bluetooth lost");
}

}  // namespace ui