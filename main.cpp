/**
 * Spider Farmer GGS AC5 → Cultivar Bridge
 * ──────────────────────────────────────────────────────────────────
 * Reads telemetry from a Spider Farmer GGS controller over BLE,
 * pushes it to Cultivar via a Supabase edge function over HTTPS.
 *
 * Replaces the MQTT path in cr0ssn0tice/Spider-Farmer-GGS-Controller-MQTT
 * with direct HTTPS-to-Supabase, plus a captive portal for on-device
 * WiFi / endpoint configuration (no firmware recompile needed).
 *
 * BLE protocol credit: cr0ssn0tice (GPL-compatible reverse-engineering)
 * Service: 0000ff00-0000-1000-8000-00805f9b34fb
 * Notify:  0000ff01-0000-1000-8000-00805f9b34fb  (controller → us)
 * Write:   0000ff02-0000-1000-8000-00805f9b34fb  (us → controller, unused for v1)
 *
 * Telemetry payload (plain JSON, may be fragmented across BLE packets,
 * may have garbage bytes mixed in):
 *   {
 *     "method": "getDevSta",
 *     "code": 200,
 *     "data": {
 *       "sensor": { "temp": 23.3, "humi": 37.7, "vpd": 1.78 },
 *       "fan":    { "on": 1, "level": 5 },
 *       "light":  { "level": 26 }
 *     }
 *   }
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <NimBLEDevice.h>

// ═══════════════════════════════════════════════════════════════════
// CONFIG (compile-time defaults; overridable via captive portal)
// ═══════════════════════════════════════════════════════════════════

#define FIRMWARE_VERSION   "1.0.0"
#define AP_NAME_PREFIX     "GGS-Bridge-"
#define AP_PASSWORD        "cultivar"           // captive portal password
#define CONFIG_PORTAL_TIMEOUT_SEC 300           // 5 min then reboot & retry
#define STATUS_LED_PIN     2                    // most ESP32 dev boards
#define POST_INTERVAL_MS   30000                // 30s between Supabase POSTs
#define HEARTBEAT_MS       300000               // 5min status post even w/o BLE data
#define BLE_SCAN_TIMEOUT_SEC 10
#define BLE_RECONNECT_MS   15000

// BLE service UUIDs - shared across the Spider Farmer GGS family
// (Controller, AC5 Power Strip, AC10) per cr0ssn0tice's reverse-engineering
static const NimBLEUUID SVC_UUID("0000ff00-0000-1000-8000-00805f9b34fb");
static const NimBLEUUID NOTIFY_UUID("0000ff01-0000-1000-8000-00805f9b34fb");
static const NimBLEUUID WRITE_UUID("0000ff02-0000-1000-8000-00805f9b34fb");

// ═══════════════════════════════════════════════════════════════════
// PERSISTENT STATE (NVS-backed via Preferences)
// ═══════════════════════════════════════════════════════════════════
Preferences prefs;
String supabaseUrl;      // e.g. https://vzdbutqgdceqfmhwehdt.supabase.co
String supabaseAnonKey;  // anon key for edge function auth
String bleAddress;       // GGS controller MAC (lowercase, colons)
String roomLabel;        // "Flower Room" / "Veg Room" for Cultivar tagging
String deviceId;         // unique ID for this bridge

// ═══════════════════════════════════════════════════════════════════
// RUNTIME STATE
// ═══════════════════════════════════════════════════════════════════
NimBLEClient* bleClient = nullptr;
NimBLERemoteCharacteristic* notifyChar = nullptr;
bool bleConnected = false;
unsigned long lastBleAttemptMs = 0;
unsigned long lastPostMs = 0;
unsigned long lastHeartbeatMs = 0;
String jsonBuffer = "";

// Last-known telemetry (populated by notifyCallback, drained by POST)
struct Telemetry {
  // Air sensor (confirmed from cr0ssn0tice's repo)
  float temp = NAN;          // air °C
  float humi = NAN;          // air %RH
  float vpd = NAN;           // kPa
  // Soil sensor (3-in-1 Soil Sensor Pro)
  // ⚠ Key names are best-guess until we sniff a real payload tomorrow.
  //   The raw-dump mode (see below) will let us verify and adjust on the fly.
  float soilMoisture = NAN;  // VWC %
  float soilTemp = NAN;      // soil °C
  float soilEc = NAN;        // μS/cm (or mS/cm — confirm units from raw dump)
  // Outlet state
  int fanOn = -1;
  int fanLevel = -1;
  int lightOn = -1;
  int lightLevel = -1;
  unsigned long capturedAt = 0;
  bool fresh = false;
};
Telemetry latest;

// Raw-payload dump mode: when enabled (toggled via captive portal),
// the bridge POSTs the full JSON buffer to Supabase as a debug record.
// Use this to learn exact key names for the soil sensor on first boot.
bool rawDumpMode = false;
String lastRawPayload = "";
bool lastRawPayloadFresh = false;

// ═══════════════════════════════════════════════════════════════════
// HELPERS
// ═══════════════════════════════════════════════════════════════════

void blink(int times, int periodMs = 100) {
  for (int i = 0; i < times; i++) {
    digitalWrite(STATUS_LED_PIN, HIGH);
    delay(periodMs);
    digitalWrite(STATUS_LED_PIN, LOW);
    delay(periodMs);
  }
}

// Extract a numeric value from a noisy JSON-ish buffer. Same approach as
// cr0ssn0tice's parser: don't parse the whole thing (it's fragmented +
// has binary garbage), just search for "parentKey":...."targetKey":VALUE
String extractValueAfter(const String& json, const char* parentKey, const char* targetKey) {
  String parentNeedle = String("\"") + parentKey + "\":";
  int parentPos = json.indexOf(parentNeedle);
  if (parentPos == -1) return "";

  String targetNeedle = String("\"") + targetKey + "\":";
  int targetPos = json.indexOf(targetNeedle, parentPos);
  if (targetPos == -1) return "";
  if (targetPos - parentPos > 200) return "";  // sanity: scoped to this parent

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

// ═══════════════════════════════════════════════════════════════════
// BLE: notification callback (telemetry arrives here)
// ═══════════════════════════════════════════════════════════════════

void notifyCallback(NimBLERemoteCharacteristic* c, uint8_t* data, size_t len, bool isNotify) {
  // Stream printable ASCII into the buffer, discard everything else.
  // BLE notifications fragment a single JSON message across many packets.
  for (size_t i = 0; i < len; i++) {
    char ch = (char)data[i];
    if (ch >= 32 && ch <= 126) {
      jsonBuffer += ch;
    }
  }

  // Trigger when we see "fan" key + closing braces (heuristic from cr0ssn0tice).
  // The full payload always contains "fan" and ends with "}}".
  if (jsonBuffer.indexOf("fan\"") > 0 && jsonBuffer.indexOf("}}") > 0) {
    Serial.println("\n──── BLE payload received ────");
    Serial.println(jsonBuffer);

    // Capture for raw-dump mode (debugging soil sensor keys, etc)
    lastRawPayload = jsonBuffer;
    lastRawPayloadFresh = true;

    // ── Air sensor (confirmed keys) ──
    String t = extractValueAfter(jsonBuffer, "sensor", "temp");
    String h = extractValueAfter(jsonBuffer, "sensor", "humi");
    String v = extractValueAfter(jsonBuffer, "sensor", "vpd");

    // ── Soil sensor (best-guess keys — see raw dump to verify) ──
    // Try common Spider Farmer / IoT naming conventions in priority order.
    // First non-empty hit wins. Confirm and prune these after first dump.
    String sm = extractValueAfter(jsonBuffer, "soil", "moisture");
    if (sm.length() == 0) sm = extractValueAfter(jsonBuffer, "soil", "vwc");
    if (sm.length() == 0) sm = extractValueAfter(jsonBuffer, "soil", "humi");
    if (sm.length() == 0) sm = extractValueAfter(jsonBuffer, "soilSensor", "moisture");

    String st = extractValueAfter(jsonBuffer, "soil", "temp");
    if (st.length() == 0) st = extractValueAfter(jsonBuffer, "soilSensor", "temp");

    String se = extractValueAfter(jsonBuffer, "soil", "ec");
    if (se.length() == 0) se = extractValueAfter(jsonBuffer, "soil", "conductivity");
    if (se.length() == 0) se = extractValueAfter(jsonBuffer, "soilSensor", "ec");

    // ── Outlets ──
    String fl = extractValueAfter(jsonBuffer, "fan", "level");
    String fo = extractValueAfter(jsonBuffer, "fan", "on");
    String ll = extractValueAfter(jsonBuffer, "light", "level");
    String lo = extractValueAfter(jsonBuffer, "light", "on");

    if (t.length()) latest.temp = t.toFloat();
    if (h.length()) latest.humi = h.toFloat();
    if (v.length()) latest.vpd = v.toFloat();
    if (sm.length()) latest.soilMoisture = sm.toFloat();
    if (st.length()) latest.soilTemp = st.toFloat();
    if (se.length()) latest.soilEc = se.toFloat();
    if (fl.length()) latest.fanLevel = fl.toInt();
    if (fo.length()) latest.fanOn = fo.toInt();
    if (ll.length()) latest.lightLevel = ll.toInt();
    if (lo.length()) latest.lightOn = lo.toInt();

    latest.capturedAt = millis();
    latest.fresh = true;

    Serial.printf("Air:  T=%.1f°C  RH=%.1f%%  VPD=%.2fkPa\n",
                  latest.temp, latest.humi, latest.vpd);
    Serial.printf("Soil: VWC=%.1f%%  T=%.1f°C  EC=%.1f\n",
                  latest.soilMoisture, latest.soilTemp, latest.soilEc);
    Serial.printf("Fan:  on=%d  level=%d   Light: on=%d  level=%d\n",
                  latest.fanOn, latest.fanLevel, latest.lightOn, latest.lightLevel);

    if (!sm.length() && !st.length() && !se.length()) {
      Serial.println("[!] No soil fields parsed - enable rawDump to see actual keys");
    }

    jsonBuffer = "";
    blink(1, 50);
  }

  // Safety: prevent buffer runaway if "}}" never appears
  if (jsonBuffer.length() > 2500) {
    Serial.println("[WARN] BLE buffer overflow, dumping");
    jsonBuffer = "";
  }
}

// ═══════════════════════════════════════════════════════════════════
// BLE: connection lifecycle
// ═══════════════════════════════════════════════════════════════════

class BleClientCb : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient* c) override {
    Serial.println("[BLE] connected, requesting MTU 517");
    c->setMTU(517);
    bleConnected = true;
  }
  void onDisconnect(NimBLEClient* c, int reason) override {
    Serial.printf("[BLE] disconnected (reason=%d)\n", reason);
    bleConnected = false;
    notifyChar = nullptr;
  }
};

bool connectBle() {
  if (bleAddress.length() == 0) {
    Serial.println("[BLE] no MAC address configured, skipping");
    return false;
  }

  // Lowercase the MAC (NimBLE is picky)
  bleAddress.toLowerCase();

  Serial.printf("[BLE] connecting to %s\n", bleAddress.c_str());

  if (bleClient == nullptr) {
    bleClient = NimBLEDevice::createClient();
    bleClient->setClientCallbacks(new BleClientCb(), false);
  }

  if (!bleClient->connect(NimBLEAddress(bleAddress.c_str(), BLE_ADDR_PUBLIC))) {
    Serial.println("[BLE] connect failed (try BLE_ADDR_RANDOM if persistent)");
    return false;
  }

  delay(150);

  // Walk the services to find our notification characteristic
  NimBLERemoteService* svc = bleClient->getService(SVC_UUID);
  if (svc == nullptr) {
    Serial.println("[BLE] FF00 service not found - is this a GGS device?");
    bleClient->disconnect();
    return false;
  }

  notifyChar = svc->getCharacteristic(NOTIFY_UUID);
  if (notifyChar == nullptr) {
    Serial.println("[BLE] FF01 notify characteristic not found");
    bleClient->disconnect();
    return false;
  }

  if (!notifyChar->canNotify()) {
    Serial.println("[BLE] FF01 doesn't support notify, aborting");
    bleClient->disconnect();
    return false;
  }

  if (!notifyChar->subscribe(true, notifyCallback)) {
    Serial.println("[BLE] subscribe failed");
    bleClient->disconnect();
    return false;
  }

  Serial.println("[BLE] subscribed, telemetry should flow shortly");
  blink(3, 100);
  return true;
}

// Optional: scan for any "SF-GGS-*" device if no MAC is configured.
// User can leave the MAC field blank in the captive portal and we'll
// auto-discover - but only if there's just one GGS device in range.
String scanForGgs() {
  Serial.println("[BLE] scanning for SF-GGS-* devices...");
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);
  NimBLEScanResults results = scan->getResults(BLE_SCAN_TIMEOUT_SEC * 1000, false);

  String found = "";
  int matches = 0;
  for (int i = 0; i < results.getCount(); i++) {
    const NimBLEAdvertisedDevice* dev = results.getDevice(i);
    String name = dev->getName().c_str();
    if (name.startsWith("SF-GGS")) {
      Serial.printf("  Found: %s @ %s\n", name.c_str(), dev->getAddress().toString().c_str());
      found = dev->getAddress().toString().c_str();
      matches++;
    }
  }
  scan->clearResults();

  if (matches == 0) {
    Serial.println("[BLE] no GGS devices found");
    return "";
  }
  if (matches > 1) {
    Serial.println("[BLE] multiple GGS devices found - please set MAC explicitly");
    return "";
  }
  return found;
}

// ═══════════════════════════════════════════════════════════════════
// SUPABASE: POST telemetry to edge function
// ═══════════════════════════════════════════════════════════════════

bool postTelemetry(bool heartbeatOnly) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[HTTP] WiFi not connected, skipping POST");
    return false;
  }
  if (supabaseUrl.length() == 0 || supabaseAnonKey.length() == 0) {
    Serial.println("[HTTP] Supabase config missing, skipping POST");
    return false;
  }

  String endpoint = supabaseUrl + "/functions/v1/spiderfarmer-sync";

  JsonDocument doc;
  doc["deviceId"] = deviceId;
  doc["firmwareVersion"] = FIRMWARE_VERSION;
  doc["roomLabel"] = roomLabel;
  doc["bleConnected"] = bleConnected;
  doc["uptimeMs"] = millis();
  doc["heartbeat"] = heartbeatOnly;

  if (!heartbeatOnly && !isnan(latest.temp)) {
    JsonObject t = doc["telemetry"].to<JsonObject>();
    // Air
    if (!isnan(latest.temp)) t["temp_c"] = latest.temp;
    if (!isnan(latest.humi)) t["humidity_pct"] = latest.humi;
    if (!isnan(latest.vpd)) t["vpd_kpa"] = latest.vpd;
    // Soil
    if (!isnan(latest.soilMoisture)) t["soil_vwc_pct"] = latest.soilMoisture;
    if (!isnan(latest.soilTemp))     t["soil_temp_c"] = latest.soilTemp;
    if (!isnan(latest.soilEc))       t["soil_ec"]      = latest.soilEc;  // units TBD
    // Outlets
    if (latest.fanOn >= 0) t["fan_on"] = latest.fanOn;
    if (latest.fanLevel >= 0) t["fan_level"] = latest.fanLevel;
    if (latest.lightOn >= 0) t["light_on"] = latest.lightOn;
    if (latest.lightLevel >= 0) t["light_level"] = latest.lightLevel;
    t["captured_ms_ago"] = millis() - latest.capturedAt;
  }

  // Raw-dump mode: ship the full unparsed JSON so we can verify soil-sensor keys.
  // Toggled via captive portal. Disable once parser is confirmed.
  if (rawDumpMode && lastRawPayloadFresh) {
    doc["rawPayload"] = lastRawPayload;
    lastRawPayloadFresh = false;
  }

  String body;
  serializeJson(doc, body);

  WiFiClientSecure client;
  client.setInsecure();  // skip cert pinning for v1; Supabase URL is HTTPS already
                         // TODO: pin Supabase root cert in v2 for proper TLS

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
    latest.fresh = false;
    blink(2, 50);
    return true;
  }
  return false;
}

// ═══════════════════════════════════════════════════════════════════
// WIFI: captive portal setup
// ═══════════════════════════════════════════════════════════════════

WiFiManager wm;
WiFiManagerParameter* p_supaUrl;
WiFiManagerParameter* p_supaKey;
WiFiManagerParameter* p_bleMac;
WiFiManagerParameter* p_room;
WiFiManagerParameter* p_rawDump;

void saveConfigCallback() {
  Serial.println("[CFG] saving captive portal config to NVS");
  prefs.begin("ggs", false);
  prefs.putString("supaUrl", p_supaUrl->getValue());
  prefs.putString("supaKey", p_supaKey->getValue());
  prefs.putString("bleMac", p_bleMac->getValue());
  prefs.putString("room", p_room->getValue());
  prefs.putBool("rawDump", String(p_rawDump->getValue()) == "1");
  prefs.end();
}

void loadConfig() {
  prefs.begin("ggs", true);
  supabaseUrl = prefs.getString("supaUrl", "");
  supabaseAnonKey = prefs.getString("supaKey", "");
  bleAddress = prefs.getString("bleMac", "");
  roomLabel = prefs.getString("room", "Flower Room");
  rawDumpMode = prefs.getBool("rawDump", true);  // ON by default for v1 - turn off after first verified payload
  prefs.end();

  // Derive a stable device ID from the MAC
  uint64_t chipId = ESP.getEfuseMac();
  char buf[24];
  snprintf(buf, sizeof(buf), "ggs-bridge-%012llx", chipId);
  deviceId = buf;

  Serial.println("──── Config loaded ────");
  Serial.printf("  Device ID:      %s\n", deviceId.c_str());
  Serial.printf("  Supabase URL:   %s\n", supabaseUrl.length() ? supabaseUrl.c_str() : "(unset)");
  Serial.printf("  Supabase key:   %s\n", supabaseAnonKey.length() ? "***set***" : "(unset)");
  Serial.printf("  BLE MAC:        %s\n", bleAddress.length() ? bleAddress.c_str() : "(autoscan)");
  Serial.printf("  Room label:     %s\n", roomLabel.c_str());
}

void startCaptivePortal() {
  // AP SSID: GGS-Bridge-XXXX where XXXX is last 4 hex of MAC
  char apSsid[32];
  uint64_t chipId = ESP.getEfuseMac();
  snprintf(apSsid, sizeof(apSsid), "%s%04X", AP_NAME_PREFIX, (uint16_t)(chipId & 0xFFFF));
  Serial.printf("[WM] starting AP: %s (pw: %s)\n", apSsid, AP_PASSWORD);

  p_supaUrl = new WiFiManagerParameter("supaUrl", "Supabase URL (https://...)",
                                       supabaseUrl.c_str(), 80);
  p_supaKey = new WiFiManagerParameter("supaKey", "Supabase anon key",
                                       supabaseAnonKey.c_str(), 256);
  p_bleMac  = new WiFiManagerParameter("bleMac",  "GGS BLE MAC (blank = autoscan)",
                                       bleAddress.c_str(), 18);
  p_room    = new WiFiManagerParameter("room",    "Room label (Flower Room / Veg Room)",
                                       roomLabel.c_str(), 40);
  p_rawDump = new WiFiManagerParameter("rawDump", "Send raw BLE payload to Supabase for debugging (1/0)",
                                       rawDumpMode ? "1" : "0", 2);

  wm.addParameter(p_supaUrl);
  wm.addParameter(p_supaKey);
  wm.addParameter(p_bleMac);
  wm.addParameter(p_room);
  wm.addParameter(p_rawDump);
  wm.setSaveConfigCallback(saveConfigCallback);
  wm.setConfigPortalTimeout(CONFIG_PORTAL_TIMEOUT_SEC);

  // Auto-connect attempts saved creds first, falls back to portal
  if (!wm.autoConnect(apSsid, AP_PASSWORD)) {
    Serial.println("[WM] timed out, rebooting");
    delay(1000);
    ESP.restart();
  }

  // Refresh in-memory config in case the portal saved new values
  loadConfig();

  Serial.printf("[WM] connected, IP=%s\n", WiFi.localIP().toString().c_str());
}

// ═══════════════════════════════════════════════════════════════════
// SETUP & LOOP
// ═══════════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("═══════════════════════════════════════════");
  Serial.println("  Spider Farmer GGS → Cultivar Bridge");
  Serial.printf("  Firmware v%s\n", FIRMWARE_VERSION);
  Serial.println("═══════════════════════════════════════════");

  pinMode(STATUS_LED_PIN, OUTPUT);
  blink(2, 200);

  loadConfig();
  startCaptivePortal();

  NimBLEDevice::init("");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  // If no MAC configured, autoscan once at boot
  if (bleAddress.length() == 0) {
    String found = scanForGgs();
    if (found.length()) {
      bleAddress = found;
      prefs.begin("ggs", false);
      prefs.putString("bleMac", bleAddress);
      prefs.end();
      Serial.printf("[BLE] autoscan locked in %s\n", bleAddress.c_str());
    }
  }
}

void loop() {
  // Keep WiFi alive (WiFiManager handles reconnect internally)
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] disconnected, retrying...");
    WiFi.reconnect();
    delay(2000);
    return;
  }

  // Maintain BLE link
  if (!bleConnected && millis() - lastBleAttemptMs > BLE_RECONNECT_MS) {
    lastBleAttemptMs = millis();
    connectBle();
  }

  // Push telemetry to Supabase
  unsigned long now = millis();
  if (latest.fresh && now - lastPostMs > POST_INTERVAL_MS) {
    lastPostMs = now;
    postTelemetry(false);
  } else if (now - lastHeartbeatMs > HEARTBEAT_MS) {
    lastHeartbeatMs = now;
    postTelemetry(true);  // heartbeat: tells Cultivar the bridge is alive
  }

  delay(50);
}
