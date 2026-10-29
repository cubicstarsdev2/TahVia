// UI: boot screen, idle screen, navigation HUD and firmware update screen.
// All drawing goes through MonoTileRenderer. Text/icons are 1-bpp TFT_eSprites blitted as bitmaps.
#pragma once
#include <TFT_eSPI.h>
#include "MonoTileRenderer.h"
#include "nav_model.h"
#include "ota.h"

namespace ui {

enum Screen : uint8_t { SCR_IDLE = 0, SCR_NAV = 1, SCR_UPDATE = 2 };

struct Hud {                 // per-frame data owned by main
  int      battPct;          // -1 = unknown
  bool     connected;
  bool     pairing;          // show passkey
  bool     showFps;          // CONFIG flag bit2
  uint32_t passkey;
  float    fps;
  int      tiles;
  uint32_t nowMs;
};

// ---- Boot (drawn in BASE) ----
void initBoot(TFT_eSPI* tft, mtr::Renderer* r);   // logo + title sprites only (fast)
void initRest();                                  // all other sprites and icons
void bootDraw();                                  // logo, title, empty bar
void bootLabel(const char* s);
void bootBar(int pct);                            // 0..100

// ---- Runtime screens ----
void enter(Screen s);                             // clears BASE; call on every screen change
void idleFrame(const Hud& h);                     // phone not connected / no route
void navFrame(NavModel& m, const Hud& h);         // route + marker + HUD
void updateFrame(const ota::Info& o, const Hud& h);   // firmware update: progress / done / failed
}  // namespace ui