#include "config.h"
#include <ArduinoJson.h>
#include <Preferences.h>
#include <string.h>
#include <ctype.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace {
Preferences prefs;
AppConfig current;
SemaphoreHandle_t configMutex = nullptr;
portMUX_TYPE configMux = portMUX_INITIALIZER_UNLOCKED;
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
  JsonObject schedule = o["schedule"].to<JsonObject>();
  schedule["enabled"] = r.schedule.enabled;
  schedule["on"] = r.schedule.on;
  schedule["off"] = r.schedule.off;
  JsonObject detector = o["humanDetector"].to<JsonObject>();
  detector["enabled"] = r.humanDetector.enabled;
  detector["gpio"] = r.humanDetector.gpio;
  detector["holdSec"] = r.humanDetector.holdSec;
}

bool checkString(JsonVariantConst value, size_t capacity, const char* field, String& err) {
  if (value.isNull()) return true;
  if (!value.is<const char*>()) {
    err = String(field) + " must be a string";
    return false;
  }
  if (strlen(value.as<const char*>()) >= capacity) {
    err = String(field) + " is too long";
    return false;
  }
  return true;
}

bool checkBool(JsonVariantConst value, const char* field, String& err) {
  if (value.isNull() || value.is<bool>()) return true;
  err = String(field) + " must be boolean";
  return false;
}

bool checkInt(JsonVariantConst value, int minimum, int maximum, const char* field, String& err) {
  if (value.isNull()) return true;
  if (!value.is<int>()) {
    err = String(field) + " must be an integer";
    return false;
  }
  const int number = value.as<int>();
  if (number < minimum || number > maximum) {
    err = String(field) + " is out of range";
    return false;
  }
  return true;
}

bool checkObject(JsonVariantConst value, const char* field, String& err) {
  if (value.isNull() || value.is<JsonObjectConst>()) return true;
  err = String(field) + " must be an object";
  return false;
}

void readRelay(JsonObjectConst o, RelayCfg& r) {
  r.enabled = o["enabled"] | r.enabled;
  if (o["name"].is<const char*>()) copyStr(r.name, sizeof(r.name), o["name"]);
  r.gpio = o["gpio"] | r.gpio;
  r.buttonGpio = o["button"] | r.buttonGpio;
  r.activeHigh = o["activeHigh"] | r.activeHigh;
  JsonObjectConst schedule = o["schedule"];
  if (!schedule.isNull()) {
    r.schedule.enabled = schedule["enabled"] | r.schedule.enabled;
    if (schedule["on"].is<const char*>()) copyStr(r.schedule.on, sizeof(r.schedule.on), schedule["on"]);
    if (schedule["off"].is<const char*>()) copyStr(r.schedule.off, sizeof(r.schedule.off), schedule["off"]);
  }
  JsonObjectConst detector = o["humanDetector"];
  if (!detector.isNull()) {
    r.humanDetector.enabled = detector["enabled"] | r.humanDetector.enabled;
    r.humanDetector.gpio = detector["gpio"] | r.humanDetector.gpio;
    r.humanDetector.holdSec = detector["holdSec"] | r.humanDetector.holdSec;
  }
}
}  // namespace

namespace Config {

String toJsonUnlocked(const AppConfig& cfg, bool includeSecrets);

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
  AppConfig snapshot;
  if (configMutex) {
    xSemaphoreTake(configMutex, portMAX_DELAY);
    snapshot = current;
    xSemaphoreGive(configMutex);
  } else {
    portENTER_CRITICAL(&configMux);
    snapshot = current;
    portEXIT_CRITICAL(&configMux);
  }
  return snapshot;
}

bool save(const AppConfig& cfg) {
  String json = toJsonUnlocked(cfg, true);
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
  if (configMutex) {
    xSemaphoreTake(configMutex, portMAX_DELAY);
    current = cfg;
    xSemaphoreGive(configMutex);
  } else {
    portENTER_CRITICAL(&configMux);
    current = cfg;
    portEXIT_CRITICAL(&configMux);
  }
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
  if (configMutex) {
    xSemaphoreTake(configMutex, portMAX_DELAY);
    current = AppConfig{};
    xSemaphoreGive(configMutex);
  } else {
    portENTER_CRITICAL(&configMux);
    current = AppConfig{};
    portEXIT_CRITICAL(&configMux);
  }
  return true;
}

