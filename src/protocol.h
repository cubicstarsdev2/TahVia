// TahviaNav BLE protocol v1 - message ids and enums (see README section 6).
#pragma once
#include <stdint.h>

#define PROTO_VERSION 1

// ---- Firmware version --------------------------------------------------------------------------------
// >>> To release a new firmware, edit ONLY this string ("major.minor.patch") and put the same
// >>> "## x.y.z" line at the top of README.md (the phone reads the README to decide whether to update).
// No build script needed: the string is parsed at compile time. Limits: major 0-255, minor 0-15, patch 0-15.
// Can still be overridden from the build flags:  -DFW_VERSION_STRING=\"1.2.3\"
#ifndef FW_VERSION_STRING
#define FW_VERSION_STRING "1.1.0"
#endif

// Wire format (HELLO_ACK / OTA_BEGIN, u16): (major << 8) | (minor << 4) | patch.  1.0.0 == 0x0100.
// (C++11 constexpr: single-expression recursive functions only.)
namespace fwv {
constexpr bool isDigit(char c) { return c >= '0' && c <= '9'; }
constexpr int  num(const char* s, int i, int acc = 0) { return isDigit(s[i]) ? num(s, i + 1, acc * 10 + (s[i] - '0')) : acc; }
constexpr int  skipDigits(const char* s, int i) { return isDigit(s[i]) ? skipDigits(s, i + 1) : i; }
constexpr int  nextPart(const char* s, int i) { return s[i] == '.' ? i + 1 : (s[i] == 0 ? i : nextPart(s, i + 1)); }
constexpr int  major(const char* s) { return num(s, 0); }
constexpr int  minor(const char* s) { return num(s, nextPart(s, 0)); }
constexpr int  patch(const char* s) { return num(s, nextPart(s, nextPart(s, 0))); }
// exactly "<digits>.<digits>.<digits>"
constexpr int  dotAfterDigits(const char* s, int i) { return (isDigit(s[i]) && s[skipDigits(s, i)] == '.') ? skipDigits(s, i) + 1 : -1; }
constexpr bool tail(const char* s, int i)  { return isDigit(s[i]) && s[skipDigits(s, i)] == 0; }
constexpr bool part3(const char* s, int i) { return i >= 0 && tail(s, i); }
constexpr bool part2(const char* s, int i) { return i >= 0 && part3(s, dotAfterDigits(s, i)); }
constexpr bool valid(const char* s)        { return part2(s, dotAfterDigits(s, 0)); }
}  // namespace fwv

static_assert(fwv::valid(FW_VERSION_STRING), "FW_VERSION_STRING must look like \"1.2.3\"");
#define FW_VERSION_MAJOR (fwv::major(FW_VERSION_STRING))
#define FW_VERSION_MINOR (fwv::minor(FW_VERSION_STRING))
#define FW_VERSION_PATCH (fwv::patch(FW_VERSION_STRING))
static_assert(FW_VERSION_MAJOR >= 0 && FW_VERSION_MAJOR <= 255 &&
              FW_VERSION_MINOR >= 0 && FW_VERSION_MINOR <= 15 &&
              FW_VERSION_PATCH >= 0 && FW_VERSION_PATCH <= 15,
              "Version must fit the u16 code: major 0-255, minor 0-15, patch 0-15");
#define FW_VERSION    ((FW_VERSION_MAJOR << 8) | (FW_VERSION_MINOR << 4) | FW_VERSION_PATCH)
// Split a version code back into its parts (for display).
#define FW_V_MAJOR(v) (((v) >> 8) & 0xFF)
#define FW_V_MINOR(v) (((v) >> 4) & 0x0F)
#define FW_V_PATCH(v) ((v) & 0x0F)

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