/**
 * ESP-NOW link implementation.
 *
 * Architecture
 * ============
 * Each ESP knows its own ROLE_POSITION (0=SHED, 1=GARAGE, 2=HOUSE).
 * On boot, every node:
 *   1) Starts broadcasting BEACON packets every 2s announcing
 *      "I am role X, on channel Y" to the ESP-NOW broadcast MAC.
 *   2) Listens for BEACONs from other roles, builds a tiny routing table.
 *   3) Stops broadcasting beacons (or slows them) once paired up + down.
 *
 * A node's "upstream" is the role at position+1 (toward HOUSE).
 * A node's "downstream" is the role at position-1 (toward SHED).
 * HOUSE has no upstream; SHED has no downstream.
 *
 * Channel sync
 * ============
 * ESP-NOW requires sender + receiver on the same WiFi channel. HOUSE is
 * locked to whatever channel Orbi puts it on. HOUSE broadcasts that channel
 * in every BEACON. GARAGE locks to it, then broadcasts the same channel
 * downstream. SHED locks to it. Whole chain syncs in <10 sec on cold boot.
 *
 * If HOUSE's channel changes (Orbi reboots, picks a new channel), GARAGE
 * notices the channel field in the next BEACON, re-locks, propagates.
 */

#include "espnow_link.h"
#include "role_config.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <string.h>

