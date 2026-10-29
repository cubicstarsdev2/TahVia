// BleNav - NimBLE GATT server + protocol parser (README section 6).
// BLE callbacks only copy bytes into a FreeRTOS queue; everything is parsed in poll(),
// which runs from loop(), so NavModel and the renderer never see another thread.
#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "nav_model.h"

class BleNav {
 public:
  void begin(NavModel* model);
  void poll(uint32_t nowMs);                 // drain RX queue, send housekeeping ACKs
  bool connected() const { return connected_; }
  bool pairing()   const { return pairing_; }       // passkey is being displayed
  uint32_t passkey() const { return passkey_; }
  void setBatteryLevel(uint8_t pct);         // standard Battery Service (0x180F)

  // --- internal: called from NimBLE callbacks (other task) ---
  void _onConnect();
  void _onDisconnect();
  void _onRx(const uint8_t* d, size_t n);
  uint32_t _onPasskey();
  void _onAuthDone();

 private:
  struct RxMsg { uint8_t len; uint8_t data[244]; };
  void handle(const uint8_t* d, size_t n, uint32_t now);
  void send(const uint8_t* d, size_t n);
  void sendHelloAck();
  void sendAck(uint8_t id, uint8_t status, uint32_t expected, uint16_t freeSlots);
  void ackFor(const RouteRing& r, uint8_t status) { sendAck(r.id(), status, r.head(), r.freeSlots()); }

  NavModel* m_ = nullptr;
  QueueHandle_t q_ = nullptr;
  volatile bool connected_ = false, pairing_ = false, evtDisc_ = false;
  volatile uint32_t passkey_ = 0;
  uint8_t  chunksSinceAck_ = 0;
  uint16_t lastFree_ = 0;
  uint32_t lastTopUpMs_ = 0;
};