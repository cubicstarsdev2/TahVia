// PoseFilter - dead reckoning at display rate + fix blending + heading smoothing (README 6.7).
#pragma once
#include <stdint.h>
#include <math.h>
#include "config.h"

class PoseFilter {
 public:
  void reset() { init_ = false; gpsValid_ = false; stale_ = false; x_ = y_ = 0; pendX_ = pendY_ = 0;
                 hdg_ = raw_ = 0; speed_ = 0; }

  // Called when a POSE message arrives. x,y in dm; heading in 0.1 deg; speed in cm/s.
  void onPose(int32_t x, int32_t y, uint16_t hdgDeci, uint16_t speedCms, bool valid, uint32_t now) {
    lastMs_ = now;
    if (!valid) { gpsValid_ = false; return; }
    gpsValid_ = true; stale_ = false;
    raw_   = hdgDeci * 0.1f;
    speed_ = speedCms * 0.1f;                       // dm/s
    if (!init_) { x_ = x; y_ = y; pendX_ = pendY_ = 0; hdg_ = raw_; init_ = true; return; }
    double ex = x - x_, ey = y - y_;
    if (fabs(ex) > JUMP_DM || fabs(ey) > JUMP_DM) { x_ = x; y_ = y; pendX_ = pendY_ = 0; }  // teleport
    else { pendX_ = ex; pendY_ = ey; }                                                      // blend
  }

  void update(float dt, uint32_t now) {
    if (!init_) return;
    stale_ = (now - lastMs_) > POSE_TIMEOUT_MS;
    // heading: shortest arc, low-pass
    float d = raw_ - hdg_;
    while (d > 180.f) d -= 360.f;
    while (d < -180.f) d += 360.f;
    hdg_ += d * (1.f - expf(-dt / HDG_TAU));
    while (hdg_ >= 360.f) hdg_ -= 360.f;
    while (hdg_ < 0.f) hdg_ += 360.f;
    if (!stale_ && gpsValid_) {                      // dead reckoning
      float th = raw_ * 0.017453292f;
      x_ += speed_ * dt * sinf(th);
      y_ += speed_ * dt * cosf(th);
    }
    float f = 1.f - expf(-dt / BLEND_TAU);           // fix blending (~300 ms)
    x_ += pendX_ * f; pendX_ -= pendX_ * f;
    y_ += pendY_ * f; pendY_ -= pendY_ * f;
  }

  void rebase(int32_t dx, int32_t dy) { x_ -= dx; y_ -= dy; }

  bool   hasFix()  const { return init_; }
  bool   stale()   const { return stale_ || !gpsValid_; }
  double x()       const { return x_; }              // dm
  double y()       const { return y_; }
  float  heading() const { return hdg_; }            // deg clockwise from north
  float  speedMs() const { return stale() ? 0.f : speed_ * 0.1f; }

 private:
  static constexpr double JUMP_DM  = 5000.0;         // >500 m: snap instead of blend
  static constexpr float  BLEND_TAU = 0.10f;
  static constexpr float  HDG_TAU   = 0.25f;
  bool   init_ = false, gpsValid_ = false, stale_ = false;
  double x_ = 0, y_ = 0, pendX_ = 0, pendY_ = 0;
  float  hdg_ = 0, raw_ = 0, speed_ = 0;
  uint32_t lastMs_ = 0;
};