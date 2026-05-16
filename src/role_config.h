/**
 * Per-role compile-time configuration.
 *
 * Exactly one of ROLE_SHED, ROLE_GARAGE, ROLE_HOUSE must be defined
 * (set by platformio.ini build_flags per environment).
 *
 * This header gives each role a human-readable name, a NUL-terminated
 * tag used in ESP-NOW handshake packets, and a position in the chain.
 */

#pragma once

#if defined(ROLE_SHED) + defined(ROLE_GARAGE) + defined(ROLE_HOUSE) != 1
  #error "Exactly one of ROLE_SHED, ROLE_GARAGE, ROLE_HOUSE must be defined"
#endif

// Chain positions: SHED=0 (origin) → GARAGE=1 → HOUSE=2 (uploader).
// Higher position = closer to the internet. Packets flow upstream
// (position+1) toward HOUSE.

#if defined(ROLE_SHED)
  #define ROLE_NAME    "SHED"
  #define ROLE_TAG     "SHED"
  #define ROLE_POSITION 0
  #define HAS_BLE      1
  #define HAS_WIFI     0
  #define IS_RELAY     0
  #define IS_UPLOADER  0
#elif defined(ROLE_GARAGE)
  #define ROLE_NAME    "GARAGE"
  #define ROLE_TAG     "GRGE"
  #define ROLE_POSITION 1
  #define HAS_BLE      0
  #define HAS_WIFI     0
  #define IS_RELAY     1
  #define IS_UPLOADER  0
#elif defined(ROLE_HOUSE)
  #define ROLE_NAME    "HOUSE"
  #define ROLE_TAG     "HOUS"
  #define ROLE_POSITION 2
  #define HAS_BLE      0
  #define HAS_WIFI     1
  #define IS_RELAY     0
  #define IS_UPLOADER  1
#endif

// Firmware version is shared across all three — same git commit.
#define FIRMWARE_VERSION "2.0.2"

// Status LED — same pin all roles
#define STATUS_LED_PIN 2

// Timing constants
#define POST_INTERVAL_MS        30000UL   // HOUSE only: rate-limit Supabase POSTs
#define HEARTBEAT_MS           300000UL   // HOUSE: heartbeat row every 5 min
#define BLE_RECONNECT_MS        15000UL   // SHED only
#define ESPNOW_DISCOVERY_MS      2000UL   // Broadcast "I exist" every 2s during pairing
#define ESPNOW_LINK_TIMEOUT_MS  60000UL   // Consider link dead after 60s silence
