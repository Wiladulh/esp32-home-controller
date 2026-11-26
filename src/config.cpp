#include "config.h"
#include <ArduinoJson.h>
#include <Preferences.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace {
Preferences prefs;
AppConfig current;
SemaphoreHandle_t configMutex = nullptr;
constexpr const char* NS = "cfg";
constexpr const char* KEY = "json";

void copyStr(char* dst, size_t cap, const char* src) {
  strlcpy(dst, src ? src : "", cap);
}

void writeRelay(JsonObject o, const RelayCfg& r, uint8_t index) {
  o["enabled"] = r.enabled;
  if (r.name[0] != '\0') {
    o["name"] = r.name;
  } else {
    o["name"] = "Relay " + String(index + 1);
  }
  o["gpio"] = r.gpio;
  o["button"] = r.buttonGpio;
  o["activeHigh"] = r.activeHigh;
}

void readRelay(JsonObjectConst o, RelayCfg& r) {
  r.enabled = o["enabled"] | r.enabled;
  if (o["name"].is<const char*>()) copyStr(r.name, sizeof(r.name), o["name"]);
  r.gpio = o["gpio"] | r.gpio;
  r.buttonGpio = o["button"] | r.buttonGpio;
  r.activeHigh = o["activeHigh"] | r.activeHigh;
}
}  // namespace

namespace Config {

String toJsonUnlocked(bool includeSecrets);

void begin() {
  if (!configMutex) configMutex = xSemaphoreCreateMutex();
  // A missing namespace is normal on first boot. Do not read through an
  // unopened Preferences handle.
  if (!prefs.begin(NS, true)) {
    current = AppConfig{};
    Serial.println("[cfg] NVS namespace unavailable; using defaults");
    return;
  }
  String json = prefs.getString(KEY, "");
  prefs.end();
  if (json.length() == 0) {
    current = AppConfig{};
    return;
  }
  AppConfig loaded;
  String err;
  if (fromJson(json.c_str(), loaded, err)) {
    current = loaded;
  } else {
    // corrupt config: fall back to defaults so the device still boots
    current = AppConfig{};
  }
}

AppConfig get() {
  if (configMutex) xSemaphoreTake(configMutex, portMAX_DELAY);
  AppConfig snapshot = current;
  if (configMutex) xSemaphoreGive(configMutex);
  return snapshot;
}

bool save(const AppConfig& cfg) {
  if (configMutex) xSemaphoreTake(configMutex, portMAX_DELAY);
  AppConfig previous = current;
  current = cfg;
  String json = toJsonUnlocked(true);
  current = previous;
  if (configMutex) xSemaphoreGive(configMutex);
  if (!prefs.begin(NS, false)) {
    Serial.println("[cfg] NVS write namespace open failed");
    return false;
  }
  size_t n = prefs.putString(KEY, json);
  prefs.end();

  // Read back only after a successful write-open. A failed read-open is
  // treated as a failed save rather than as an empty configuration.
  String stored;
  bool readOk = prefs.begin(NS, true);
  if (readOk) {
    stored = prefs.getString(KEY, "");
    prefs.end();
  }
  Serial.printf("[cfg] save: json=%u bytes, nvs wrote=%u, read back=%u, match=%d\n",
                (unsigned)json.length(), (unsigned)n, (unsigned)stored.length(), (int)(readOk && stored == json));
  if (n == 0 || !readOk || stored != json) return false;
  if (configMutex) xSemaphoreTake(configMutex, portMAX_DELAY);
  current = cfg;
  if (configMutex) xSemaphoreGive(configMutex);
  return true;
}

bool resetToDefaults() {
  if (!prefs.begin(NS, false)) {
    Serial.println("[cfg] NVS reset namespace open failed");
    return false;
  }
  const bool cleared = prefs.clear();
  const bool empty = cleared && !prefs.isKey(KEY);
  prefs.end();
  if (!empty) {
    Serial.println("[cfg] NVS reset failed; current config retained");
    return false;
  }
  if (configMutex) xSemaphoreTake(configMutex, portMAX_DELAY);
  current = AppConfig{};
  if (configMutex) xSemaphoreGive(configMutex);
  return true;
}

String toJsonUnlocked(bool includeSecrets) {
  JsonDocument doc;
  doc["pin"] = includeSecrets ? current.pin : "";
  doc["apPass"] = includeSecrets ? current.apPass : "";
  doc["hostname"] = current.hostname;

  JsonObject w = doc["wifi"].to<JsonObject>();
  w["ssid"] = current.wifi.ssid;
  w["pass"] = includeSecrets ? current.wifi.pass : "";

  JsonObject m = doc["mqtt"].to<JsonObject>();
  m["enabled"] = current.mqtt.enabled;
  m["scheme"] = current.mqtt.scheme;
  m["host"] = current.mqtt.host;
  m["port"] = current.mqtt.port;
  m["path"] = current.mqtt.path;
  m["user"] = includeSecrets ? current.mqtt.user : "";
  m["pass"] = includeSecrets ? current.mqtt.pass : "";
  m["prefix"] = current.mqtt.topicPrefix;

  JsonArray rel = doc["relays"].to<JsonArray>();
  for (uint8_t i = 0; i < MAX_RELAYS; i++) {
    writeRelay(rel.add<JsonObject>(), current.relays[i], i);
  }

  JsonObject f = doc["fan"].to<JsonObject>();
  f["enabled"] = current.fan.enabled;
  f["name"] = current.fan.name;
  f["maxSpeed"] = current.fan.maxSpeed;
  f["activeHigh"] = current.fan.activeHigh;
  JsonArray fg = f["speedGpio"].to<JsonArray>();
  for (uint8_t i = 0; i < FAN_MAX_SPEEDS; i++) fg.add(current.fan.speedGpio[i]);

  JsonObject s = doc["sensor"].to<JsonObject>();
  s["type"] = (uint8_t)current.sensor.type;
  s["gpio"] = current.sensor.gpio;
  s["intervalSec"] = current.sensor.intervalSec;

  JsonObject it = doc["itag"].to<JsonObject>();
  it["enabled"] = current.itagEnabled;
  it["addr"] = current.itagAddr;

  String out;
  serializeJson(doc, out);
  return out;
}

String toJson(bool includeSecrets) {
  if (configMutex) xSemaphoreTake(configMutex, portMAX_DELAY);
  String out = toJsonUnlocked(includeSecrets);
  if (configMutex) xSemaphoreGive(configMutex);
  return out;
}

// Merges JSON into `out`. Missing keys keep their current value.
// Empty secret fields (pin, wifi pass, mqtt user/pass) keep the stored value,
// because GET never returns secrets.
bool fromJson(const char* json, AppConfig& out, String& err) {
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, json);
  if (e) {
    err = String("invalid json: ") + e.c_str();
    return false;
  }

