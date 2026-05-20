/**
 * Spider Farmer GGS → Cultivar relay chain firmware (v2.0.0).
 *
 * One codebase, three roles selected at compile time via -DROLE_*:
 *
 *   SHED    — reads BLE from AC5, sends telemetry via ESP-NOW
 *   GARAGE  — pure repeater, no config required
 *   HOUSE   — receives via ESP-NOW, uploads to Supabase over WiFi
 *
 * See platformio.ini for the three build environments and
 * src/role_config.h for the per-role flags.
 */

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "role_config.h"
#include "espnow_link.h"

#if HAS_WIFI
  #include <WiFi.h>
  #include <WiFiManager.h>
  #include <HTTPClient.h>
  #include <WiFiClientSecure.h>
#else
  #include <WiFi.h>   // still needed for WiFi.macAddress() etc.
#endif

#if HAS_BLE
  #include <NimBLEDevice.h>
  #include <WebServer.h>
  #include <DNSServer.h>
#endif

// ────────────────────────────────────────────────────────────────────
// Shared state
// ────────────────────────────────────────────────────────────────────
Preferences prefs;
String deviceId;

void blink(int times, int periodMs = 100) {
    for (int i = 0; i < times; i++) {
        digitalWrite(STATUS_LED_PIN, HIGH);
        delay(periodMs);
        digitalWrite(STATUS_LED_PIN, LOW);
        delay(periodMs);
    }
}

// LED status patterns. Three states, distinct visual fingerprints:
enum LedPattern : uint8_t {
    LED_SOLID            = 0,  // everything healthy — solid on
    LED_PARTIAL          = 1,  // working but missing optional link — 2-blink-pause
    LED_CRITICAL         = 2,  // critical link down — fast blink
};
//
// Patterns (non-blocking, driven by millis()):
//   SOLID:    LED always HIGH
//   PARTIAL:  blink-blink-pause, repeating  (period ~1.6s: on-off-on-off-pause)
//   CRITICAL: fast blink (period 200ms: on-off-on-off)
//
void updateStatusLed(LedPattern pattern) {
    uint32_t now = millis();
    bool on = false;
    if (pattern == LED_SOLID) {
        on = true;
    } else if (pattern == LED_CRITICAL) {
        // 5 Hz blink
        on = (now / 100) % 2 == 0;
    } else { // LED_PARTIAL
        // Two short blinks then a longer pause inside a 1600ms window:
        //  [0..150]on [150..300]off [300..450]on [450..600]off [600..1600]off
        uint32_t phase = now % 1600;
        on = (phase < 150) || (phase >= 300 && phase < 450);
    }
    digitalWrite(STATUS_LED_PIN, on ? HIGH : LOW);
}

// ════════════════════════════════════════════════════════════════════
// SHED ROLE — BLE collector
// ════════════════════════════════════════════════════════════════════
#if HAS_BLE

String bleAddress;
String roomLabel;

NimBLEClient* bleClient = nullptr;
NimBLERemoteCharacteristic* notifyChar = nullptr;
bool bleConnected = false;
unsigned long lastBleAttemptMs = 0;
unsigned long lastTelemetrySendMs = 0;
String jsonBuffer = "";
uint32_t telemetrySeq = 0;

struct Telemetry {
    float temp = NAN, humi = NAN, vpd = NAN;
    float soilMoisture = NAN, soilTemp = NAN, soilEc = NAN;
    int fanOn = -1, fanLevel = -1, lightOn = -1, lightLevel = -1;
    unsigned long capturedAt = 0;
    bool fresh = false;
};
Telemetry latest;

WebServer  webServer(80);
DNSServer  dnsServer;
const byte DNS_PORT = 53;

// JSON value extraction helpers (identical to v1)
String extractValueAfter(const String& json, const char* parentKey, const char* targetKey) {
    String parentNeedle = String("\"") + parentKey + "\":";
    int parentPos = json.indexOf(parentNeedle);
    if (parentPos == -1) return "";
    String targetNeedle = String("\"") + targetKey + "\":";
    int targetPos = json.indexOf(targetNeedle, parentPos);
    if (targetPos == -1) return "";
    if (targetPos - parentPos > 200) return "";
    int startVal = targetPos + targetNeedle.length();
    int endVal = startVal;
    while (endVal < (int)json.length()) {
        char c = json[endVal];
        if (c == ',' || c == '}' || c == ']') break;
        endVal++;
    }
    String result = json.substring(startVal, endVal);
    result.replace("\"", "");
    result.trim();
    return result;
}

