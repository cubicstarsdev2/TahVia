#include "ble_nav.h"
#include <NimBLEDevice.h>
#include <esp_random.h>
#include "config.h"

using namespace proto;

// Default = NimBLE-Arduino 1.x API. 2.x is used only if the library's version header says major >= 2,
// or if forced with -DTAHVIA_NIMBLE_V2=1 (force 1.x with -DTAHVIA_NIMBLE_V2=0).
#ifndef TAHVIA_NIMBLE_V2
  #if defined(__has_include)
    #if __has_include(<NimBLEVersion.h>)
      #include <NimBLEVersion.h>
    #endif
  #endif
  #if defined(NIMBLE_CPP_VERSION_MAJOR) && (NIMBLE_CPP_VERSION_MAJOR >= 2)
    #define TAHVIA_NIMBLE_V2 1
  #else
    #define TAHVIA_NIMBLE_V2 0
  #endif
#endif

namespace {
BleNav*               g_self = nullptr;
NimBLECharacteristic* g_tx   = nullptr;
NimBLECharacteristic* g_batt = nullptr;

#if TAHVIA_NIMBLE_V2
class ServerCb : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& ci) override {
    s->updateConnParams(ci.getConnHandle(), 12, 24, 0, 400);   // 15-30 ms, 4 s supervision timeout
    g_self->_onConnect();
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) override { g_self->_onDisconnect(); }
  uint32_t onPassKeyDisplay() override { return g_self->_onPasskey(); }
  void onAuthenticationComplete(NimBLEConnInfo& ci) override {
    if (!ci.isEncrypted()) { NimBLEDevice::getServer()->disconnect(ci.getConnHandle()); return; }
    g_self->_onAuthDone();
  }
};
class RxCb : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    auto v = c->getValue();
    g_self->_onRx((const uint8_t*)v.data(), v.size());
  }
};
#else   // NimBLE-Arduino 1.x
class ServerCb : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, ble_gap_conn_desc* d) override {
    s->updateConnParams(d->conn_handle, 12, 24, 0, 400);
    g_self->_onConnect();
  }
  void onDisconnect(NimBLEServer*) override { g_self->_onDisconnect(); }
  uint32_t onPassKeyRequest() override { return g_self->_onPasskey(); }
  void onAuthenticationComplete(ble_gap_conn_desc* d) override {
    if (!d->sec_state.encrypted) { NimBLEDevice::getServer()->disconnect(d->conn_handle); return; }
    g_self->_onAuthDone();
  }
};
class RxCb : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c) override {
    auto v = c->getValue();                                  // std::string or NimBLEAttValue
    g_self->_onRx((const uint8_t*)v.data(), v.size());
  }
};
#endif

inline uint16_t rdU16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }
inline uint32_t rdU32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
inline int32_t  rdI32(const uint8_t* p) { int32_t  v; memcpy(&v, p, 4); return v; }
inline void     wrU16(uint8_t* p, uint16_t v) { memcpy(p, &v, 2); }
inline void     wrU32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }
}  // namespace

// ---------------------------------------------------------------------------------------------
void BleNav::begin(NavModel* m) {
  m_ = m; g_self = this;
  q_ = xQueueCreate(BLE_RXQ_DEPTH, sizeof(RxMsg));

  NimBLEDevice::init(DEVICE_NAME);
  NimBLEDevice::setMTU(BLE_MTU);
#if BLE_REQUIRE_PAIRING
  NimBLEDevice::setSecurityAuth(true, true, true);             // bonding, MITM, LE Secure Connections
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);      // we show the passkey on the TFT
#endif

  NimBLEServer* srv = NimBLEDevice::createServer();
  srv->setCallbacks(new ServerCb());
#if TAHVIA_NIMBLE_V2
  srv->advertiseOnDisconnect(true);
#endif

  NimBLEService* svc = srv->createService(BLE_SERVICE_UUID);
  uint32_t rxProps = NIMBLE_PROPERTY::WRITE_NR;
