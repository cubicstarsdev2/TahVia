// MonoTileRenderer v2 - renderizador 1 bpp por tiles, pensado para SPI SIN DMA.
//
// Cuello de botella sin DMA: la CPU espera cada byte enviado. Por eso v2 minimiza PIXELES enviados:
//   - Se mantiene un "shadow" 1 bpp de lo que la pantalla muestra (shown_).
//   - Cada tile sucio se compone y se compara con el shadow; solo se envia el rectangulo
//     minimo (alineado a 8 px en x) que cambio. Tiles identicos no cuestan nada.
//   - El presupuesto de flush() es en pixeles, no en tiles.
//
// Capas (1 bit por pixel, 1 = primer plano):
//   BASE     buffer persistente (mapa). base*() dibuja y marca tiles sucios.
//   OVERLAY  lista de comandos que se re-emite cada frame (overlayBegin() + ov*()).
//
// Memoria: base_ + shown_ = 2 x 19.200 bytes (480x320).
#pragma once
#include <stdint.h>
#include <string.h>

#ifndef MTR_WIDTH
#define MTR_WIDTH 480
#endif
#ifndef MTR_HEIGHT
#define MTR_HEIGHT 320
#endif
#ifndef MTR_MAX_CMDS
#define MTR_MAX_CMDS 96
#endif
#ifndef MTR_TILE
#define MTR_TILE 8           // tile side in px: multiple of 8 that divides the screen (8, 16, 32, 40...)
#endif                        //   small = tighter changed-rectangle (fewer unchanged pixels sent), more tiles to scan
                              //   big   = less per-tile overhead (window setup), more unchanged pixels inside the rectangle
#ifndef MTR_STATS
#define MTR_STATS 1           // measure compose vs push time per flush (needs Arduino micros(); otherwise reports 0)
#endif
#if MTR_STATS && defined(ARDUINO)
  #include <Arduino.h>
  #define MTR_NOW_US() ((uint32_t)micros())
#else
  #define MTR_NOW_US() ((uint32_t)0)
#endif
#ifndef MTR_SWAP_BYTES
#define MTR_SWAP_BYTES 1      // RGB565 con bytes intercambiados (solo para el camino push)
#endif

namespace mtr {

constexpr int TILE    = MTR_TILE;
constexpr int TB      = TILE / 8;               // bytes por fila de tile
constexpr int TILES_X = MTR_WIDTH / TILE;
constexpr int TILES_Y = MTR_HEIGHT / TILE;
constexpr int TILES   = TILES_X * TILES_Y;
constexpr int STRIDE  = MTR_WIDTH / 8;

static_assert(MTR_TILE % 8 == 0, "MTR_TILE must be a multiple of 8 (x is sent in whole bytes)");
static_assert(MTR_WIDTH % TILE == 0 && MTR_HEIGHT % TILE == 0,
              "El tamano de pantalla debe ser multiplo de TILE");

enum Mode : uint8_t { CLEAR = 0, SET = 1, XOR = 2 };
enum Op : uint8_t { OP_LINE, OP_FILL_RECT, OP_RECT, OP_CIRCLE, OP_FILL_CIRCLE, OP_BITMAP };

struct Cmd {
  uint8_t  op, mode, width, _pad;
  int16_t  a, b, c, d;
  int16_t  x0, y0, x1, y1;    // bbox inclusivo
  const uint8_t* bmp;
};

// Callbacks hacia la pantalla. Hace falta `push` o `pushMono` (si hay pushMono se usa ese).
struct Display {
  void* ctx;
  void (*begin)(void* ctx);                                                 // ej. tft.startWrite()
  void (*push)(void* ctx, int x, int y, int w, int h, const uint16_t* px);  // RGB565 (con swap segun MTR_SWAP_BYTES)
  void (*end)(void* ctx);                                                   // ej. tft.endWrite()
  // RECOMENDADO sin DMA: recibe el rectangulo en 1 bpp (MSB primero). La fila r empieza en
  // bits + r*stride y tiene w/8 bytes utiles. El callback expande al formato nativo con una
  // LUT propia (ej. 3 bytes/pixel para ILI9488) y escribe directo al SPI.
  void (*pushMono)(void* ctx, int x, int y, int w, int h, const uint8_t* bits, int stride);
};

class Renderer {
 public:
  void begin(const Display& d);
  void setColors(uint16_t fg, uint16_t bg);   // RGB565 normal
  uint16_t fg() const { return fg_; }
  uint16_t bg() const { return bg_; }
  void invalidateAll();
  void forceResend() { memset(shownOk_, 0, sizeof(shownOk_)); }  // llamar si algo mas dibujo en la pantalla

