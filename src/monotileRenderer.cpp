#include "MonoTileRenderer.h"
#include <stdlib.h>
#include <math.h>

namespace mtr {
namespace {

struct Surface {
  uint8_t* data;
  int stride;
  int ox, oy, w, h;
};

inline void applyMask(uint8_t& b, uint8_t mask, Mode m) {
  switch (m) {
    case SET:   b |= mask; break;
    case CLEAR: b &= (uint8_t)~mask; break;
    default:    b ^= mask; break;
  }
}

inline void putPixel(Surface& s, int x, int y, Mode m) {
  x -= s.ox; y -= s.oy;
  if ((unsigned)x >= (unsigned)s.w || (unsigned)y >= (unsigned)s.h) return;
  applyMask(s.data[y * s.stride + (x >> 3)], (uint8_t)(0x80 >> (x & 7)), m);
}

void hspan(Surface& s, int y, int x0, int x1, Mode m) {
  y -= s.oy;
  if ((unsigned)y >= (unsigned)s.h) return;
  x0 -= s.ox; x1 -= s.ox;
  if (x0 < 0) x0 = 0;
  if (x1 >= s.w) x1 = s.w - 1;
  if (x0 > x1) return;
  uint8_t* row = s.data + y * s.stride;
  int b0 = x0 >> 3, b1 = x1 >> 3;
  uint8_t first = (uint8_t)(0xFF >> (x0 & 7));
  uint8_t last  = (uint8_t)(0xFF << (7 - (x1 & 7)));
  if (b0 == b1) { applyMask(row[b0], first & last, m); return; }
  applyMask(row[b0], first, m);
  for (int b = b0 + 1; b < b1; b++) applyMask(row[b], 0xFF, m);
  applyMask(row[b1], last, m);
}

void fillRectS(Surface& s, int x, int y, int w, int h, Mode m) {
  if (w <= 0 || h <= 0) return;
  int ya = y < s.oy ? s.oy : y;
  int yb = (y + h < s.oy + s.h) ? y + h : s.oy + s.h;
  for (int yy = ya; yy < yb; yy++) hspan(s, yy, x, x + w - 1, m);
}

void rectS(Surface& s, int x, int y, int w, int h, Mode m) {
  if (w <= 0 || h <= 0) return;
  fillRectS(s, x, y, w, 1, m);
  if (h > 1) fillRectS(s, x, y + h - 1, w, 1, m);
  if (h > 2) {
    fillRectS(s, x, y + 1, 1, h - 2, m);
    if (w > 1) fillRectS(s, x + w - 1, y + 1, 1, h - 2, m);
  }
}

inline int isqrt(int v) {
  if (v <= 0) return 0;
  int r = (int)sqrtf((float)v);
  while (r * r > v) r--;
  while ((r + 1) * (r + 1) <= v) r++;
  return r;
}

void fillDisc(Surface& s, int cx, int cy, int rr, int r2, Mode m) {
  int d0 = s.oy - cy; if (d0 < -rr) d0 = -rr;           // solo filas visibles
  int d1 = s.oy + s.h - 1 - cy; if (d1 > rr) d1 = rr;
  for (int dy = d0; dy <= d1; dy++) {
    int hw = isqrt(r2 - dy * dy);
    hspan(s, cy + dy, cx - hw, cx + hw, m);
  }
}

void circleS(Surface& s, int cx, int cy, int r, Mode m) {
  if (r <= 0) { putPixel(s, cx, cy, m); return; }
  int x = r, y = 0, d = 1 - r;
  while (x >= y) {
    putPixel(s, cx + x, cy + y, m);
    if (x)       putPixel(s, cx - x, cy + y, m);
    if (y)       putPixel(s, cx + x, cy - y, m);
    if (x && y)  putPixel(s, cx - x, cy - y, m);
    if (x != y) {
      putPixel(s, cx + y, cy + x, m);
      if (y)      putPixel(s, cx - y, cy + x, m);
      if (x)      putPixel(s, cx + y, cy - x, m);
      if (x && y) putPixel(s, cx - y, cy - x, m);
    }
    y++;
    if (d < 0) d += 2 * y + 1;
    else { x--; d += 2 * (y - x) + 1; }
  }
}

// Bresenham. width > 1 estampa un disco por paso (usar SET/CLEAR; con XOR solo width 1).
// Los pasos fuera de la superficie (+margen) solo cuestan una comparacion.
void lineS(Surface& s, int x0, int y0, int x1, int y1, int width, Mode m) {
  if (width > 16) width = 16;
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  const int rr = width / 2, r2 = (width * width) / 4;
  const int pad = width > 1 ? rr : 0;
  const int xl = s.ox - pad, xh = s.ox + s.w + pad;
  const int yl = s.oy - pad, yh = s.oy + s.h + pad;
  int8_t hw[17];
  if (width > 1)
    for (int d = -rr; d <= rr; d++) hw[d + rr] = (int8_t)isqrt(r2 - d * d);
  for (;;) {
    if (x0 >= xl && x0 < xh && y0 >= yl && y0 < yh) {
      if (width <= 1) putPixel(s, x0, y0, m);
      else {
        for (int d = -rr; d <= rr; d++) {
          int y = y0 + d;
          if (y < s.oy || y >= s.oy + s.h) continue;
          hspan(s, y, x0 - hw[d + rr], x0 + hw[d + rr], m);
        }
      }
    }
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

// Bitmap recortado por columnas/filas; salta bytes fuente vacios.
void bitmapS(Surface& s, int x, int y, int w, int h, const uint8_t* bits, Mode m) {
  const int stride = (w + 7) >> 3;
  int c0 = s.ox - x; if (c0 < 0) c0 = 0;
  int c1 = s.ox + s.w - x; if (c1 > w) c1 = w;
  if (c0 >= c1) return;
  int r0 = s.oy - y; if (r0 < 0) r0 = 0;
  int r1 = s.oy + s.h - y; if (r1 > h) r1 = h;
  for (int r = r0; r < r1; r++) {
    const uint8_t* row = bits + r * stride;
    for (int cb = c0 >> 3; cb <= (c1 - 1) >> 3; cb++) {
      uint8_t v = row[cb];
      if (!v) continue;
      for (int i = 0; i < 8; i++) {
        int c = cb * 8 + i;
        if (c < c0 || c >= c1 || !(v & (0x80 >> i))) continue;
        putPixel(s, x + c, y + r, m);
      }
    }
  }
}

void execCmd(Surface& s, const Cmd& c) {
  Mode m = (Mode)c.mode;
  switch (c.op) {
    case OP_LINE:        lineS(s, c.a, c.b, c.c, c.d, c.width, m); break;
    case OP_FILL_RECT:   fillRectS(s, c.a, c.b, c.c, c.d, m); break;
    case OP_RECT:        rectS(s, c.a, c.b, c.c, c.d, m); break;
    case OP_CIRCLE:      circleS(s, c.a, c.b, c.c, m); break;
    case OP_FILL_CIRCLE: fillDisc(s, c.a, c.b, c.c, c.c * c.c + c.c, m); break;
    case OP_BITMAP:      bitmapS(s, c.a, c.b, c.c, c.d, c.bmp, m); break;
  }
}

Cmd blank(Op op, Mode m) {
  Cmd c; memset(&c, 0, sizeof(c));
  c.op = op; c.mode = m;
  return c;
}
inline int mn(int a, int b) { return a < b ? a : b; }
inline int mx(int a, int b) { return a > b ? a : b; }

Cmd mkLine(int x0, int y0, int x1, int y1, int w, Mode m) {
  Cmd c = blank(OP_LINE, m);
  c.a = x0; c.b = y0; c.c = x1; c.d = y1; c.width = (uint8_t)w;
  int pad = w > 1 ? (w > 16 ? 16 : w) / 2 : 0;
  c.x0 = mn(x0, x1) - pad; c.y0 = mn(y0, y1) - pad;
  c.x1 = mx(x0, x1) + pad; c.y1 = mx(y0, y1) + pad;
  return c;
}
bool mkRect(Cmd& c, Op op, int x, int y, int w, int h, Mode m) {
  if (w <= 0 || h <= 0) return false;
  c = blank(op, m);
  c.a = x; c.b = y; c.c = w; c.d = h;
  c.x0 = x; c.y0 = y; c.x1 = x + w - 1; c.y1 = y + h - 1;
  return true;
}
Cmd mkCircle(Op op, int cx, int cy, int r, Mode m) {
  Cmd c = blank(op, m);
  c.a = cx; c.b = cy; c.c = r;
  c.x0 = cx - r; c.y0 = cy - r; c.x1 = cx + r; c.y1 = cy + r;
  return c;
}

inline bool sameCmd(const Cmd& a, const Cmd& b) { return memcmp(&a, &b, sizeof(Cmd)) == 0; }

}  // namespace

// ===================================================================
void Renderer::begin(const Display& d) {
  disp_ = d;
  memset(base_, 0, sizeof(base_));
  nCur_ = nPrev_ = 0;
  overlayOpen_ = false;
  scan_ = 0;
  buildLut();
  invalidateAll();
  forceResend();
}

void Renderer::setColors(uint16_t fg, uint16_t bg) {
  fg_ = fg; bg_ = bg;
  buildLut();
  invalidateAll();
  forceResend();
}

void Renderer::buildLut() {
  auto sw = [](uint16_t v) -> uint16_t {
#if MTR_SWAP_BYTES
    return (uint16_t)((v << 8) | (v >> 8));
#else
    return v;
#endif
  };
  uint16_t f = sw(fg_), b = sw(bg_);
  for (int v = 0; v < 256; v++)
    for (int i = 0; i < 8; i++)
      lut_[v][i] = ((v >> (7 - i)) & 1) ? f : b;
}

void Renderer::invalidateAll() {
  for (auto& w : dirty_) w = 0xFFFFFFFFu;
}

void Renderer::markDirtyRect(int x0, int y0, int x1, int y1) {
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 >= MTR_WIDTH) x1 = MTR_WIDTH - 1;
  if (y1 >= MTR_HEIGHT) y1 = MTR_HEIGHT - 1;
  if (x0 > x1 || y0 > y1) return;
  for (int ty = y0 / TILE; ty <= y1 / TILE; ty++)
    for (int tx = x0 / TILE; tx <= x1 / TILE; tx++) {
      int t = ty * TILES_X + tx;
      dirty_[t >> 5] |= 1u << (t & 31);
    }
}

int Renderer::dirtyCount() const {
  int n = 0;
  for (int t = 0; t < TILES; t++) if (isDirty(t)) n++;
  return n;
}

// ---- BASE ----
#define BASE_SURFACE Surface s{base_, STRIDE, 0, 0, MTR_WIDTH, MTR_HEIGHT}

void Renderer::baseClear(Mode m) {
  if (m == XOR) for (auto& b : base_) b = (uint8_t)~b;
  else memset(base_, m == SET ? 0xFF : 0x00, sizeof(base_));
  invalidateAll();
}
void Renderer::baseLine(int x0, int y0, int x1, int y1, int width, Mode m) {
  BASE_SURFACE; Cmd c = mkLine(x0, y0, x1, y1, width, m);
  execCmd(s, c); markDirtyRect(c.x0, c.y0, c.x1, c.y1);
}
void Renderer::basePolyline(const int16_t* xy, int n, int width, Mode m) {
  for (int i = 0; i + 1 < n; i++)
    baseLine(xy[2 * i], xy[2 * i + 1], xy[2 * i + 2], xy[2 * i + 3], width, m);
}
void Renderer::baseFillRect(int x, int y, int w, int h, Mode m) {
  BASE_SURFACE; Cmd c;
  if (!mkRect(c, OP_FILL_RECT, x, y, w, h, m)) return;
  execCmd(s, c); markDirtyRect(c.x0, c.y0, c.x1, c.y1);
}
void Renderer::baseRect(int x, int y, int w, int h, Mode m) {
  BASE_SURFACE; Cmd c;
  if (!mkRect(c, OP_RECT, x, y, w, h, m)) return;
  execCmd(s, c); markDirtyRect(c.x0, c.y0, c.x1, c.y1);
}
void Renderer::baseCircle(int cx, int cy, int r, Mode m) {
  BASE_SURFACE; Cmd c = mkCircle(OP_CIRCLE, cx, cy, r, m);
  execCmd(s, c); markDirtyRect(c.x0, c.y0, c.x1, c.y1);
}
void Renderer::baseFillCircle(int cx, int cy, int r, Mode m) {
  BASE_SURFACE; Cmd c = mkCircle(OP_FILL_CIRCLE, cx, cy, r, m);
  execCmd(s, c); markDirtyRect(c.x0, c.y0, c.x1, c.y1);
}
void Renderer::baseBitmap(int x, int y, int w, int h, const uint8_t* bits, Mode m) {
  BASE_SURFACE; Cmd c;
  if (!mkRect(c, OP_BITMAP, x, y, w, h, m)) return;
  c.bmp = bits;
  execCmd(s, c); markDirtyRect(c.x0, c.y0, c.x1, c.y1);
}
bool Renderer::basePixel(int x, int y) const {
  if ((unsigned)x >= MTR_WIDTH || (unsigned)y >= MTR_HEIGHT) return false;
  return (base_[y * STRIDE + (x >> 3)] >> (7 - (x & 7))) & 1;
}

// ---- OVERLAY ----
void Renderer::overlayBegin() {
  if (overlayOpen_) return;
  memcpy(prev_, cur_, nCur_ * sizeof(Cmd));
  nPrev_ = nCur_;
  nCur_ = 0;
  overlayOpen_ = true;
}

bool Renderer::addCmd(const Cmd& c) {
  if (!overlayOpen_) overlayBegin();
  if (nCur_ >= MTR_MAX_CMDS) return false;
  cur_[nCur_++] = c;
  return true;
}

bool Renderer::ovLine(int x0, int y0, int x1, int y1, int w, Mode m) { return addCmd(mkLine(x0, y0, x1, y1, w, m)); }
bool Renderer::ovFillRect(int x, int y, int w, int h, Mode m) { Cmd c; return mkRect(c, OP_FILL_RECT, x, y, w, h, m) && addCmd(c); }
bool Renderer::ovRect(int x, int y, int w, int h, Mode m)     { Cmd c; return mkRect(c, OP_RECT, x, y, w, h, m) && addCmd(c); }
bool Renderer::ovCircle(int cx, int cy, int r, Mode m)        { return addCmd(mkCircle(OP_CIRCLE, cx, cy, r, m)); }
bool Renderer::ovFillCircle(int cx, int cy, int r, Mode m)    { return addCmd(mkCircle(OP_FILL_CIRCLE, cx, cy, r, m)); }
bool Renderer::ovBitmap(int x, int y, int w, int h, const uint8_t* bits, Mode m) {
  Cmd c;
  if (!mkRect(c, OP_BITMAP, x, y, w, h, m)) return false;
  c.bmp = bits;
  return addCmd(c);
}

// Compara con la lista anterior. Camino rapido: mismo comando en la misma posicion (O(n)
// cuando el overlay es estable); solo los que no coinciden buscan en el resto de la lista.
void Renderer::closeOverlay() {
  auto has = [](const Cmd* list, int n, int pos, const Cmd& c) {
    if (pos < n && sameCmd(list[pos], c)) return true;
    for (int i = 0; i < n; i++) if (sameCmd(list[i], c)) return true;
    return false;
  };
  for (int i = 0; i < nPrev_; i++)
    if (!has(cur_, nCur_, i, prev_[i])) markDirtyRect(prev_[i].x0, prev_[i].y0, prev_[i].x1, prev_[i].y1);
  for (int i = 0; i < nCur_; i++)
    if (!has(prev_, nPrev_, i, cur_[i])) markDirtyRect(cur_[i].x0, cur_[i].y0, cur_[i].x1, cur_[i].y1);
  overlayOpen_ = false;
}

// ---- Render ----
void Renderer::composeTile(int tx, int ty) {
  const int px = tx * TILE, py = ty * TILE;
  for (int r = 0; r < TILE; r++)
    memcpy(tile_ + r * TB, base_ + (py + r) * STRIDE + (px >> 3), TB);

  Surface s{tile_, TB, px, py, TILE, TILE};
  for (int i = 0; i < nCur_; i++) {
    const Cmd& c = cur_[i];
    if (c.x1 < px || c.x0 >= px + TILE || c.y1 < py || c.y0 >= py + TILE) continue;
    execCmd(s, c);
  }
}

bool Renderer::tileHasOverlay(int tx, int ty) const {
  const int px = tx * TILE, py = ty * TILE;
  for (int i = 0; i < nCur_; i++) {
    const Cmd& c = cur_[i];
    if (c.x1 < px || c.x0 >= px + TILE || c.y1 < py || c.y0 >= py + TILE) continue;
    return true;
  }
  return false;
}

// Compone el tile, lo compara con el shadow y envia SOLO el rectangulo que cambio
// (filas r0..r1, bytes c0..c1 => ancho multiplo de 8). Devuelve pixeles enviados.
int Renderer::sendTile(int t, bool& open) {
  const int tx = t % TILES_X, ty = t / TILES_X;
  const int px = tx * TILE, py = ty * TILE;
  const uint32_t t0 = MTR_NOW_US();
  st_.processed++;
  composeTile(tx, ty);

  uint8_t* sh = shown_ + py * STRIDE + (px >> 3);
  int r0 = -1, r1 = -1, c0 = TB, c1 = -1;

  if (!shownOk_[t]) {
    r0 = 0; r1 = TILE - 1; c0 = 0; c1 = TB - 1;
  } else {
    for (int r = 0; r < TILE; r++) {
      const uint8_t* a = tile_ + r * TB;
      const uint8_t* b = sh + r * STRIDE;
      bool ch = false;
      for (int k = 0; k < TB; k++)
        if (a[k] != b[k]) { ch = true; if (k < c0) c0 = k; if (k > c1) c1 = k; }
      if (ch) { if (r0 < 0) r0 = r; r1 = r; }
    }
    if (r1 < 0) { st_.composeUs += MTR_NOW_US() - t0; return 0; }   // la pantalla ya muestra esto
  }

  for (int r = r0; r <= r1; r++) memcpy(sh + r * STRIDE, tile_ + r * TB, TB);
  shownOk_[t] = 1;

  const int x = px + c0 * 8, y = py + r0;
  const int w = (c1 - c0 + 1) * 8, h = r1 - r0 + 1;

  const uint32_t t1 = MTR_NOW_US();
  st_.composeUs += t1 - t0;
  if (!open) { if (disp_.begin) disp_.begin(disp_.ctx); open = true; }
  if (disp_.pushMono) {
    disp_.pushMono(disp_.ctx, x, y, w, h, tile_ + r0 * TB + c0, TB);
  } else {
    for (int r = 0; r < h; r++)
      for (int k = c0; k <= c1; k++)
        memcpy(out_ + r * w + (k - c0) * 8, lut_[tile_[(r0 + r) * TB + k]], 8 * sizeof(uint16_t));
    disp_.push(disp_.ctx, x, y, w, h, out_);
  }
  st_.pushUs += MTR_NOW_US() - t1;
  st_.sent++; st_.pixels += (uint32_t)(w * h);
  return w * h;
}

// Pasada 0: tiles sucios que toca el overlay (max 3/4 de cada presupuesto). Pasada 1: el resto.
// Round-robin desde scan_. Los tiles no procesados siguen sucios y se componen con el
// contenido mas reciente. Los tiles sin cambios reales no consumen presupuesto de pixeles,
// pero SI cuentan para maxTiles (cuestan CPU).
int Renderer::flush(int maxPixels, int maxTiles) {
  if (overlayOpen_) closeOverlay();
  st_ = Stats{};

  const int ovLimit = maxPixels ? (maxPixels * 3 + 3) / 4 : 0;
  const int ovTiles = maxTiles  ? (maxTiles * 3 + 3) / 4  : 0;
  int n = 0, proc = 0;
  bool open = false, exhausted = false;

  for (int pass = 0; pass < 2 && !exhausted; pass++) {
    const int limit  = (pass == 0) ? ovLimit : maxPixels;
    const int tlimit = (pass == 0) ? ovTiles : maxTiles;
    for (int i = 0; i < TILES; i++) {
      int t = scan_ + i;
      if (t >= TILES) t -= TILES;
      if (!isDirty(t)) continue;
      if (pass == 0 && !tileHasOverlay(t % TILES_X, t / TILES_X)) continue;

      if ((limit && n >= limit) || (tlimit && proc >= tlimit)) {
        if (pass == 1) { scan_ = (uint16_t)t; exhausted = true; }
        break;
      }
      clearDirty(t);
      proc++;
      n += sendTile(t, open);
    }
  }

  if (open && disp_.end) disp_.end(disp_.ctx);
  return n;
}

}  // namespace mtr