#if BLE_REQUIRE_PAIRING
  rxProps |= NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::WRITE_AUTHEN;   // writing triggers pairing
#endif
  NimBLECharacteristic* rx = svc->createCharacteristic(BLE_RX_UUID, rxProps);
  rx->setCallbacks(new RxCb());
  g_tx = svc->createCharacteristic(BLE_TX_UUID, NIMBLE_PROPERTY::NOTIFY);

  NimBLEService* bat = srv->createService("180F");
  g_batt = bat->createCharacteristic("2A19", NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  uint8_t lvl = 100; g_batt->setValue(&lvl, 1);

#if TAHVIA_NIMBLE_V2
  srv->start();
#else
  svc->start();                                              // 1.x: services are started individually
  bat->start();
#endif

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(BLE_SERVICE_UUID);
#if TAHVIA_NIMBLE_V2
  adv->setName(DEVICE_NAME);
  adv->enableScanResponse(true);
#else
  adv->setScanResponse(true);                                // name comes from NimBLEDevice::init()
#endif
  adv->start();
}

void BleNav::setBatteryLevel(uint8_t pct) {
  if (!g_batt) return;
  g_batt->setValue(&pct, 1);
  if (connected_) g_batt->notify();
}

// ---- callbacks (NimBLE task) -----------------------------------------------------------------
void BleNav::_onConnect()    { connected_ = true; if (q_) xQueueReset(q_); }
void BleNav::_onDisconnect() { connected_ = false; pairing_ = false; evtDisc_ = true; if (q_) xQueueReset(q_); }
uint32_t BleNav::_onPasskey() { passkey_ = esp_random() % 1000000UL; pairing_ = true; return passkey_; }
void BleNav::_onAuthDone()   { pairing_ = false; }
void BleNav::_onRx(const uint8_t* d, size_t n) {
  if (!q_ || n == 0 || n > BLE_MAX_MSG) { if (m_) m_->badMsgs++; return; }
  RxMsg msg; msg.len = (uint8_t)n; memcpy(msg.data, d, n);
  if (xQueueSend(q_, &msg, 0) != pdTRUE && m_) m_->droppedMsgs++;   // route gaps recover via ACK
}

// ---- loop() side -----------------------------------------------------------------------------
void BleNav::poll(uint32_t now) {
  if (!q_) return;
  if (evtDisc_) { evtDisc_ = false; m_->resetSession(); }
  RxMsg msg;
  while (xQueueReceive(q_, &msg, 0) == pdTRUE) handle(msg.data, msg.len, now);

  // The phone only learns about free slots from ACKs, and trimming frees them silently:
  // report when at least 64 more slots became free (checked once per second).
  RouteRing* a = m_->active;
  if (connected_ && a->valid() && now - lastTopUpMs_ >= 1000) {
    lastTopUpMs_ = now;
    if (a->freeSlots() >= lastFree_ + 64) ackFor(*a, ACK_OK);
  }
}

void BleNav::send(const uint8_t* d, size_t n) {
  if (connected_ && g_tx) g_tx->notify(d, n);
}

void BleNav::sendHelloAck() {
  uint8_t b[10];
  b[0] = HELLO_ACK; b[1] = PROTO_VERSION;
  wrU16(b + 2, FW_VERSION); wrU16(b + 4, SCREEN_W); wrU16(b + 6, SCREEN_H); wrU16(b + 8, RouteRing::CAP);
  send(b, sizeof(b));
}

void BleNav::sendAck(uint8_t id, uint8_t status, uint32_t expected, uint16_t freeSlots) {
  uint8_t b[9];
  b[0] = ROUTE_ACK; b[1] = id; b[2] = status;
  wrU32(b + 3, expected); wrU16(b + 7, freeSlots);
  send(b, sizeof(b));
  lastFree_ = freeSlots;
}

