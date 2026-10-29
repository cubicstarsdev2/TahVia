#include "nav_model.h"
#include <stdlib.h>

namespace {
const uint32_t KEEP_BEHIND = 32;                    // points kept behind the cursor
inline int64_t d2(const Pt& p, int32_t x, int32_t y) {
  int64_t dx = (int64_t)p.x - x, dy = (int64_t)p.y - y;
  return dx * dx + dy * dy;
}
}

void NavModel::resetSession() {
  ringA.clear(); ringB.clear(); active = &ringA; shadow = &ringB;
  navState = proto::NAV_IDLE; icon = 0; street[0] = 0; distToTurn = 0xFFFF;
  cursor = 0; pose.reset(); routeRev++;
}

void NavModel::pickNearestStart() {
  int32_t px = (int32_t)lround(pose.x()), py = (int32_t)lround(pose.y());
  uint32_t end = active->tail() + 128;
  if (end > active->head()) end = active->head();
  int64_t best = INT64_MAX;
  for (uint32_t i = active->tail(); i < end; i++) {
    int64_t d = d2(active->at(i), px, py);
    if (d < best) { best = d; cursor = i; }
  }
}

void NavModel::commitShadow() {
  RouteRing* t = active; active = shadow; shadow = t;
  shadow->clear();
  cursor = active->tail();
  if (pose.hasFix()) pickNearestStart();
  routeRev++;
}

void NavModel::update(float dt, uint32_t nowMs) {
  pose.update(dt, nowMs);
  if (!active->valid() || !pose.hasFix()) return;
  RouteRing& r = *active;
  if (cursor < r.tail()) cursor = r.tail();
  if (cursor >= r.head()) cursor = r.head() - 1;

  // Greedy forward walk: advance while the next point is not farther than the current one.
  int32_t px = (int32_t)lround(pose.x()), py = (int32_t)lround(pose.y());
  for (int steps = 0; steps < 32 && cursor + 1 < r.head(); steps++) {
    if (d2(r.at(cursor + 1), px, py) <= d2(r.at(cursor), px, py)) cursor++;
    else break;
  }
  r.trimBefore(cursor > KEEP_BEHIND ? cursor - KEEP_BEHIND : 0);   // frees slots for the phone
}