String toJsonUnlocked(const AppConfig& cfg, bool includeSecrets) {
  JsonDocument doc;
  doc["pin"] = includeSecrets ? cfg.pin : "";
  doc["apPass"] = includeSecrets ? cfg.apPass : "";
  doc["hostname"] = cfg.hostname;

  JsonObject w = doc["wifi"].to<JsonObject>();
  w["ssid"] = cfg.wifi.ssid;
  w["pass"] = includeSecrets ? cfg.wifi.pass : "";

  JsonObject m = doc["mqtt"].to<JsonObject>();
  m["enabled"] = cfg.mqtt.enabled;
  m["scheme"] = cfg.mqtt.scheme;
  m["host"] = cfg.mqtt.host;
  m["port"] = cfg.mqtt.port;
  m["path"] = cfg.mqtt.path;
  m["user"] = includeSecrets ? cfg.mqtt.user : "";
  m["pass"] = includeSecrets ? cfg.mqtt.pass : "";
  m["prefix"] = cfg.mqtt.topicPrefix;

  JsonArray rel = doc["relays"].to<JsonArray>();
  for (uint8_t i = 0; i < MAX_RELAYS; i++) {
    writeRelay(rel.add<JsonObject>(), cfg.relays[i], i);
  }

  JsonObject f = doc["fan"].to<JsonObject>();
  f["enabled"] = cfg.fan.enabled;
  f["name"] = cfg.fan.name;
  f["maxSpeed"] = cfg.fan.maxSpeed;
  f["activeHigh"] = cfg.fan.activeHigh;
  JsonArray fg = f["speedGpio"].to<JsonArray>();
  for (uint8_t i = 0; i < FAN_MAX_SPEEDS; i++) fg.add(cfg.fan.speedGpio[i]);

  JsonObject s = doc["sensor"].to<JsonObject>();
  s["type"] = (uint8_t)cfg.sensor.type;
  s["gpio"] = cfg.sensor.gpio;
  s["intervalSec"] = cfg.sensor.intervalSec;

  JsonObject it = doc["itag"].to<JsonObject>();
  it["enabled"] = cfg.itagEnabled;
  it["addr"] = cfg.itagAddr;

  String out;
  serializeJson(doc, out);
  return out;
}

