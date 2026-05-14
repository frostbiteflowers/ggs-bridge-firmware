/**
 * Spider Farmer GGS AC5 → Cultivar Bridge
 * Reads telemetry over BLE, pushes to Supabase via HTTPS.
 * BLE protocol credit: cr0ssn0tice
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <NimBLEDevice.h>

#define FIRMWARE_VERSION   "1.0.1"
#define AP_NAME_PREFIX     "GGS-Bridge-"
#define AP_PASSWORD        "cultivar"
#define CONFIG_PORTAL_TIMEOUT_SEC 300
#define STATUS_LED_PIN     2
#define POST_INTERVAL_MS   30000
#define HEARTBEAT_MS       300000
#define BLE_SCAN_TIMEOUT_SEC 10
#define BLE_RECONNECT_MS   15000

Preferences prefs;
String supabaseUrl;
String supabaseAnonKey;
String bleAddress;
String roomLabel;
String deviceId;

NimBLEClient* bleClient = nullptr;
NimBLERemoteCharacteristic* notifyChar = nullptr;
bool bleConnected = false;
unsigned long lastBleAttemptMs = 0;
unsigned long lastPostMs = 0;
unsigned long lastHeartbeatMs = 0;
String jsonBuffer = "";

struct Telemetry {
  float temp = NAN;
  float humi = NAN;
  float vpd = NAN;
  float soilMoisture = NAN;
  float soilTemp = NAN;
  float soilEc = NAN;
  int fanOn = -1;
  int fanLevel = -1;
  int lightOn = -1;
  int lightLevel = -1;
  unsigned long capturedAt = 0;
  bool fresh = false;
};
Telemetry latest;

bool rawDumpMode = false;
String lastRawPayload = "";
bool lastRawPayloadFresh = false;

void blink(int times, int periodMs = 100) {
  for (int i = 0; i < times; i++) {
    digitalWrite(STATUS_LED_PIN, HIGH);
    delay(periodMs);
    digitalWrite(STATUS_LED_PIN, LOW);
    delay(periodMs);
  }
}

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

void notifyCallback(NimBLERemoteCharacteristic* c, uint8_t* data, size_t len, bool isNotify) {
  // Log raw bytes for debugging unknown protocols
  Serial.printf("[BLE NOTIFY] %d bytes: ", (int)len);
  for (size_t i = 0; i < len && i < 64; i++) {
    Serial.printf("%02x ", data[i]);
  }
  Serial.println();
  Serial.print("[BLE NOTIFY ascii]: ");
  for (size_t i = 0; i < len; i++) {
    char ch = (char)data[i];
    Serial.print((ch >= 32 && ch <= 126) ? ch : '.');
  }
  Serial.println();

  // Stream printable ASCII into the buffer
  for (size_t i = 0; i < len; i++) {
    char ch = (char)data[i];
    if (ch >= 32 && ch <= 126) {
      jsonBuffer += ch;
    }
  }

  // Trigger when we see closing braces (relaxed - works for any JSON-like payload)
  bool hasJsonEnd = jsonBuffer.indexOf("}}") > 0 ||
                    (jsonBuffer.length() > 80 && jsonBuffer.endsWith("}"));

  if (hasJsonEnd) {
    Serial.println("\n──── BLE payload received ────");
    Serial.println(jsonBuffer);

    lastRawPayload = jsonBuffer;
    lastRawPayloadFresh = true;

    String t = extractValueAfter(jsonBuffer, "sensor", "temp");
    String h = extractValueAfter(jsonBuffer, "sensor", "humi");
    String v = extractValueAfter(jsonBuffer, "sensor", "vpd");

    String sm = extractValueAfter(jsonBuffer, "soil", "moisture");
    if (sm.length() == 0) sm = extractValueAfter(jsonBuffer, "soil", "vwc");
    if (sm.length() == 0) sm = extractValueAfter(jsonBuffer, "soil", "humi");
    if (sm.length() == 0) sm = extractValueAfter(jsonBuffer, "soilSensor", "moisture");

    String st = extractValueAfter(jsonBuffer, "soil", "temp");
    if (st.length() == 0) st = extractValueAfter(jsonBuffer, "soilSensor", "temp");

    String se = extractValueAfter(jsonBuffer, "soil", "ec");
    if (se.length() == 0) se = extractValueAfter(jsonBuffer, "soil", "conductivity");
    if (se.length() == 0) se = extractValueAfter(jsonBuffer, "soilSensor", "ec");

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

    Serial.printf("Air:  T=%.1f RH=%.1f VPD=%.2f\n", latest.temp, latest.humi, latest.vpd);
    Serial.printf("Soil: VWC=%.1f T=%.1f EC=%.1f\n", latest.soilMoisture, latest.soilTemp, latest.soilEc);
    Serial.printf("Fan:  on=%d lvl=%d  Light: on=%d lvl=%d\n", latest.fanOn, latest.fanLevel, latest.lightOn, latest.lightLevel);

    jsonBuffer = "";
    blink(1, 50);
  }

  if (jsonBuffer.length() > 2500) {
    Serial.println("[WARN] buffer overflow, dumping");
    lastRawPayload = jsonBuffer;
    lastRawPayloadFresh = true;
    jsonBuffer = "";
  }
}

class BleClientCb : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient* c) override {
    Serial.println("[BLE] connected");
    bleConnected = true;
  }
  void onDisconnect(NimBLEClient* c) override {
    Serial.println("[BLE] disconnected");
    bleConnected = false;
    notifyChar = nullptr;
  }
};

bool connectBle() {
  if (bleAddress.length() == 0) {
    Serial.println("[BLE] no MAC address configured, skipping");
    return false;
  }

  bleAddress.toLowerCase();
  Serial.printf("[BLE] connecting to %s\n", bleAddress.c_str());

  if (bleClient == nullptr) {
    bleClient = NimBLEDevice::createClient();
    bleClient->setClientCallbacks(new BleClientCb(), false);
  }

  if (!bleClient->connect(NimBLEAddress(bleAddress.c_str(), BLE_ADDR_PUBLIC))) {
    Serial.println("[BLE] connect failed");
    return false;
  }

  delay(300);

  Serial.println("[BLE] ─── discovering services ───");
  std::vector<NimBLERemoteService*>* services = bleClient->getServices(true);
  if (!services || services->empty()) {
    Serial.println("[BLE] no services found");
    bleClient->disconnect();
    return false;
  }

  NimBLERemoteCharacteristic* foundNotifyChar = nullptr;
  Serial.printf("[BLE] %d services discovered:\n", (int)services->size());

  for (auto* svc : *services) {
    std::string svcStr = svc->getUUID().toString();
    Serial.printf("  SVC %s\n", svcStr.c_str());
    std::vector<NimBLERemoteCharacteristic*>* chars = svc->getCharacteristics(true);
    if (!chars) continue;
    for (auto* chr : *chars) {
      bool canN = chr->canNotify();
      bool canR = chr->canRead();
      bool canW = chr->canWrite() || chr->canWriteNoResponse();
      Serial.printf("    CHR %s [%s%s%s]\n",
                    chr->getUUID().toString().c_str(),
                    canR ? "R" : "-", canW ? "W" : "-", canN ? "N" : "-");
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

  Serial.printf("[BLE] subscribing to %s (in svc %s)\n",
                foundNotifyChar->getUUID().toString().c_str(),
                foundNotifyChar->getRemoteService()->getUUID().toString().c_str());

  if (!foundNotifyChar->subscribe(true, notifyCallback)) {
    Serial.println("[BLE] subscribe failed");
    bleClient->disconnect();
    return false;
  }

  notifyChar = foundNotifyChar;
  Serial.println("[BLE] subscribed - telemetry should flow shortly");
  blink(3, 100);
  return true;
}

String scanForGgs() {
  Serial.println("[BLE] scanning for SF-GGS-* devices...");
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);
  NimBLEScanResults results = scan->start(BLE_SCAN_TIMEOUT_SEC, false);

  String found = "";
  int matches = 0;
  for (int i = 0; i < results.getCount(); i++) {
    NimBLEAdvertisedDevice dev = results.getDevice(i);
    String name = dev.getName().c_str();
    if (name.startsWith("SF-GGS")) {
      Serial.printf("  Found: %s @ %s\n", name.c_str(), dev.getAddress().toString().c_str());
      found = dev.getAddress().toString().c_str();
      matches++;
    }
  }
  scan->clearResults();

  if (matches == 0) {
    Serial.println("[BLE] no GGS devices found");
    return "";
  }
  if (matches > 1) {
    Serial.println("[BLE] multiple GGS devices found - set MAC explicitly");
    return "";
  }
  return found;
}

bool postTelemetry(bool heartbeatOnly) {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (supabaseUrl.length() == 0 || supabaseAnonKey.length() == 0) return false;

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
    if (!isnan(latest.temp)) t["temp_c"] = latest.temp;
    if (!isnan(latest.humi)) t["humidity_pct"] = latest.humi;
    if (!isnan(latest.vpd)) t["vpd_kpa"] = latest.vpd;
    if (!isnan(latest.soilMoisture)) t["soil_vwc_pct"] = latest.soilMoisture;
    if (!isnan(latest.soilTemp))     t["soil_temp_c"] = latest.soilTemp;
    if (!isnan(latest.soilEc))       t["soil_ec"]      = latest.soilEc;
    if (latest.fanOn >= 0) t["fan_on"] = latest.fanOn;
    if (latest.fanLevel >= 0) t["fan_level"] = latest.fanLevel;
    if (latest.lightOn >= 0) t["light_on"] = latest.lightOn;
    if (latest.lightLevel >= 0) t["light_level"] = latest.lightLevel;
    t["captured_ms_ago"] = millis() - latest.capturedAt;
  }

  if (rawDumpMode && lastRawPayloadFresh) {
    doc["rawPayload"] = lastRawPayload;
    lastRawPayloadFresh = false;
  }

  String body;
  serializeJson(doc, body);

  WiFiClientSecure client;
  client.setInsecure();

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

WiFiManager wm;
WiFiManagerParameter* p_supaUrl;
WiFiManagerParameter* p_supaKey;
WiFiManagerParameter* p_bleMac;
WiFiManagerParameter* p_room;
WiFiManagerParameter* p_rawDump;

void saveConfigCallback() {
  Serial.println("[CFG] saving");
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
  rawDumpMode = prefs.getBool("rawDump", true);
  prefs.end();

  uint64_t chipId = ESP.getEfuseMac();
  char buf[24];
  snprintf(buf, sizeof(buf), "ggs-bridge-%012llx", chipId);
  deviceId = buf;

  Serial.println("──── Config loaded ────");
  Serial.printf("  Device ID:    %s\n", deviceId.c_str());
  Serial.printf("  Supabase URL: %s\n", supabaseUrl.length() ? supabaseUrl.c_str() : "(unset)");
  Serial.printf("  Supabase key: %s\n", supabaseAnonKey.length() ? "***set***" : "(unset)");
  Serial.printf("  BLE MAC:      %s\n", bleAddress.length() ? bleAddress.c_str() : "(autoscan)");
  Serial.printf("  Room:         %s\n", roomLabel.c_str());
  Serial.printf("  rawDump:      %s\n", rawDumpMode ? "ON" : "OFF");
}

void startCaptivePortal() {
  char apSsid[32];
  uint64_t chipId = ESP.getEfuseMac();
  snprintf(apSsid, sizeof(apSsid), "%s%04X", AP_NAME_PREFIX, (uint16_t)(chipId & 0xFFFF));

  p_supaUrl = new WiFiManagerParameter("supaUrl", "Supabase URL", supabaseUrl.c_str(), 80);
  p_supaKey = new WiFiManagerParameter("supaKey", "Supabase anon key", supabaseAnonKey.c_str(), 256);
  p_bleMac  = new WiFiManagerParameter("bleMac",  "GGS BLE MAC (blank=autoscan)", bleAddress.c_str(), 18);
  p_room    = new WiFiManagerParameter("room",    "Room label", roomLabel.c_str(), 40);
  p_rawDump = new WiFiManagerParameter("rawDump", "rawDump (1/0)", rawDumpMode ? "1" : "0", 2);

  wm.addParameter(p_supaUrl);
  wm.addParameter(p_supaKey);
  wm.addParameter(p_bleMac);
  wm.addParameter(p_room);
  wm.addParameter(p_rawDump);
  wm.setSaveConfigCallback(saveConfigCallback);
  wm.setConfigPortalTimeout(CONFIG_PORTAL_TIMEOUT_SEC);

  if (!wm.autoConnect(apSsid, AP_PASSWORD)) {
    Serial.println("[WM] timed out, rebooting");
    delay(1000);
    ESP.restart();
  }

  loadConfig();
  Serial.printf("[WM] connected, IP=%s\n", WiFi.localIP().toString().c_str());
}

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
  NimBLEDevice::setMTU(517);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

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
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] disconnected, retrying...");
    WiFi.reconnect();
    delay(2000);
    return;
  }

  if (!bleConnected && millis() - lastBleAttemptMs > BLE_RECONNECT_MS) {
    lastBleAttemptMs = millis();
    connectBle();
  }

  unsigned long now = millis();
  if (latest.fresh && now - lastPostMs > POST_INTERVAL_MS) {
    lastPostMs = now;
    postTelemetry(false);
  } else if (now - lastHeartbeatMs > HEARTBEAT_MS) {
    lastHeartbeatMs = now;
    postTelemetry(true);
  }

  delay(50);
}