// Messages shorter than expected are ignored and counted; extra trailing bytes are tolerated
// (new fields are only ever appended - README 6.8). Unknown types are ignored.
void BleNav::handle(const uint8_t* d, size_t n, uint32_t now) {
  NavModel& m = *m_;
  if (n < 1) return;
  switch (d[0]) {
    case HELLO:
      if (n < 2) { m.badMsgs++; break; }
      m.resetSession();
      sendHelloAck();                                   // phone checks proto and stops on mismatch
      break;

    case REBASE:
      if (n < 9) { m.badMsgs++; break; }
      {
        int32_t dx = rdI32(d + 1), dy = rdI32(d + 5);
        m.active->rebase(dx, dy); m.shadow->rebase(dx, dy); m.pose.rebase(dx, dy); m.routeRev++;
      }
      break;

    case POSE:
      if (n < 16) { m.badMsgs++; break; }
      m.pose.onPose(rdI32(d + 1), rdI32(d + 5), rdU16(d + 9), rdU16(d + 11), (d[15] & 1) != 0, now);
      m.distToTurn = rdU16(d + 13);
      break;

    case ROUTE_BEGIN:
      if (n < 14) { m.badMsgs++; break; }
      m.shadow->begin(d[1], rdU32(d + 2), rdI32(d + 6), rdI32(d + 10));
      chunksSinceAck_ = 0;
      ackFor(*m.shadow, ACK_OK);
      break;

    case ROUTE_CHUNK: {
      if (n < 7) { m.badMsgs++; break; }
      uint8_t id = d[1], cnt = d[6];
      if (n < (size_t)(7 + 4 * cnt)) { m.badMsgs++; break; }
      RouteRing* r = (m.shadow->valid() && m.shadow->id() == id) ? m.shadow
                   : (m.active->valid() && m.active->id() == id) ? m.active : nullptr;
      if (!r) { sendAck(id, ACK_UNKNOWN, 0, 0); break; }
      uint8_t st = r->append(rdU32(d + 2), d + 7, cnt);
      if (st == ACK_OK && r == m.active) m.routeRev++;
      if (st != ACK_OK || ++chunksSinceAck_ >= 4) { ackFor(*r, st); chunksSinceAck_ = 0; }
      break;
    }

    case ROUTE_COMMIT:
      if (n < 2) { m.badMsgs++; break; }
      if (m.shadow->valid() && m.shadow->id() == d[1]) { m.commitShadow(); ackFor(*m.active, ACK_OK); }
      else if (!(m.active->valid() && m.active->id() == d[1])) sendAck(d[1], ACK_UNKNOWN, 0, 0);
      break;

    case ROUTE_CLEAR:
      if (n < 2) { m.badMsgs++; break; }
      if (m.shadow->valid() && m.shadow->id() == d[1]) m.shadow->clear();
      if (m.active->valid() && m.active->id() == d[1]) { m.active->clear(); m.routeRev++; }
      break;

    case MANEUVER: {
      if (n < 2) { m.badMsgs++; break; }
      m.icon = d[1] <= 10 ? d[1] : 0;
      size_t k = 0;
      for (; k < 40 && 2 + k < n && d[2 + k]; k++) {
        uint8_t c = d[2 + k];
        m.street[k] = (c >= 32 && c < 127) ? (char)c : '?';   // device font is ASCII only
      }
      m.street[k] = 0;
      break;
    }

    case CONFIG:
      if (n < 4) { m.badMsgs++; break; }
      {
        uint16_t z = rdU16(d + 1);
        if (z >= 5 && z <= 2000) m.cfg.zoom = z;
        uint8_t f = d[3];
        m.cfg.headingUp = f & CFG_HEADING_UP;
        m.cfg.invert    = f & CFG_INVERT;
        m.cfg.showFps   = f & CFG_SHOW_FPS;
        m.cfg.imperial  = f & CFG_IMPERIAL;
      }
      break;

    case NAV_STATE:
      if (n < 2) { m.badMsgs++; break; }
      if (d[1] <= NAV_NO_GPS) m.navState = d[1];
      break;

    default: break;                                     // unknown type: ignore
  }
}