String toJson(bool includeSecrets) {
  const AppConfig snapshot = get();
  return toJsonUnlocked(snapshot, includeSecrets);
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
  if (!doc.is<JsonObjectConst>()) {
    err = "configuration root must be an object";
    return false;
  }

  if (!checkString(doc["pin"], sizeof(out.pin), "pin", err) ||
      !checkString(doc["hostname"], sizeof(out.hostname), "hostname", err) ||
      !checkString(doc["apPass"], sizeof(out.apPass), "apPass", err) ||
      !checkObject(doc["wifi"], "wifi", err) ||
      !checkObject(doc["mqtt"], "mqtt", err) ||
      !checkObject(doc["fan"], "fan", err) ||
      !checkObject(doc["sensor"], "sensor", err) ||
      !checkObject(doc["itag"], "itag", err)) return false;

  if (!checkString(doc["wifi"]["ssid"], 33, "wifi.ssid", err) ||
      !checkString(doc["wifi"]["pass"], sizeof(out.wifi.pass), "wifi.pass", err) ||
      !checkString(doc["mqtt"]["scheme"], sizeof(out.mqtt.scheme), "mqtt.scheme", err) ||
      !checkString(doc["mqtt"]["host"], sizeof(out.mqtt.host), "mqtt.host", err) ||
      !checkString(doc["mqtt"]["path"], sizeof(out.mqtt.path), "mqtt.path", err) ||
      !checkString(doc["mqtt"]["user"], sizeof(out.mqtt.user), "mqtt.user", err) ||
      !checkString(doc["mqtt"]["pass"], sizeof(out.mqtt.pass), "mqtt.pass", err) ||
      !checkString(doc["mqtt"]["prefix"], sizeof(out.mqtt.topicPrefix), "mqtt.prefix", err) ||
      !checkBool(doc["mqtt"]["enabled"], "mqtt.enabled", err) ||
      !checkInt(doc["mqtt"]["port"], 1, 65535, "mqtt.port", err) ||
      !checkBool(doc["fan"]["enabled"], "fan.enabled", err) ||
      !checkString(doc["fan"]["name"], sizeof(out.fan.name), "fan.name", err) ||
      !checkInt(doc["fan"]["maxSpeed"], 2, 3, "fan.maxSpeed", err) ||
      !checkBool(doc["fan"]["activeHigh"], "fan.activeHigh", err) ||
      !checkInt(doc["sensor"]["type"], 0, 2, "sensor.type", err) ||
      !checkInt(doc["sensor"]["gpio"], -1, 39, "sensor.gpio", err) ||
      !checkInt(doc["sensor"]["intervalSec"], 5, 65535, "sensor.intervalSec", err) ||
      !checkBool(doc["itag"]["enabled"], "itag.enabled", err) ||
      !checkString(doc["itag"]["addr"], sizeof(out.itagAddr), "itag.addr", err)) return false;

  if (!doc["wifi"]["pass"].isNull() && doc["wifi"]["pass"].is<const char*>() &&
      strlen(doc["wifi"]["pass"].as<const char*>()) > 0 &&
      strlen(doc["wifi"]["pass"].as<const char*>()) < 8) {
    err = "WiFi password must be empty or 8 to 63 characters";
    return false;
  }

  JsonVariantConst relaysValue = doc["relays"];
  if (!relaysValue.isNull()) {
    if (!relaysValue.is<JsonArrayConst>()) {
      err = "relays must be an array";
      return false;
    }
    JsonArrayConst relays = relaysValue.as<JsonArrayConst>();
    if (relays.size() > MAX_RELAYS) {
      err = "too many relays";
      return false;
    }
    for (JsonVariantConst relay : relays) {
      if (!relay.is<JsonObjectConst>()) {
        err = "each relay must be an object";
        return false;
      }
      JsonObjectConst item = relay.as<JsonObjectConst>();
      if (!checkBool(item["enabled"], "relay.enabled", err) ||
          !checkString(item["name"], NAME_LEN, "relay.name", err) ||
          !checkInt(item["gpio"], -1, 39, "relay.gpio", err) ||
          !checkInt(item["button"], -1, 39, "relay.button", err) ||
          !checkBool(item["activeHigh"], "relay.activeHigh", err) ||
          !checkObject(item["schedule"], "relay.schedule", err) ||
          !checkObject(item["humanDetector"], "relay.humanDetector", err) ||
          !checkBool(item["schedule"]["enabled"], "relay.schedule.enabled", err) ||
          !checkString(item["schedule"]["on"], 6, "relay.schedule.on", err) ||
          !checkString(item["schedule"]["off"], 6, "relay.schedule.off", err) ||
          !checkBool(item["humanDetector"]["enabled"], "relay.humanDetector.enabled", err) ||
          !checkInt(item["humanDetector"]["gpio"], -1, 39, "relay.humanDetector.gpio", err) ||
          !checkInt(item["humanDetector"]["holdSec"], 1, 3600, "relay.humanDetector.holdSec", err)) return false;
    }
  }

  JsonVariantConst speedPinsValue = doc["fan"]["speedGpio"];
  if (!speedPinsValue.isNull()) {
    if (!speedPinsValue.is<JsonArrayConst>()) {
      err = "fan.speedGpio must be an array";
      return false;
    }
    JsonArrayConst speedPins = speedPinsValue.as<JsonArrayConst>();
    if (speedPins.size() > FAN_MAX_SPEEDS) {
      err = "too many fan GPIO entries";
      return false;
    }
    for (JsonVariantConst pin : speedPins) {
      if (!checkInt(pin, -1, 39, "fan.speedGpio", err)) return false;
    }
  }

  if (doc["itag"]["addr"].is<const char*>() && strlen(doc["itag"]["addr"].as<const char*>()) > 0) {
    const char* address = doc["itag"]["addr"].as<const char*>();
    if (strlen(address) != 17) {
      err = "itag.addr must be a MAC address";
      return false;
    }
    for (size_t i = 0; i < 17; ++i) {
      if (i % 3 == 2) {
        if (address[i] != ':') {
          err = "itag.addr must be a colon-separated MAC address";
          return false;
        }
      } else if (!isxdigit(static_cast<unsigned char>(address[i]))) {
        err = "itag.addr contains invalid hexadecimal characters";
        return false;
      }
    }
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

  if (out.hostname[0] == '\\0') {
    err = "hostname must not be empty";
    return false;
  }
  if (out.mqtt.enabled && out.mqtt.host[0] == '\\0') {
    err = "mqtt.host is required when MQTT is enabled";
    return false;
  }
  if (out.itagEnabled && out.itagAddr[0] == '\\0') {
    err = "itag.addr is required when iTag is enabled";
    return false;
  }

  return true;
}

}  // namespace Config
