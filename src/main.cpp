/*
  TahVia firmware - ESP32 + TFT_eSPI (ILI9488 480x320) + MonoTileRenderer + NimBLE

  Flow
    setup():  boot screen (logo + loading bar)  ->  idle screen
    loop():   IDLE  while the phone is not connected, or connected with no route
              NAV   while connected, a route is active and NAV_STATE != idle
              (the screen switches back automatically on disconnect / ROUTE_CLEAR / NAV_STATE idle)

  Modules (README section 2)
    ble_nav.*      NimBLE server, RX queue, protocol parser, ACKs
    route_ring.h   ring buffer of route points + shadow buffer for reroutes
    pose_filter.h  dead reckoning, fix blending, heading smoothing
    nav_model.*    shared state + cursor tracking + trimming
    ui.*           boot / idle / navigation HUD
    battery.h      optional ADC battery reading

  The renderer is only touched from loop() (and setup()).
*/
#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include "MonoTileRenderer.h"
#include "config.h"
#include "battery.h"
#include "nav_model.h"
#include "ble_nav.h"
#include "ota.h"
#include "ui.h"

// ---- Tile budget (NAV screen only) ---------------------------------------------------------------
// NAV_MAX_TILES  max tiles sent to the panel per frame in NAV. Tiles that don't fit stay dirty and
//                are sent on the following frames (higher loop rate, possible tearing between frames).
//                  0    = adaptive: the budget follows the measured SPI cost per tile
//                  N>0  = fixed at N tiles per frame (the whole map is ~90 tiles, the screen is 150)
//                Can be overridden from config.h or the build flags (-DNAV_MAX_TILES=24).
// IDLE is always unlimited (it is mostly static).
#ifndef NAV_MAX_TILES
#define NAV_MAX_TILES          24
#endif
// Only used when NAV_MAX_TILES == 0 (adaptive):
#ifndef NAV_FLUSH_TIME_SHARE
#define NAV_FLUSH_TIME_SHARE   0.60f   // fraction of the frame period that may be spent sending tiles
#endif
#ifndef NAV_BUDGET_MIN
#define NAV_BUDGET_MIN         8
#endif
#ifndef NAV_BUDGET_MAX
#define NAV_BUDGET_MAX         mtr::TILES
#endif

static TFT_eSPI      tft;
static mtr::Renderer rend;
static NavModel      model;
static BleNav        ble;

static ui::Screen screen = ui::SCR_IDLE;

// ---- Display callbacks (TFT_eSPI) -----------------------------------------------------------
static void dBegin(void* c) { ((TFT_eSPI*)c)->startWrite(); }
static void dPush(void* c, int x, int y, int w, int h, const uint16_t* px) {
  ((TFT_eSPI*)c)->pushImage(x, y, w, h, (uint16_t*)px);
}
static void dEnd(void* c) { ((TFT_eSPI*)c)->endWrite(); }

// ---- Colors (invert flag from CONFIG) --------------------------------------------------------
static bool appliedInvert = false;
static void applyColors(bool invert) {
  if (invert) rend.setColors(TFT_WHITE, TFT_BLACK);
  else        rend.setColors(TFT_BLACK, TFT_WHITE);
  appliedInvert = invert;
}

// ---- Boot ------------------------------------------------------------------------------------
static float bootShown = 0;

// Animate the bar up to `target` percent (at least BOOT_BAR_RATE %/s), flushing each step.
static void bootTo(int target) {
  uint32_t t = millis();
  rend.flush();
  while (bootShown < target) {
    uint32_t n = millis();
    bootShown += BOOT_BAR_RATE * (n - t) * 1e-3f;
    t = n;
    if (bootShown > target) bootShown = (float)target;
    ui::bootBar((int)bootShown);
    rend.flush();
    delay(15);
  }
}

void setup() {
  Serial.begin(9600);

  tft.init();
  tft.setRotation(1);                    // 480x320 landscape
  tft.fillScreen(TFT_WHITE);

  mtr::Display d{};
  d.ctx = &tft; d.begin = dBegin; d.push = dPush; d.wait = nullptr; d.end = dEnd;
  rend.begin(d);
  applyColors(false);

  // Boot screen first (only the logo sprites are built before the first frame)
  ui::initBoot(&tft, &rend);
  ui::bootDraw();
  rend.flush();

  ui::bootLabel("Loading interface...");   rend.flush();
  ui::initRest();                          bootTo(40);

  ui::bootLabel("Checking battery...");    rend.flush();
  battery::begin();                        bootTo(55);

  ui::bootLabel("Starting Bluetooth...");  rend.flush();
  ble.begin(&model);                       bootTo(90);

  ui::bootLabel("Ready");                  bootTo(100);
  delay(300);

  screen = ui::SCR_IDLE;                   // boot screen -> idle screen
  ui::enter(screen);
  Serial.println("TahVia ready");
}

