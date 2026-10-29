#pragma once
// ---- Identity / BLE ---------------------------------------------------------
#define DEVICE_NAME          "TahVia"
// Custom 128-bit UUIDs. The phone app must use the same ones.
#define BLE_SERVICE_UUID     "7a8f0001-3c5e-4b1a-9d27-5e1f4a6b7c80"
#define BLE_RX_UUID          "7a8f0002-3c5e-4b1a-9d27-5e1f4a6b7c80"   // phone -> device (write no response)
#define BLE_TX_UUID          "7a8f0003-3c5e-4b1a-9d27-5e1f4a6b7c80"   // device -> phone (notify)
#define BLE_MTU              247
#define BLE_MAX_MSG          244
#define BLE_RXQ_DEPTH        32
// 1 = LE Secure Connections + bonding, passkey shown on the TFT. 0 = open link (bench testing).
#ifndef BLE_REQUIRE_PAIRING
#define BLE_REQUIRE_PAIRING  1
#endif

// ---- Screen -----------------------------------------------------------------
#define SCREEN_W             480
#define SCREEN_H             320

// ---- Timing -----------------------------------------------------------------
#define FRAME_US_NAV         (1000000UL / 60)
#define FRAME_US_IDLE        (1000000UL / 60)
#define POSE_TIMEOUT_MS      3000
#define BOOT_BAR_RATE        80.0f     // percent per second (bar animation speed)