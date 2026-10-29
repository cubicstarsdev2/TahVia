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
// NOTE: rend.flush() budgets are in PIXELS (renderer v2), so the tile counts below are converted with
//       TILE_PX. (Before, 24 was passed as 24 pixels = about one tile per frame.)
// NAV_MAX_TILES  max tiles sent to the panel per frame in NAV. Tiles that don't fit stay dirty and
//                are sent on the following frames (higher loop rate, possible tearing between frames).
//                  0    = adaptive: the budget follows the measured SPI cost per tile
//                  N>0  = fixed at N tiles per frame (the whole map is ~90 tiles, the screen is 150)
//                Can be overridden from config.h or the build flags (-DNAV_MAX_TILES=24).
// IDLE is always unlimited (it is mostly static).
// Default = 24 tiles' worth of pixels at 32 px tiles, whatever MTR_TILE is (-DMTR_TILE=16 -> 96 tiles of 16x16).
#ifndef NAV_MAX_TILES
#define NAV_MAX_TILES          (24 * 32 * 32 / (MTR_TILE * MTR_TILE))
#endif
// NAV_MAX_PROC   max tiles composed + compared per frame in NAV (changed or not). 0 = unlimited.
//                Bounds CPU time when a map redraw dirties every tile; the rest stays dirty for the next frame.
#ifndef NAV_MAX_PROC
#define NAV_MAX_PROC           0
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

static constexpr int TILE_PX = mtr::TILE * mtr::TILE;   // pixels in a full tile

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
  Serial.begin(9600);                  // 9600 baud made the once-per-second stats line block loop()

  tft.init();
  tft.setRotation(1);                    // 480x320 landscape
  tft.fillScreen(TFT_WHITE);

  mtr::Display d{};
  d.ctx = &tft; d.begin = dBegin; d.push = dPush; d.end = dEnd;
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
static long     statPx = 0;
static uint32_t statComposeUs = 0, statPushUs = 0, statProc = 0, statSent = 0;
static float    fps = 0;
static int      avgKpx = 0;

#if NAV_MAX_TILES > 0
static int      pxBudget = NAV_MAX_TILES * TILE_PX;   // fixed (pixels)
#else
static int      pxBudget = 32 * TILE_PX;              // adaptive: starting value (pixels)
static float    usPerPx  = 0.8f;                      // moving average of the cost per sent pixel (us)
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
  h.passkey = ble.passkey(); h.showFps = model.cfg.showFps; h.fps = fps; h.kpx = avgKpx; h.nowMs = nowMs;

  if      (screen == ui::SCR_NAV)    ui::navFrame(model, h);
  else if (screen == ui::SCR_UPDATE) ui::updateFrame(ota::info(), h);
  else                               ui::idleFrame(h);

  const int budget = (screen == ui::SCR_NAV) ? pxBudget : 0;   // 0 = unlimited
  uint32_t f0 = micros();
  int px = rend.flush(budget, screen == ui::SCR_NAV ? NAV_MAX_PROC : 0);
  { const mtr::Renderer::Stats& rs = rend.lastStats();
    statComposeUs += rs.composeUs; statPushUs += rs.pushUs; statProc += rs.processed; statSent += rs.sent; }
  uint32_t flushUs = micros() - f0;

#if NAV_MAX_TILES <= 0
  // Adaptive budget: make flushing fit in NAV_FLUSH_TIME_SHARE of the frame period.
  if (screen == ui::SCR_NAV && px >= 512) {
    usPerPx += 0.2f * ((float)flushUs / px - usPerPx);
    if (usPerPx < 0.05f) usPerPx = 0.05f;
    int b = (int)(frameUs * NAV_FLUSH_TIME_SHARE / usPerPx);
    const int lo = NAV_BUDGET_MIN * TILE_PX, hi = NAV_BUDGET_MAX * TILE_PX;
    pxBudget = b < lo ? lo : (b > hi ? hi : b);
  }
#else
  (void)flushUs;
#endif

  // ---- stats (once per second) ----
  statFrames++; statPx += px;
  uint32_t el = millis() - statT0;
  if (el >= 1000) {
    fps = statFrames * 1000.0f / el;                         // NOTE: loop rate, not panel refresh rate
    avgKpx = (int)(statPx / statFrames / 1000);
    Serial.printf("%s %.0f fps %d kpx  budget=%d kpx pending=%d tiles  proc=%lu sent=%lu compose=%luus push=%luus (per frame)  bad=%lu dropped=%lu free=%u\n",
                  screen == ui::SCR_NAV ? "NAV" : "IDLE", fps, avgKpx,
                  pxBudget / 1000, rend.dirtyCount(),
                  (unsigned long)(statProc / statFrames), (unsigned long)(statSent / statFrames),
                  (unsigned long)(statComposeUs / statFrames), (unsigned long)(statPushUs / statFrames),
                  (unsigned long)model.badMsgs, (unsigned long)model.droppedMsgs,
                  model.active->freeSlots());
    statT0 = millis(); statFrames = 0; statPx = 0;
    statComposeUs = statPushUs = statProc = statSent = 0;
  }
}