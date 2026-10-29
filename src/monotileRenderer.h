// MonoTileRenderer - renderizador 1 bpp por tiles con seguimiento de tiles sucios.
//
// Dos capas, ambas en 1 bit por pixel (1 = primer plano, 0 = fondo):
//
//   BASE     Buffer persistente de 480x320 (19.200 bytes). Aqui se dibuja el mapa.
//            Las funciones base*() dibujan de inmediato y marcan tiles sucios.
//
//   OVERLAY  Lista de comandos (marcador, flecha de giro, HUD). Se re-emite en cada
//            frame con overlayBegin() + ov*(). El renderizador compara con el frame
//            anterior y solo ensucia los tiles de los comandos que cambiaron.
//
// flush(maxTiles): por cada tile sucio copia el tile de BASE, le aplica los comandos de
// OVERLAY que lo tocan, expande 1 bit -> RGB565 (2 colores) y lo envia a la pantalla.
// Con maxTiles > 0 envia como maximo esa cantidad de tiles por llamada; los demas siguen
// marcados como sucios y se envian en las llamadas siguientes (siempre con el contenido
// mas reciente de BASE + OVERLAY). Los tiles que tocan el overlay tienen prioridad.
//
// No depende de TFT_eSPI: el envio a pantalla es un callback (ver struct Display).
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
#define MTR_MAX_CMDS 96       // comandos de overlay por frame (x2 listas)
#endif
#ifndef MTR_SWAP_BYTES
#define MTR_SWAP_BYTES 1      // 1 = RGB565 con bytes intercambiados (pushImageDMA con swapBytes=false)
#endif

namespace mtr {

constexpr int TILE    = 32;                    // 32 px = 4 bytes por fila de tile
constexpr int TILES_X = MTR_WIDTH / TILE;
constexpr int TILES_Y = MTR_HEIGHT / TILE;
constexpr int TILES   = TILES_X * TILES_Y;     // 150 con 480x320
constexpr int STRIDE  = MTR_WIDTH / 8;         // bytes por fila del buffer base

static_assert(MTR_WIDTH % TILE == 0 && MTR_HEIGHT % TILE == 0,
              "El tamano de pantalla debe ser multiplo de TILE");

enum Mode : uint8_t { CLEAR = 0, SET = 1, XOR = 2 };

enum Op : uint8_t { OP_LINE, OP_FILL_RECT, OP_RECT, OP_CIRCLE, OP_FILL_CIRCLE, OP_BITMAP };

// Comando de dibujo. Se construye siempre con memset(0) previo para poder
// compararlo con memcmp entre frames.
struct Cmd {
  uint8_t  op, mode, width, _pad;
  int16_t  a, b, c, d;        // argumentos segun op (ver abajo)
  int16_t  x0, y0, x1, y1;    // bounding box inclusivo en pantalla
  const uint8_t* bmp;         // solo OP_BITMAP
};
// OP_LINE: a,b=x0,y0  c,d=x1,y1  width | OP_FILL_RECT/OP_RECT: a,b=x,y c,d=w,h
// OP_CIRCLE/OP_FILL_CIRCLE: a,b=cx,cy c=r | OP_BITMAP: a,b=x,y c,d=w,h bmp (1bpp MSB primero)

// Callbacks hacia la pantalla. Hace falta `push` o `pushMono` (si hay pushMono se usa ese).
struct Display {
  void* ctx;
  void (*begin)(void* ctx);                                                 // ej. tft.startWrite()
  void (*push)(void* ctx, int x, int y, int w, int h, const uint16_t* px);  // ej. tft.pushImageDMA(...)
  void (*wait)(void* ctx);                                                  // ej. tft.dmaWait()
  void (*end)(void* ctx);                                                   // ej. tft.endWrite()
  // Alternativa a push: recibe el tile en 1 bpp (MSB primero, w/8 bytes por fila) y el callback lo
  // expande al formato nativo de la pantalla (ej. RGB666 para ILI9488 por SPI). Ahorra la expansion a RGB565.
  void (*pushMono)(void* ctx, int x, int y, int w, int h, const uint8_t* bits);
};

class Renderer {
 public:
  void begin(const Display& d);
  void setColors(uint16_t fg, uint16_t bg);   // RGB565 normal; el swap de bytes lo hace la libreria
  void invalidateAll();                       // marca todos los tiles como sucios
  void forceResend() { memset(sentOk_, 0, sizeof(sentOk_)); }  // olvida lo enviado: llamar si algo mas dibujo en la pantalla

