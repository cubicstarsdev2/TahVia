// NavModel - everything the BLE parser writes and the UI reads. Only touched from loop().
#pragma once
#include "route_ring.h"
#include "pose_filter.h"

struct NavConfig {
  uint16_t zoom = 60;              // px per 100 m
  bool headingUp = true, invert = false, showFps = false, imperial = false;
};

struct NavModel {
  RouteRing  ringA, ringB;
  RouteRing* active = &ringA;      // drawn
  RouteRing* shadow = &ringB;      // being uploaded
  PoseFilter pose;
  NavConfig  cfg;

  uint8_t  navState = proto::NAV_IDLE;
  uint8_t  icon = 0;
  char     street[41] = {0};
  uint16_t distToTurn = 0xFFFF;
  uint32_t cursor = 0;             // global index of the route point nearest to the marker
  uint32_t routeRev = 0;           // bumped whenever the ACTIVE route changes (forces a map redraw)
  uint32_t badMsgs = 0, droppedMsgs = 0;

  bool hasRoute() const { return active->count() >= 2; }
  void resetSession();             // HELLO / disconnect: forget everything rebuildable
  void commitShadow();             // ROUTE_COMMIT: swap shadow -> active
  void update(float dt, uint32_t nowMs);   // pose filter, cursor, trimming
 private:
  void pickNearestStart();
};