String extractValueDirect(const String& json, const char* key) {
    String needle = String("\"") + key + "\":";
    int pos = json.indexOf(needle);
    if (pos == -1) return "";
    int startVal = pos + needle.length();
    int endVal = startVal;
    while (endVal < (int)json.length()) {
        char c = json[endVal];
        if (c == ',' || c == '}' || c == ']') break;
        endVal++;
    }
    String result = json.substring(startVal, endVal);
    result.replace("\"", "");
    result.trim();
    return result;
}

void notifyCallback(NimBLERemoteCharacteristic* c, uint8_t* data, size_t len, bool isNotify) {
    Serial.printf("[BLE NOTIFY] %d bytes\n", (int)len);
    for (size_t i = 0; i < len; i++) {
        char ch = (char)data[i];
        if (ch >= 32 && ch <= 126) jsonBuffer += ch;
    }
    bool hasJsonEnd = jsonBuffer.indexOf("}}") > 0 ||
                      (jsonBuffer.length() > 80 && jsonBuffer.endsWith("}"));
    if (hasJsonEnd) {
        Serial.println("──── BLE payload received ────");
        Serial.println(jsonBuffer);

        String t  = extractValueAfter(jsonBuffer, "sensor", "temp");
        String h  = extractValueAfter(jsonBuffer, "sensor", "humi");
        String v  = extractValueAfter(jsonBuffer, "sensor", "vpd");
        String sm = extractValueDirect(jsonBuffer, "humiSoil");
        String st = extractValueDirect(jsonBuffer, "tempSoil");
        String se = extractValueDirect(jsonBuffer, "ECSoil");
        String fl = extractValueAfter(jsonBuffer, "fan", "level");
        String fo = extractValueAfter(jsonBuffer, "fan", "on");
        String ll = extractValueAfter(jsonBuffer, "light", "level");
        String lo = extractValueAfter(jsonBuffer, "light", "on");

        if (t.length())  latest.temp = t.toFloat();
        if (h.length())  latest.humi = h.toFloat();
        if (v.length())  latest.vpd = v.toFloat();
        if (sm.length()) latest.soilMoisture = sm.toFloat();
        if (st.length()) latest.soilTemp = st.toFloat();
        if (se.length()) latest.soilEc = se.toFloat();
        if (fl.length()) latest.fanLevel = fl.toInt();
        if (fo.length()) latest.fanOn = fo.toInt();
        if (ll.length()) latest.lightLevel = ll.toInt();
        if (lo.length()) latest.lightOn = lo.toInt();
        latest.capturedAt = millis();
        latest.fresh = true;

        Serial.printf("Air:  T=%.1f RH=%.1f VPD=%.2f\n", latest.temp, latest.humi, latest.vpd);
        Serial.printf("Soil: VWC=%.1f T=%.1f EC=%.2f\n", latest.soilMoisture, latest.soilTemp, latest.soilEc);

        jsonBuffer = "";
        blink(1, 50);
    }
    if (jsonBuffer.length() > 2500) {
        Serial.println("[WARN] buffer overflow, dumping");
        jsonBuffer = "";
    }
}

// Scan all advertising BLE devices for ~5 sec and dump them to logs.
// Helps confirm what AC5 actually advertises as (name, MAC, RSSI).
void scanAllBle() {
    Serial.println("[BLE SCAN] starting 5s scan of all advertising devices...");
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setActiveScan(true);
    NimBLEScanResults results = scan->start(5, false);
    int n = results.getCount();
    Serial.printf("[BLE SCAN] %d devices found:\n", n);
    for (int i = 0; i < n; i++) {
        NimBLEAdvertisedDevice dev = results.getDevice(i);
        Serial.printf("[BLE SCAN]   %s  RSSI %d  \"%s\"\n",
                      dev.getAddress().toString().c_str(),
                      dev.getRSSI(),
                      dev.getName().c_str());
    }
    scan->clearResults();
    Serial.println("[BLE SCAN] done");
}

class BleClientCb : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient* c) override {
        Serial.println("[BLE] connected"); bleConnected = true;
    }
    void onDisconnect(NimBLEClient* c) override {
        Serial.println("[BLE] disconnected");
        bleConnected = false; notifyChar = nullptr;
    }
};

