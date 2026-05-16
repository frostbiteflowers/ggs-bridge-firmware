/**
 * ESP-NOW link layer for the GGS bridge relay chain.
 *
 * Handles:
 *   - Discovery / pairing via broadcast (no MAC hardcoding needed)
 *   - WiFi-channel synchronization (HOUSE announces, downstream locks in)
 *   - Bidirectional packet relay (data goes upstream, ACKs/channel come downstream)
 *
 * Public API is intentionally tiny: init(), loop(), sendUpstream(), and a
 * callback for received data packets.
 */

#pragma once

#include <Arduino.h>
#include <esp_now.h>

// Largest payload we ever send (the full JSON telemetry packet from SHED).
// ESP-NOW raw limit is 250 bytes — we stay well under.
#define ESPNOW_MAX_PAYLOAD 230

// Packet types
enum EspNowPacketType : uint8_t {
    PKT_BEACON   = 0x01,  // "I exist, I am ROLE, on channel N"
    PKT_DATA     = 0x02,  // telemetry payload (JSON string)
    PKT_HEARTBEAT = 0x03, // empty keepalive
};

// Wire format. Fixed-size header (16 bytes) + variable payload.
struct __attribute__((packed)) EspNowPacket {
    uint8_t  type;           // EspNowPacketType
    uint8_t  srcRole;        // ROLE_POSITION of sender (0/1/2)
    uint8_t  channel;        // WiFi channel HOUSE is on (0 if unknown)
    uint8_t  hops;           // increments at each relay
    uint32_t seq;            // monotonic from origin
    uint8_t  originMac[6];   // MAC of the SHED (the actual sensor source)
    uint16_t payloadLen;     // bytes in payload that follow
    uint8_t  payload[ESPNOW_MAX_PAYLOAD];
};

// Called when a data packet (PKT_DATA) reaches a role that should consume it.
// Only HOUSE (the uploader) registers a meaningful callback. SHED/GARAGE
// pass packets through transparently.
typedef void (*DataPacketHandler)(const EspNowPacket* pkt);

namespace EspNowLink {
    // Initialize ESP-NOW. Must be called after WiFi.mode() has been set
    // (HOUSE: STA mode after WiFi connect; SHED/GARAGE: STA mode w/o connect).
    bool begin();

    // Pump the state machine. Call from loop().
    void loop();

    // Send a data payload upstream (toward HOUSE). On SHED this is how
    // telemetry leaves; on GARAGE this is how relayed packets continue.
    // Returns false if no upstream peer is known yet.
    bool sendUpstream(const uint8_t* payload, uint16_t len, uint32_t seq);

    // Register the data callback. Only HOUSE uses this.
    void onDataPacket(DataPacketHandler cb);

    // Status queries — used by the LED blink logic and HOUSE's heartbeat.
    bool   isUpstreamHealthy();
    bool   isDownstreamHealthy();
    uint32_t lastUpstreamSeenMs();
    uint32_t lastDownstreamSeenMs();
    uint8_t  getCurrentChannel();
    void     getOwnMac(uint8_t out[6]);

    // HOUSE only: call after WiFi connects to announce channel downstream.
    void announceChannel(uint8_t channel);
}