static const uint8_t BROADCAST_MAC[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

// Known peers (we only ever care about the role directly above and below us).
static uint8_t  upstreamMac[6]   = {0};   // role at position+1
static uint8_t  downstreamMac[6] = {0};   // role at position-1
static bool     upstreamKnown    = false;
static bool     downstreamKnown  = false;
static uint32_t lastUpstreamRxMs   = 0;
static uint32_t lastDownstreamRxMs = 0;
static uint32_t lastBeaconTxMs     = 0;
static uint8_t  currentChannel     = 0;   // 0 = unknown / not locked yet
static uint32_t txSeq              = 0;

static DataPacketHandler dataHandler = nullptr;

// Forward decls
static void onEspNowRecv(const uint8_t* mac, const uint8_t* data, int len);
static void onEspNowSent(const uint8_t* mac, esp_now_send_status_t status);
static void sendBeacon();
static void registerPeer(const uint8_t mac[6], uint8_t channel);
static void unregisterPeer(const uint8_t mac[6]);
static bool isBroadcast(const uint8_t mac[6]);
static void macCopy(uint8_t dst[6], const uint8_t src[6]) { memcpy(dst, src, 6); }
static bool macEq(const uint8_t a[6], const uint8_t b[6]) { return memcmp(a, b, 6) == 0; }
static void macToStr(const uint8_t mac[6], char* out /* >=18 bytes */) {
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
}

namespace EspNowLink {

bool begin() {
    // The radio must be in station mode (off) for ESP-NOW to coexist with WiFi.
    // For HOUSE, WiFi.begin() has already been called and we adopt its channel.
    // For SHED and GARAGE, we set STA mode and lock the radio to channel 1
    // initially. We'll channel-hop in loop() until we hear HOUSE.
    #if HAS_WIFI
        // HOUSE: WiFi is already up, channel is locked by Orbi
        currentChannel = WiFi.channel();
    #else
        WiFi.mode(WIFI_STA);
        WiFi.disconnect(false, true);
        currentChannel = 1;  // start on channel 1, will hop if HOUSE not found
        esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
    #endif

    if (esp_now_init() != ESP_OK) {
        Serial.println("[ESPNOW] init failed");
        return false;
    }

    esp_now_register_recv_cb(onEspNowRecv);
    esp_now_register_send_cb(onEspNowSent);

    // Always have the broadcast address registered so we can send/recv beacons.
    registerPeer(BROADCAST_MAC, currentChannel);

    uint8_t myMac[6];
    WiFi.macAddress(myMac);
    char macStr[18];
    macToStr(myMac, macStr);
    Serial.printf("[ESPNOW] up, role=%s mac=%s channel=%u\n",
                  ROLE_NAME, macStr, currentChannel);
    return true;
}

void loop() {
    uint32_t now = millis();

    // Broadcast a beacon every 2s while we still need to find peers.
    bool stillSearching = false;
    #if ROLE_POSITION > 0
        if (!downstreamKnown) stillSearching = true;
    #endif
    #if ROLE_POSITION < 2
        if (!upstreamKnown)   stillSearching = true;
    #endif

    // Channel hopping: SHED and GARAGE don't know HOUSE's channel yet.
    // While unpaired upstream, cycle through channels 1, 6, 11 (the three
    // main 2.4 GHz US channels) every 3 seconds to find HOUSE's broadcast.
    // Once paired, we stop hopping and stay on HOUSE's channel.
    #if !HAS_WIFI
    {
        static uint32_t lastHopMs = 0;
        static const uint8_t HOP_CHANNELS[] = {1, 6, 11};
        static uint8_t hopIdx = 0;
        if (!upstreamKnown && now - lastHopMs > 3000) {
            lastHopMs = now;
            hopIdx = (hopIdx + 1) % (sizeof(HOP_CHANNELS) / sizeof(HOP_CHANNELS[0]));
            uint8_t newCh = HOP_CHANNELS[hopIdx];
            if (newCh != currentChannel) {
                Serial.printf("[ESPNOW] scanning channel %u\n", newCh);
                currentChannel = newCh;
                esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
                // Re-register the broadcast peer on the new channel
                unregisterPeer(BROADCAST_MAC);
                registerPeer(BROADCAST_MAC, currentChannel);
                if (upstreamKnown) {
                    unregisterPeer(upstreamMac);
                    registerPeer(upstreamMac, currentChannel);
                }
                if (downstreamKnown) {
                    unregisterPeer(downstreamMac);
                    registerPeer(downstreamMac, currentChannel);
                }
            }
        }
    }
    #endif

    // Even after pairing, beacon occasionally so peers can re-find us if
    // they reboot. Every 10s once stable, every 2s while searching.
    uint32_t interval = stillSearching ? ESPNOW_DISCOVERY_MS : 10000;
    if (now - lastBeaconTxMs > interval) {
        lastBeaconTxMs = now;
        sendBeacon();
    }
}

bool sendUpstream(const uint8_t* payload, uint16_t len, uint32_t seq) {
    #if ROLE_POSITION == 2
        // HOUSE has no upstream — caller shouldn't have called this.
        return false;
    #else
        if (!upstreamKnown) return false;
        if (len > ESPNOW_MAX_PAYLOAD) {
            Serial.printf("[ESPNOW] payload too big (%u > %u)\n",
                          len, (unsigned)ESPNOW_MAX_PAYLOAD);
            return false;
        }
        EspNowPacket pkt = {};
        pkt.type       = PKT_DATA;
        pkt.srcRole    = ROLE_POSITION;
        pkt.channel    = currentChannel;
        pkt.hops       = 0;  // origin
        pkt.seq        = seq;
        WiFi.macAddress(pkt.originMac);
        pkt.payloadLen = len;
        memcpy(pkt.payload, payload, len);

        size_t wireLen = sizeof(pkt) - ESPNOW_MAX_PAYLOAD + len;
        esp_err_t err = esp_now_send(upstreamMac, (const uint8_t*)&pkt, wireLen);
        if (err != ESP_OK) {
            Serial.printf("[ESPNOW] send failed err=%d\n", err);
            return false;
        }
        return true;
    #endif
}

void onDataPacket(DataPacketHandler cb) { dataHandler = cb; }

bool isUpstreamHealthy() {
    #if ROLE_POSITION == 2
        return true;   // HOUSE is upstream-of-itself; trivially healthy
    #else
        return upstreamKnown &&
               (millis() - lastUpstreamRxMs < ESPNOW_LINK_TIMEOUT_MS);
    #endif
}

bool isDownstreamHealthy() {
    #if ROLE_POSITION == 0
        return true;   // SHED has no downstream
    #else
        return downstreamKnown &&
               (millis() - lastDownstreamRxMs < ESPNOW_LINK_TIMEOUT_MS);
    #endif
}

uint32_t lastUpstreamSeenMs()   { return lastUpstreamRxMs; }
uint32_t lastDownstreamSeenMs() { return lastDownstreamRxMs; }
uint8_t  getCurrentChannel()    { return currentChannel; }
void     getOwnMac(uint8_t out[6]) { WiFi.macAddress(out); }

void announceChannel(uint8_t channel) {
    if (channel == currentChannel) return;
    Serial.printf("[ESPNOW] channel %u → %u\n", currentChannel, channel);
    currentChannel = channel;
    // Actually move the radio to this channel (only meaningful for non-WiFi roles).
    #if !HAS_WIFI
        esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
    #endif
    // Re-register broadcast on new channel
    unregisterPeer(BROADCAST_MAC);
    registerPeer(BROADCAST_MAC, currentChannel);
    // Re-register known peers on new channel
    if (upstreamKnown)   { unregisterPeer(upstreamMac);   registerPeer(upstreamMac,   currentChannel); }
    if (downstreamKnown) { unregisterPeer(downstreamMac); registerPeer(downstreamMac, currentChannel); }
    sendBeacon();
}

}  // namespace

// ─── static helpers ────────────────────────────────────────────────────

static void sendBeacon() {
    EspNowPacket pkt = {};
    pkt.type    = PKT_BEACON;
    pkt.srcRole = ROLE_POSITION;
    pkt.channel = currentChannel;
    pkt.payloadLen = 0;
    WiFi.macAddress(pkt.originMac);
    size_t wireLen = sizeof(pkt) - ESPNOW_MAX_PAYLOAD;
    esp_now_send(BROADCAST_MAC, (const uint8_t*)&pkt, wireLen);
}

static void registerPeer(const uint8_t mac[6], uint8_t channel) {
    if (esp_now_is_peer_exist(mac)) return;
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = channel;
    peer.encrypt = false;
    peer.ifidx   = WIFI_IF_STA;
    esp_err_t err = esp_now_add_peer(&peer);
    if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST) {
        Serial.printf("[ESPNOW] add_peer failed err=%d\n", err);
    }
}