// Try connecting with a specific address type. Returns true on success.
bool connectBleWithType(uint8_t addrType, const char* typeName) {
    Serial.printf("[BLE] connecting to %s (type: %s)\n", bleAddress.c_str(), typeName);
    if (bleClient == nullptr) {
        bleClient = NimBLEDevice::createClient();
        bleClient->setClientCallbacks(new BleClientCb(), false);
    }
    bleClient->setConnectTimeout(8);  // 8 sec timeout instead of default 30
    if (!bleClient->connect(NimBLEAddress(bleAddress.c_str(), addrType))) {
        Serial.printf("[BLE] connect failed (type: %s)\n", typeName);
        return false;
    }
    delay(300);

    std::vector<NimBLERemoteService*>* services = bleClient->getServices(true);
    if (!services || services->empty()) {
        Serial.println("[BLE] no services found");
        bleClient->disconnect();
        return false;
    }

    NimBLERemoteCharacteristic* foundNotifyChar = nullptr;
    Serial.printf("[BLE] %d services discovered\n", (int)services->size());
    for (auto* svc : *services) {
        std::string svcStr = svc->getUUID().toString();
        std::vector<NimBLERemoteCharacteristic*>* chars = svc->getCharacteristics(true);
        if (!chars) continue;
        for (auto* chr : *chars) {
            bool canN = chr->canNotify();
            if (canN && !foundNotifyChar &&
                svcStr.find("1800") == std::string::npos &&
                svcStr.find("1801") == std::string::npos &&
                svcStr.find("180a") == std::string::npos &&
                svcStr.find("180f") == std::string::npos) {
                foundNotifyChar = chr;
            }
        }
    }
    if (!foundNotifyChar) {
        Serial.println("[BLE] no notify char in custom services");
        bleClient->disconnect();
        return false;
    }
    if (!foundNotifyChar->subscribe(true, notifyCallback)) {
        Serial.println("[BLE] subscribe failed");
        bleClient->disconnect();
        return false;
    }
    notifyChar = foundNotifyChar;
    Serial.printf("[BLE] subscribed (type: %s)\n", typeName);
    blink(3, 100);
    return true;
}

bool connectBle() {
    if (bleAddress.length() == 0) return false;
    bleAddress.toLowerCase();

    // Try PUBLIC first (most common for off-the-shelf BLE devices)
    if (connectBleWithType(BLE_ADDR_PUBLIC, "PUBLIC")) return true;
    delay(500);
    // Then try RANDOM (some devices use this)
    if (connectBleWithType(BLE_ADDR_RANDOM, "RANDOM")) return true;
    return false;
}

// Build the upstream telemetry payload (compact JSON, will be wrapped on HOUSE).
size_t buildTelemetryPayload(uint8_t* buf, size_t bufSize) {
    JsonDocument doc;
    doc["room"] = roomLabel;
    doc["bleConnected"] = bleConnected;
    doc["uptimeMs"] = millis();
    JsonObject t = doc["t"].to<JsonObject>();
    if (!isnan(latest.temp))         t["temp_c"]       = latest.temp;
    if (!isnan(latest.humi))         t["humidity_pct"] = latest.humi;
    if (!isnan(latest.vpd))          t["vpd_kpa"]      = latest.vpd;
    if (!isnan(latest.soilMoisture)) t["soil_vwc_pct"] = latest.soilMoisture;
    if (!isnan(latest.soilTemp))     t["soil_temp_c"]  = latest.soilTemp;
    if (!isnan(latest.soilEc))       t["soil_ec"]      = latest.soilEc;
    if (latest.fanOn >= 0)           t["fan_on"]       = latest.fanOn;
    if (latest.fanLevel >= 0)        t["fan_level"]    = latest.fanLevel;
    if (latest.lightOn >= 0)         t["light_on"]     = latest.lightOn;
    if (latest.lightLevel >= 0)      t["light_level"]  = latest.lightLevel;
    t["captured_ms_ago"] = millis() - latest.capturedAt;
    return serializeJson(doc, buf, bufSize);
}

