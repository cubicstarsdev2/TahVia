// TahviaNav BLE protocol v1 - message ids and enums (see README section 6).
#pragma once
#include <stdint.h>

#define PROTO_VERSION 1
#define FW_VERSION    0x0100          // 1.0

namespace proto {
enum Msg : uint8_t {
  HELLO = 0x01, REBASE = 0x03, POSE = 0x10,
  ROUTE_BEGIN = 0x20, ROUTE_CHUNK = 0x21, ROUTE_COMMIT = 0x22, ROUTE_CLEAR = 0x23,
  MANEUVER = 0x30, CONFIG = 0x40, NAV_STATE = 0x50,
  HELLO_ACK = 0x81, ROUTE_ACK = 0x82,
  // ---- OTA firmware update (see OTA_PROTOCOL.md) ----
  OTA_BEGIN = 0x60, OTA_CHUNK = 0x61, OTA_END = 0x62, OTA_ABORT = 0x63,   // phone -> device
  OTA_ACK = 0x83                                                           // device -> phone
};
enum OtaState : uint8_t { OTA_IDLE = 0, OTA_RECEIVING = 1, OTA_VERIFYING = 2, OTA_DONE = 3, OTA_ERROR = 4 };
enum OtaStatus : uint8_t {
  OTA_OK = 0, OTA_E_STATE = 1, OTA_E_SIZE = 2, OTA_E_GAP = 3, OTA_E_FLASH = 4, OTA_E_MD5 = 5,
  OTA_E_BATTERY = 6, OTA_E_TIMEOUT = 7, OTA_E_ARGS = 8, OTA_E_ABORTED = 9, OTA_E_NOT_SECURE = 10, OTA_E_IMAGE = 11
};
enum Ack : uint8_t { ACK_OK = 0, ACK_GAP = 1, ACK_FULL = 2, ACK_UNKNOWN = 3 };
enum NavStateId : uint8_t { NAV_IDLE = 0, NAV_NAVIGATING = 1, NAV_REROUTING = 2, NAV_ARRIVED = 3, NAV_NO_GPS = 4 };
enum CfgFlag : uint8_t { CFG_HEADING_UP = 1, CFG_INVERT = 2, CFG_SHOW_FPS = 4, CFG_IMPERIAL = 8 };
}  // namespace proto