  // ---- BASE (inmediato, persistente) ----
  void baseClear(Mode m = CLEAR);
  void baseLine(int x0, int y0, int x1, int y1, int width = 1, Mode m = SET);
  void basePolyline(const int16_t* xy, int nPoints, int width = 1, Mode m = SET);  // xy = x0,y0,x1,y1...
  void baseFillRect(int x, int y, int w, int h, Mode m = SET);
  void baseRect(int x, int y, int w, int h, Mode m = SET);
  void baseCircle(int cx, int cy, int r, Mode m = SET);
  void baseFillCircle(int cx, int cy, int r, Mode m = SET);
  void baseBitmap(int x, int y, int w, int h, const uint8_t* bits, Mode m = SET);
  bool basePixel(int x, int y) const;

  // ---- OVERLAY (lista de comandos, se re-emite cada frame) ----
  void overlayBegin();                        // descarta la lista y guarda la anterior para comparar
  bool ovLine(int x0, int y0, int x1, int y1, int width = 1, Mode m = SET);
  bool ovFillRect(int x, int y, int w, int h, Mode m = SET);
  bool ovRect(int x, int y, int w, int h, Mode m = SET);
  bool ovCircle(int cx, int cy, int r, Mode m = SET);
  bool ovFillCircle(int cx, int cy, int r, Mode m = SET);
  bool ovBitmap(int x, int y, int w, int h, const uint8_t* bits, Mode m = SET);  // `bits` debe seguir vivo (flash)

  // ---- Salida ----
  // Envia tiles sucios. maxTiles = 0 -> sin limite (pantalla completa, ej. boot).
  // maxTiles > 0 -> como maximo esa cantidad de tiles *enviados* por llamada; el resto queda
  // pendiente (sigue sucio). Devuelve cuantos tiles se enviaron.
  int  flush(int maxTiles = 0);
  int  dirtyCount() const;                    // tiles todavia pendientes (sucios)
  void markDirtyRect(int x0, int y0, int x1, int y1);   // inclusivo

 private:
  bool addCmd(const Cmd& c);
  void closeOverlay();
  void composeTile(int tx, int ty);           // base + overlay -> tile_ (1 bpp)
  void expandTile(uint16_t* out);             // tile_ -> RGB565
  uint32_t hashTile() const;
  void buildLut();
  bool isDirty(int t) const { return dirty_[t >> 5] & (1u << (t & 31)); }
  void clearDirty(int t) { dirty_[t >> 5] &= ~(1u << (t & 31)); }
  bool tileHasOverlay(int tx, int ty) const;  // algun comando del overlay actual toca este tile
  bool sendTile(int t, bool& open);           // compone + hash + envia; false si la pantalla ya lo mostraba

  Display  disp_{};
  uint16_t fg_ = 0x0000, bg_ = 0xFFFF;
  uint8_t  base_[MTR_HEIGHT * STRIDE];
  uint32_t dirty_[(TILES + 31) / 32];
  Cmd      cur_[MTR_MAX_CMDS], prev_[MTR_MAX_CMDS];
  uint16_t nCur_ = 0, nPrev_ = 0;
  bool     overlayOpen_ = false;
  alignas(4) uint8_t tile_[TILE * TILE / 8];
  uint32_t sent_[TILES];                      // hash del ultimo contenido enviado a cada tile
  uint8_t  sentOk_[TILES];                    // 1 si sent_[t] es valido
  uint16_t out_[2][TILE * TILE];
  uint16_t lut_[256][8];                      // byte de 8 pixeles -> 8 RGB565
  uint8_t  outIdx_ = 0;
  uint16_t scan_ = 0;                         // donde empieza el barrido round-robin del proximo flush
};

}  // namespace mtr