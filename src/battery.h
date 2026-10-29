// Battery level. Three modes, highest priority first:
//   BATT_STATIC_PCT=0..100  fixed value (no battery connected); the ADC is never read
//   BATT_ADC_PIN=<ADC1 pin> LiPo behind a divider (BATT_DIVIDER = Vbat / Vpin)
//   neither                 percent() returns -1 and the UI shows "--"
#pragma once
#include <Arduino.h>
#ifndef BATT_ADC_PIN
#define BATT_ADC_PIN -1
#endif
#ifndef BATT_STATIC_PCT
#define BATT_STATIC_PCT -1
#endif
#ifndef BATT_DIVIDER
#define BATT_DIVIDER 2.0f        // Vbat / Vpin
#endif

namespace battery {
static float    g_mv = 0;
static int      g_pct = -1;
static uint32_t g_last = 0;

static int mvToPct(float mv) {
  static const uint16_t T[11] = {3300, 3500, 3600, 3680, 3720, 3760, 3800, 3850, 3920, 4000, 4150};
  if (mv <= T[0]) return 0;
  if (mv >= T[10]) return 100;
  for (int i = 0; i < 10; i++)
    if (mv < T[i + 1]) return (int)(i * 10 + 10.0f * (mv - T[i]) / (T[i + 1] - T[i]));
  return 100;
}
static void sample(bool first) {
#if BATT_STATIC_PCT >= 0
  (void)first;
#elif BATT_ADC_PIN >= 0
  float mv = analogReadMilliVolts(BATT_ADC_PIN) * BATT_DIVIDER;
  g_mv = first ? mv : g_mv * 0.8f + mv * 0.2f;
  g_pct = mvToPct(g_mv);
#else
  (void)first;
#endif
}
static void begin() {
#if BATT_STATIC_PCT >= 0
  g_pct = BATT_STATIC_PCT > 100 ? 100 : BATT_STATIC_PCT;
#elif BATT_ADC_PIN >= 0
  pinMode(BATT_ADC_PIN, INPUT);
  for (int i = 0; i < 8; i++) sample(i == 0);
#endif
}
static void update(uint32_t now) { if (now - g_last >= 2000) { g_last = now; sample(false); } }
static int  percent() { return g_pct; }
}  // namespace battery