static void unregisterPeer(const uint8_t mac[6]) {
    if (isBroadcast(mac)) {
        // Always keep broadcast registered, we'll re-add at new channel
        esp_now_del_peer(mac);
        return;
    }
    esp_now_del_peer(mac);
}

static bool isBroadcast(const uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) if (mac[i] != 0xFF) return false;
    return true;
}

static void onEspNowRecv(const uint8_t* mac, const uint8_t* data, int len) {
    if (len < (int)(sizeof(EspNowPacket) - ESPNOW_MAX_PAYLOAD)) return;
    const EspNowPacket* pkt = (const EspNowPacket*)data;

    // --- BEACON: peer discovery + channel sync -----------------------------
    if (pkt->type == PKT_BEACON) {
        // Adopt channel from peers closer to HOUSE (higher position).
        // SHED listens to GARAGE+HOUSE. GARAGE listens to HOUSE.
        if (pkt->srcRole > ROLE_POSITION && pkt->channel != 0 && pkt->channel != currentChannel) {
            EspNowLink::announceChannel(pkt->channel);
        }

        // Is this our upstream peer? (position = ours + 1)
        if (pkt->srcRole == ROLE_POSITION + 1) {
            if (!upstreamKnown || !macEq(upstreamMac, mac)) {
                macCopy(upstreamMac, mac);
                registerPeer(upstreamMac, currentChannel);
                upstreamKnown = true;
                char s[18]; macToStr(mac, s);
                Serial.printf("[ESPNOW] upstream paired: %s (%s)\n",
                              s, pkt->srcRole == 1 ? "GARAGE" :
                                 pkt->srcRole == 2 ? "HOUSE" : "?");
            }
            lastUpstreamRxMs = millis();
        }
        // Is this our downstream peer? (position = ours - 1)
        else if (ROLE_POSITION > 0 && pkt->srcRole == ROLE_POSITION - 1) {
            if (!downstreamKnown || !macEq(downstreamMac, mac)) {
                macCopy(downstreamMac, mac);
                registerPeer(downstreamMac, currentChannel);
                downstreamKnown = true;
                char s[18]; macToStr(mac, s);
                Serial.printf("[ESPNOW] downstream paired: %s (%s)\n",
                              s, pkt->srcRole == 0 ? "SHED" :
                                 pkt->srcRole == 1 ? "GARAGE" : "?");
            }
            lastDownstreamRxMs = millis();
        }
        return;
    }

    // --- DATA: relay upstream, or deliver if we're HOUSE -------------------
    if (pkt->type == PKT_DATA) {
        // Coming from downstream — refresh that link's timestamp
        lastDownstreamRxMs = millis();

        #if IS_UPLOADER
            // HOUSE: deliver to the upload callback
            if (dataHandler) dataHandler(pkt);
        #else
            // SHED can't receive DATA (no downstream). GARAGE relays it.
            #if IS_RELAY
                if (!upstreamKnown) return;
                EspNowPacket fwd = *pkt;
                fwd.hops = pkt->hops + 1;
                fwd.channel = currentChannel;
                size_t wireLen = sizeof(fwd) - ESPNOW_MAX_PAYLOAD + fwd.payloadLen;
                esp_now_send(upstreamMac, (const uint8_t*)&fwd, wireLen);
            #endif
        #endif
        return;
    }
}

static void onEspNowSent(const uint8_t* mac, esp_now_send_status_t status) {
    // Useful for debugging but spammy; comment in only when needed.
    // Serial.printf("[ESPNOW] tx %s\n",
    //               status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
}