  // ---- BASE ----
  void baseClear(Mode m = CLEAR);
  void baseLine(int x0, int y0, int x1, int y1, int width = 1, Mode m = SET);
  void basePolyline(const int16_t* xy, int nPoints, int width = 1, Mode m = SET);
  void baseFillRect(int x, int y, int w, int h, Mode m = SET);
  void baseRect(int x, int y, int w, int h, Mode m = SET);
  void baseCircle(int cx, int cy, int r, Mode m = SET);
  void baseFillCircle(int cx, int cy, int r, Mode m = SET);
  void baseBitmap(int x, int y, int w, int h, const uint8_t* bits, Mode m = SET);
  bool basePixel(int x, int y) const;

  // ---- OVERLAY ----
  void overlayBegin();
  bool ovLine(int x0, int y0, int x1, int y1, int width = 1, Mode m = SET);
  bool ovFillRect(int x, int y, int w, int h, Mode m = SET);
  bool ovRect(int x, int y, int w, int h, Mode m = SET);
  bool ovCircle(int cx, int cy, int r, Mode m = SET);
  bool ovFillCircle(int cx, int cy, int r, Mode m = SET);
  bool ovBitmap(int x, int y, int w, int h, const uint8_t* bits, Mode m = SET);

  // ---- Salida ----
  // maxPixels = 0 -> sin limite. Si no, deja de empezar tiles nuevos al superar el presupuesto
  // (puede pasarse por un tile como maximo); el resto sigue sucio para el proximo flush.
  // maxTiles  = 0 -> sin limite. Si no, procesa (compone + compara) como mucho esos tiles por flush,
  // cambien o no; acota el tiempo de CPU cuando un redibujado del mapa ensucia todos los tiles.
  // Los tiles que tocan el overlay tienen prioridad (hasta 3/4 de cada presupuesto).
  // Devuelve PIXELES enviados.
  int  flush(int maxPixels = 0, int maxTiles = 0);

  // Resultado del ultimo flush (tiempos solo con MTR_STATS y Arduino).
  struct Stats {
    uint16_t processed;   // tiles compuestos y comparados
    uint16_t sent;        // de ellos, los que realmente cambiaron y se enviaron
    uint32_t pixels;      // pixeles enviados
    uint32_t composeUs;   // componer + comparar con shown_
    uint32_t pushUs;      // callback de pantalla (SPI)
  };
  const Stats& lastStats() const { return st_; }
  int  dirtyCount() const;
  void markDirtyRect(int x0, int y0, int x1, int y1);

 private:
  bool addCmd(const Cmd& c);
  void closeOverlay();
  void composeTile(int tx, int ty);
  int  sendTile(int t, bool& open);            // devuelve pixeles enviados (0 = nada cambio)
  void buildLut();
  bool isDirty(int t) const { return dirty_[t >> 5] & (1u << (t & 31)); }
  void clearDirty(int t) { dirty_[t >> 5] &= ~(1u << (t & 31)); }
  bool tileHasOverlay(int tx, int ty) const;

  Display  disp_{};
  uint16_t fg_ = 0x0000, bg_ = 0xFFFF;
  uint8_t  base_[MTR_HEIGHT * STRIDE];
  uint8_t  shown_[MTR_HEIGHT * STRIDE];        // lo que la pantalla muestra (1 bpp)
  uint8_t  shownOk_[TILES];                    // 1 si shown_ de ese tile es valido
  uint32_t dirty_[(TILES + 31) / 32];
  Cmd      cur_[MTR_MAX_CMDS], prev_[MTR_MAX_CMDS];
  uint16_t nCur_ = 0, nPrev_ = 0;
  bool     overlayOpen_ = false;
  alignas(4) uint8_t tile_[TILE * TB];
  uint16_t out_[TILE * TILE];                  // solo camino push
  uint16_t lut_[256][8];
  uint16_t scan_ = 0;
  Stats    st_{};
};

}  // namespace mtr