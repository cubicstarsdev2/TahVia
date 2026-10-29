// OTA firmware update over BLE (wire format: OTA_PROTOCOL.md).
//
// The phone downloads firmware.bin (GitHub Pages), then streams it here in OTA_CHUNK messages.
// Chunks are written straight into the next OTA partition through the Arduino `Update` library.
// The running firmware is never touched: the boot partition only switches when OTA_END succeeds
// (size + MD5 verified). Any failure / disconnect / timeout leaves the old firmware in place.
//
// Everything here runs from loop() (ble.poll() dispatches messages), never from the BLE task.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "config.h"
#include "protocol.h"

// ---- Tunables (override in config.h or with -D flags) -------------------------------------------
#ifndef OTA_WINDOW
#define OTA_WINDOW           16      // max unacked chunks the phone may have in flight (keep <= BLE_RXQ_DEPTH / 2)
#endif
#ifndef OTA_ACK_EVERY
#define OTA_ACK_EVERY        8       // device sends OTA_ACK every N good chunks
#endif
#ifndef OTA_IDLE_TIMEOUT_MS
#define OTA_IDLE_TIMEOUT_MS  15000   // no chunk for this long -> abort (TIMEOUT)
#endif
#ifndef OTA_MIN_BATT_PCT
#define OTA_MIN_BATT_PCT     30      // refuse to start below this (ignored if battery is unknown)
#endif
#ifndef OTA_ERROR_SHOW_MS
#define OTA_ERROR_SHOW_MS    4000    // how long the "Update failed" screen stays up
#endif
#ifndef OTA_REBOOT_DELAY_MS
#define OTA_REBOOT_DELAY_MS  1500    // after a good OTA_END: time to flush the ACK + show "complete"
#endif

#define OTA_CHUNK_MAX_DATA   (BLE_MAX_MSG - 5)   // type(1) + offset(4) + data  ==  244 -> 239 data bytes

namespace ota {

typedef void (*SendFn)(const uint8_t* msg, size_t len);   // notify `msg` on the TX characteristic

struct Info {
  uint8_t  state   = proto::OTA_IDLE;   // proto::OtaState
  uint8_t  err     = proto::OTA_OK;     // proto::OtaStatus (valid when state == OTA_ERROR)
  uint32_t done    = 0;                 // bytes written
  uint32_t total   = 0;                 // image size
  uint16_t version = 0;                 // version code being installed (FW_VERSION format)
};

void setSender(SendFn fn);              // ble_nav calls this once in begin()

// `payload` = bytes after the message-type byte. `secure` = link is encrypted/bonded.
// Returns true if `type` is an OTA message (handled here).
bool onMessage(uint8_t type, const uint8_t* payload, size_t len, bool secure);
void onDisconnect();                    // call from the BLE disconnect callback (loop context)

void update(uint32_t nowMs);            // call every frame: timeouts, error screen timer, deferred reboot

bool        active();                   // true from OTA_BEGIN until the error screen times out / reboot
const Info& info();
const char* errorText(uint8_t status);  // short text for the update screen

}  // namespace ota