  if (doc["pin"].is<const char*>()) {
    const char* p = doc["pin"];
    if (strlen(p) > 0) {
      if (strlen(p) != 6) { err = "pin must be 6 digits"; return false; }
      for (const char* c = p; *c; c++) {
        if (*c < '0' || *c > '9') { err = "pin must be digits"; return false; }
      }
      copyStr(out.pin, sizeof(out.pin), p);
    }
  }
  if (doc["hostname"].is<const char*>()) copyStr(out.hostname, sizeof(out.hostname), doc["hostname"]);
  if (doc["apPass"].is<const char*>()) {
    const char* ap = doc["apPass"];
    size_t n = strlen(ap);
    if (n > 0) {  // empty keeps the stored password
      if (n < 8 || n > 63) { err = "AP password must be 8 to 63 characters"; return false; }
      copyStr(out.apPass, sizeof(out.apPass), ap);
    }
  }

  JsonObjectConst w = doc["wifi"];
  if (!w.isNull()) {
    if (w["ssid"].is<const char*>()) copyStr(out.wifi.ssid, sizeof(out.wifi.ssid), w["ssid"]);
    if (w["pass"].is<const char*>() && strlen(w["pass"]) > 0)
      copyStr(out.wifi.pass, sizeof(out.wifi.pass), w["pass"]);
  }

  JsonObjectConst m = doc["mqtt"];
  if (!m.isNull()) {
    out.mqtt.enabled = m["enabled"] | out.mqtt.enabled;
    if (m["scheme"].is<const char*>()) {
      const char* sc = m["scheme"];
      if (strcmp(sc, "wss") && strcmp(sc, "ws") && strcmp(sc, "mqtts") && strcmp(sc, "mqtt")) {
        err = "mqtt scheme must be wss, ws, mqtts or mqtt";
        return false;
      }
      copyStr(out.mqtt.scheme, sizeof(out.mqtt.scheme), sc);
    }
    if (m["host"].is<const char*>()) copyStr(out.mqtt.host, sizeof(out.mqtt.host), m["host"]);
    out.mqtt.port = m["port"] | out.mqtt.port;
    if (m["path"].is<const char*>()) copyStr(out.mqtt.path, sizeof(out.mqtt.path), m["path"]);
    if (m["user"].is<const char*>() && strlen(m["user"]) > 0)
      copyStr(out.mqtt.user, sizeof(out.mqtt.user), m["user"]);
    if (m["pass"].is<const char*>() && strlen(m["pass"]) > 0)
      copyStr(out.mqtt.pass, sizeof(out.mqtt.pass), m["pass"]);
    if (m["prefix"].is<const char*>()) copyStr(out.mqtt.topicPrefix, sizeof(out.mqtt.topicPrefix), m["prefix"]);
  }

  JsonArrayConst rel = doc["relays"];
  if (!rel.isNull()) {
    uint8_t i = 0;
    for (JsonObjectConst r : rel) {
      if (i >= MAX_RELAYS) break;
      readRelay(r, out.relays[i]);
      i++;
    }
  }

  JsonObjectConst f = doc["fan"];
  if (!f.isNull()) {
    out.fan.enabled = f["enabled"] | out.fan.enabled;
    if (f["name"].is<const char*>()) copyStr(out.fan.name, sizeof(out.fan.name), f["name"]);
    out.fan.maxSpeed = f["maxSpeed"] | out.fan.maxSpeed;
    out.fan.activeHigh = f["activeHigh"] | out.fan.activeHigh;
    JsonArrayConst fg = f["speedGpio"];
    if (!fg.isNull()) {
      uint8_t i = 0;
      for (JsonVariantConst v : fg) {
        if (i >= FAN_MAX_SPEEDS) break;
        out.fan.speedGpio[i++] = v.as<int8_t>();
      }
    }
  }

  JsonObjectConst s = doc["sensor"];
  if (!s.isNull()) {
    out.sensor.type = (SensorType)(s["type"] | (uint8_t)out.sensor.type);
    out.sensor.gpio = s["gpio"] | out.sensor.gpio;
    out.sensor.intervalSec = s["intervalSec"] | out.sensor.intervalSec;
  }

  JsonObjectConst it = doc["itag"];
  if (!it.isNull()) {
    out.itagEnabled = it["enabled"] | out.itagEnabled;
    if (it["addr"].is<const char*>()) copyStr(out.itagAddr, sizeof(out.itagAddr), it["addr"]);
  }

  return true;
}

}  // namespace Config