// ---- Main loop ---------------------------------------------------------------------------------
static uint32_t frameUs = FRAME_US_IDLE;
static uint32_t nextFrameUs = 0, lastUs = 0;
static uint32_t statT0 = 0, lastBattBle = 0;
static int      statFrames = 0;
static long     statTiles = 0;
static float    fps = 0;
static int      avgTiles = 0;

#if NAV_MAX_TILES > 0
static int      tileBudget = NAV_MAX_TILES;     // fixed
#else
static int      tileBudget = 32;                // adaptive: starting value
static float    usPerTile  = 700.f;             // moving average of the cost per sent tile (us)
#endif

void loop() {
  uint32_t nowMs = millis();
  ble.poll(nowMs);                                          // parse BLE traffic as soon as it arrives

  uint32_t nowUs = micros();
  if (nextFrameUs == 0) { nextFrameUs = nowUs; lastUs = nowUs; statT0 = nowMs; }
  if ((int32_t)(nowUs - nextFrameUs) < 0) {
    if (nextFrameUs - nowUs > 2000) delay(1);               // yield so the BLE task gets CPU
    return;
  }
  nextFrameUs += frameUs;
  if ((int32_t)(nowUs - nextFrameUs) > (int32_t)frameUs) nextFrameUs = nowUs + frameUs;   // fell behind
  float dt = (nowUs - lastUs) * 1e-6f;
  lastUs = nowUs;
  if (dt > 0.1f) dt = 0.1f;

  // ---- which screen? ----
  // The update screen wins over everything: from OTA_BEGIN until the error screen times out / reboot.
  ui::Screen want = ota::active() ? ui::SCR_UPDATE
                  : (ble.connected() && model.hasRoute() && model.navState != proto::NAV_IDLE)
                      ? ui::SCR_NAV : ui::SCR_IDLE;
  if (want != screen) {
    screen = want;
    ui::enter(screen);
    frameUs = (screen == ui::SCR_NAV) ? FRAME_US_NAV : FRAME_US_IDLE;
    nextFrameUs = nowUs;
  }

  // ---- state ----
  ota::update(nowMs);                                       // OTA timeouts, error screen timer, deferred reboot
  model.update(dt, nowMs);
  battery::update(nowMs);
  if (model.cfg.invert != appliedInvert) applyColors(model.cfg.invert);
  int pct = battery::percent();
  if (pct >= 0 && nowMs - lastBattBle > 30000) { lastBattBle = nowMs; ble.setBatteryLevel((uint8_t)pct); }

  // ---- draw ----
  ui::Hud h;
  h.battPct = pct; h.connected = ble.connected(); h.pairing = ble.pairing();
  h.passkey = ble.passkey(); h.showFps = model.cfg.showFps; h.fps = fps; h.tiles = avgTiles; h.nowMs = nowMs;

  if      (screen == ui::SCR_NAV)    ui::navFrame(model, h);
  else if (screen == ui::SCR_UPDATE) ui::updateFrame(ota::info(), h);
  else                               ui::idleFrame(h);

  const int budget = (screen == ui::SCR_NAV) ? tileBudget : 0;   // 0 = unlimited
  uint32_t f0 = micros();
  int tiles = rend.flush(budget);
  uint32_t flushUs = micros() - f0;

#if NAV_MAX_TILES <= 0
  // Adaptive budget: make flushing fit in NAV_FLUSH_TIME_SHARE of the frame period.
  if (screen == ui::SCR_NAV && tiles >= 4) {
    usPerTile += 0.2f * ((float)flushUs / tiles - usPerTile);
    if (usPerTile < 50.f) usPerTile = 50.f;
    int b = (int)(frameUs * NAV_FLUSH_TIME_SHARE / usPerTile);
    tileBudget = b < NAV_BUDGET_MIN ? NAV_BUDGET_MIN : (b > NAV_BUDGET_MAX ? NAV_BUDGET_MAX : b);
  }
#else
  (void)flushUs;
#endif

  // ---- stats (once per second) ----
  statFrames++; statTiles += tiles;
  uint32_t el = millis() - statT0;
  if (el >= 1000) {
    fps = statFrames * 1000.0f / el;                         // NOTE: loop rate, not panel refresh rate
    avgTiles = (int)(statTiles / statFrames);
    Serial.printf("%s %.0f fps %d tiles  budget=%d pending=%d  bad=%lu dropped=%lu free=%u\n",
                  screen == ui::SCR_NAV ? "NAV" : "IDLE", fps, avgTiles,
                  tileBudget, rend.dirtyCount(),
                  (unsigned long)model.badMsgs, (unsigned long)model.droppedMsgs,
                  model.active->freeSlots());
    statT0 = millis(); statFrames = 0; statTiles = 0;
  }
}