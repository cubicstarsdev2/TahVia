// RouteRing - ring buffer of route points addressed by GLOBAL index (README 6.6).
// The device keeps ~1024 points and trims behind the marker; the phone streams the rest.
#pragma once
#include <stdint.h>
#include <string.h>
#include "protocol.h"

struct Pt { int32_t x, y; };      // decimeters, local frame (x east, y north)

class RouteRing {
 public:
  enum : uint32_t { CAP = 1024, MASK = CAP - 1 };

  void clear() { used_ = false; head_ = tail_ = 0; }
  // New route: point `startIdx` is (x0,y0). Next expected index = startIdx + 1.
  void begin(uint8_t id, uint32_t startIdx, int32_t x0, int32_t y0) {
    used_ = true; id_ = id; tail_ = startIdx; head_ = startIdx + 1;
    pts_[startIdx & MASK].x = x0; pts_[startIdx & MASK].y = y0;
  }
  // Append delta-encoded points (dx i16, dy i16 little-endian). Returns a proto::Ack status.
  uint8_t append(uint32_t startIdx, const uint8_t* dl, uint8_t count) {
    if (!used_) return proto::ACK_UNKNOWN;
    if (startIdx != head_) return proto::ACK_GAP;
    if (count > freeSlots()) return proto::ACK_FULL;
    int32_t x = pts_[(head_ - 1) & MASK].x, y = pts_[(head_ - 1) & MASK].y;
    for (uint8_t i = 0; i < count; i++) {
      int16_t dx, dy;
      memcpy(&dx, dl + 4 * i, 2); memcpy(&dy, dl + 4 * i + 2, 2);
      x += dx; y += dy;
      pts_[head_ & MASK].x = x; pts_[head_ & MASK].y = y;
      head_++;
    }
    return proto::ACK_OK;
  }
  void rebase(int32_t dx, int32_t dy) {
    for (uint32_t i = tail_; i < head_; i++) { pts_[i & MASK].x -= dx; pts_[i & MASK].y -= dy; }
  }
  // Drop points before idx (always keeps the last point: it is the delta reference).
  void trimBefore(uint32_t idx) {
    if (head_ == tail_) return;
    if (idx > head_ - 1) idx = head_ - 1;
    if (idx > tail_) tail_ = idx;
  }

  bool     valid()  const { return used_; }
  uint8_t  id()     const { return id_; }
  uint32_t tail()   const { return tail_; }
  uint32_t head()   const { return head_; }          // one past the last point = next expected index
  uint32_t count()  const { return head_ - tail_; }
  uint16_t freeSlots() const { return (uint16_t)(CAP - count()); }
  const Pt& at(uint32_t idx) const { return pts_[idx & MASK]; }

 private:
  Pt       pts_[CAP];
  uint32_t head_ = 0, tail_ = 0;
  uint8_t  id_ = 0;
  bool     used_ = false;
};