const char SHED_PORTAL_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head><meta name=viewport content="width=device-width">
<title>SHED config</title>
<style>body{font-family:system-ui;max-width:480px;margin:24px auto;padding:0 16px;color:#1c1a16;background:#f6f3ec}
h1{font-weight:500;font-size:22px}label{display:block;margin:14px 0 4px;font-size:13px;color:#5a4f3a}
input{width:100%;padding:10px;border:1px solid #b8a98a;border-radius:6px;font-size:15px;background:#fff}
button{margin-top:20px;width:100%;padding:12px;background:#5a8a3a;color:#fff;border:0;border-radius:6px;font-size:16px}</style>
</head><body><h1>🌱 SHED config</h1>
<form method=POST action=/save>
<label>GGS BLE MAC <small>(blank = autoscan)</small></label>
<input name=mac value="%MAC%" placeholder="80:f1:b2:b9:07:7e">
<label>Room label</label>
<input name=room value="%ROOM%" placeholder="Flower Room">
<button>Save and reboot</button></form>
<p style="font-size:11px;color:#888;margin-top:30px">v%VER% · SHED · device %DEV%</p>
</body></html>
)HTML";

void handlePortalRoot() {
    String html = FPSTR(SHED_PORTAL_HTML);
    html.replace("%MAC%",  bleAddress);
    html.replace("%ROOM%", roomLabel);
    html.replace("%VER%",  FIRMWARE_VERSION);
    html.replace("%DEV%",  deviceId);
    webServer.send(200, "text/html", html);
}
void handlePortalSave() {
    bleAddress = webServer.arg("mac");
    roomLabel  = webServer.arg("room");
    bleAddress.trim(); roomLabel.trim();
    prefs.begin("ggs", false);
    prefs.putString("bleMac", bleAddress);
    prefs.putString("room",   roomLabel);
    prefs.end();
    webServer.send(200, "text/html", "<p>Saved. Rebooting...</p>");
    delay(1500);
    ESP.restart();
}
void startConfigPortal() {
    char ap[32];
    uint64_t chipId = ESP.getEfuseMac();
    snprintf(ap, sizeof(ap), "GGS-Shed-%04X", (uint16_t)(chipId & 0xFFFF));
    WiFi.mode(WIFI_AP_STA);  // AP for config + STA for ESP-NOW
    WiFi.softAP(ap, "cultivar");
    Serial.printf("[CFG] AP=%s pw=cultivar (open browser to 192.168.4.1)\n", ap);
    dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
    webServer.on("/",     handlePortalRoot);
    webServer.on("/save", HTTP_POST, handlePortalSave);
    webServer.onNotFound(handlePortalRoot);
    webServer.begin();
}

void loadShedConfig() {
    prefs.begin("ggs", true);
    bleAddress = prefs.getString("bleMac", "");
    roomLabel  = prefs.getString("room", "Flower Room");
    prefs.end();
}

#endif // HAS_BLE  (SHED-only block ends)

// ════════════════════════════════════════════════════════════════════
// HOUSE ROLE — WiFi + Supabase uploader
// ════════════════════════════════════════════════════════════════════
#if HAS_WIFI

String supabaseUrl;
String supabaseAnonKey;
String roomLabelHouseFallback;
WiFiManager wm;
WiFiManagerParameter* p_supaUrl = nullptr;
WiFiManagerParameter* p_supaKey = nullptr;
WiFiManagerParameter* p_room    = nullptr;
unsigned long lastPostMs = 0;
unsigned long lastHeartbeatMs = 0;

struct ReceivedPacket {
    bool     fresh = false;
    uint32_t seq = 0;
    uint8_t  hops = 0;
    uint8_t  originMac[6] = {0};
    String   payloadJson;
};
ReceivedPacket lastRx;

void onDataFromMesh(const EspNowPacket* pkt) {
    Serial.printf("[ESPNOW] data seq=%u hops=%u len=%u\n",
                  pkt->seq, pkt->hops + 1, pkt->payloadLen);
    lastRx.fresh = true;
    lastRx.seq = pkt->seq;
    lastRx.hops = pkt->hops + 1;
    memcpy(lastRx.originMac, pkt->originMac, 6);
    lastRx.payloadJson = String((const char*)pkt->payload).substring(0, pkt->payloadLen);
    blink(1, 50);
}

bool postToSupabase(bool heartbeatOnly) {
    if (WiFi.status() != WL_CONNECTED) return false;
    if (supabaseUrl.length() == 0 || supabaseAnonKey.length() == 0) return false;
    String endpoint = supabaseUrl + "/functions/v1/spiderfarmer-sync";

    JsonDocument doc;
    doc["deviceId"] = deviceId;
    doc["firmwareVersion"] = FIRMWARE_VERSION;
    doc["uptimeMs"] = millis();
    doc["heartbeat"] = heartbeatOnly;
    doc["chainRole"] = "HOUSE";

    if (!heartbeatOnly && lastRx.fresh) {
        JsonDocument inner;
        DeserializationError err = deserializeJson(inner, lastRx.payloadJson);
        if (err) {
            Serial.printf("[HTTP] bad inner JSON: %s\n", err.c_str());
        } else {
            doc["roomLabel"] = inner["room"].as<String>();
            doc["bleConnected"] = inner["bleConnected"].as<bool>();
            doc["sourceUptimeMs"] = inner["uptimeMs"].as<unsigned long>();
            doc["telemetry"] = inner["t"];
            char originId[24];
            snprintf(originId, sizeof(originId), "ggs-bridge-%02x%02x%02x%02x%02x%02x",
                     lastRx.originMac[0], lastRx.originMac[1], lastRx.originMac[2],
                     lastRx.originMac[3], lastRx.originMac[4], lastRx.originMac[5]);
            doc["originDeviceId"] = originId;
            doc["hops"] = lastRx.hops;
            doc["seq"]  = lastRx.seq;
        }
    } else {
        doc["roomLabel"] = roomLabelHouseFallback;
        doc["bleConnected"] = false;
        doc["downstreamHealthy"] = EspNowLink::isDownstreamHealthy();
    }

    String body;
    serializeJson(doc, body);

    WiFiClientSecure client; client.setInsecure();
    HTTPClient http;
    http.begin(client, endpoint);
    http.setTimeout(10000);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("apikey", supabaseAnonKey);
    http.addHeader("Authorization", "Bearer " + supabaseAnonKey);
    int code = http.POST(body);
    String resp = http.getString();
    http.end();

    Serial.printf("[HTTP] POST %d - %s\n", code, resp.c_str());
    if (code >= 200 && code < 300) {
        if (!heartbeatOnly) lastRx.fresh = false;
        blink(2, 50);
        return true;
    }
    return false;
}

void saveHouseConfig() {
    prefs.begin("ggs", false);
    prefs.putString("supaUrl", p_supaUrl->getValue());
    prefs.putString("supaKey", p_supaKey->getValue());
    prefs.putString("room",    p_room->getValue());
    prefs.end();
}

void loadHouseConfig() {
    prefs.begin("ggs", true);
    supabaseUrl     = prefs.getString("supaUrl", "");
    supabaseAnonKey = prefs.getString("supaKey", "");
    roomLabelHouseFallback = prefs.getString("room", "Flower Room");
    prefs.end();
}

void startHousePortal() {
    char apSsid[32];
    uint64_t chipId = ESP.getEfuseMac();
    snprintf(apSsid, sizeof(apSsid), "GGS-House-%04X", (uint16_t)(chipId & 0xFFFF));
    p_supaUrl = new WiFiManagerParameter("supaUrl", "Supabase URL", supabaseUrl.c_str(), 80);
    p_supaKey = new WiFiManagerParameter("supaKey", "Supabase anon key", supabaseAnonKey.c_str(), 256);
    p_room    = new WiFiManagerParameter("room",    "Fallback room label", roomLabelHouseFallback.c_str(), 40);
    wm.addParameter(p_supaUrl);
    wm.addParameter(p_supaKey);
    wm.addParameter(p_room);
    wm.setSaveConfigCallback(saveHouseConfig);
    wm.setConfigPortalTimeout(300);
    if (!wm.autoConnect(apSsid, "cultivar")) {
        Serial.println("[WM] timed out, rebooting");
        delay(1000); ESP.restart();
    }
    loadHouseConfig();
    Serial.printf("[WM] connected, IP=%s channel=%d\n",
                  WiFi.localIP().toString().c_str(), WiFi.channel());
}

#endif // HAS_WIFI

// ════════════════════════════════════════════════════════════════════
// SETUP / LOOP — dispatches per role
// ════════════════════════════════════════════════════════════════════

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println();
    Serial.println("═══════════════════════════════════════════");
    Serial.printf( "  Spider Farmer GGS → Cultivar Bridge\n");
    Serial.printf( "  Role: %s    Firmware v%s\n", ROLE_NAME, FIRMWARE_VERSION);
    Serial.println("═══════════════════════════════════════════");

    pinMode(STATUS_LED_PIN, OUTPUT);
    blink(2, 200);

    uint64_t chipId = ESP.getEfuseMac();
    char buf[24];
    snprintf(buf, sizeof(buf), "ggs-bridge-%012llx", chipId);
    deviceId = buf;

    #if HAS_BLE   // ── SHED ────────────────────────────────────────────
        loadShedConfig();
        Serial.printf("  BLE MAC: %s\n", bleAddress.length() ? bleAddress.c_str() : "(autoscan)");
        Serial.printf("  Room:    %s\n", roomLabel.c_str());

        startConfigPortal();           // brings up AP+STA WiFi mode
        EspNowLink::begin();           // uses STA interface for ESP-NOW

        NimBLEDevice::init("");
        NimBLEDevice::setMTU(517);
        NimBLEDevice::setPower(ESP_PWR_LVL_P9);

        // Initial diagnostic scan: dump every BLE device in range so we
        // can see exactly what AC5 (and the other devices like Govee,
        // AC Infinity, Vivosun) actually advertise as.
        scanAllBle();
    #endif

    #if IS_RELAY  // ── GARAGE ──────────────────────────────────────────
        EspNowLink::begin();
    #endif

    #if HAS_WIFI  // ── HOUSE ───────────────────────────────────────────
        loadHouseConfig();
        startHousePortal();
        EspNowLink::begin();
        EspNowLink::onDataPacket(onDataFromMesh);
    #endif
}

void loop() {
    EspNowLink::loop();

    // ── LED status ──────────────────────────────────────────────────
    // SHED:    BLE down       → CRITICAL (fast blink)
    //          BLE up, no upstream → PARTIAL (2-blink-pause)
    //          BLE up + upstream   → SOLID
    // GARAGE:  no HOUSE       → CRITICAL (fast blink)
    //          HOUSE up, no SHED  → PARTIAL (2-blink-pause)
    //          HOUSE up + SHED up → SOLID
    // HOUSE:   no WiFi        → CRITICAL (fast blink)
    //          WiFi up, no GARAGE → PARTIAL (2-blink-pause)
    //          WiFi up + GARAGE   → SOLID
    #if defined(ROLE_SHED)
        if (!bleConnected) updateStatusLed(LED_CRITICAL);
        else if (!EspNowLink::isUpstreamHealthy()) updateStatusLed(LED_PARTIAL);
        else updateStatusLed(LED_SOLID);
    #elif defined(ROLE_GARAGE)
        if (!EspNowLink::isUpstreamHealthy()) updateStatusLed(LED_CRITICAL);
        else if (!EspNowLink::isDownstreamHealthy()) updateStatusLed(LED_PARTIAL);
        else updateStatusLed(LED_SOLID);
    #elif defined(ROLE_HOUSE)
        if (WiFi.status() != WL_CONNECTED) updateStatusLed(LED_CRITICAL);
        else if (!EspNowLink::isDownstreamHealthy()) updateStatusLed(LED_PARTIAL);
        else updateStatusLed(LED_SOLID);
    #endif

    // ── Role-specific work ──────────────────────────────────────────
    #if HAS_BLE
        dnsServer.processNextRequest();
        webServer.handleClient();

        if (!bleConnected && millis() - lastBleAttemptMs > BLE_RECONNECT_MS) {
            lastBleAttemptMs = millis();
            connectBle();
        }
        // Periodic re-scan: every 5 minutes if BLE not connected, do a
        // fresh scan to see what's actually advertising. Helps diagnose
        // if AC5 changed MAC, went to sleep, or the network shifted.
        {
            static unsigned long lastScanMs = 0;
            if (!bleConnected && millis() - lastScanMs > 300000UL) {
                lastScanMs = millis();
                scanAllBle();
            }
        }
        if (latest.fresh && millis() - lastTelemetrySendMs > 5000) {
            uint8_t buf[ESPNOW_MAX_PAYLOAD];
            size_t  n = buildTelemetryPayload(buf, sizeof(buf));
            if (n > 0 && EspNowLink::sendUpstream(buf, n, ++telemetrySeq)) {
                latest.fresh = false;
                lastTelemetrySendMs = millis();
                Serial.printf("[SEND] seq=%u %u bytes upstream\n", telemetrySeq, (unsigned)n);
            }
        }
    #endif

    #if HAS_WIFI
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[WiFi] disconnected, retrying");
            WiFi.reconnect();
            delay(2000);
            return;
        }
        if (lastRx.fresh && millis() - lastPostMs > POST_INTERVAL_MS) {
            lastPostMs = millis();
            postToSupabase(false);
        } else if (millis() - lastHeartbeatMs > HEARTBEAT_MS) {
            lastHeartbeatMs = millis();
            postToSupabase(true);
        }
    #endif

    delay(20);
}
