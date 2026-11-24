#include "web.h"
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include "config.h"
#include "gpio_map.h"
#include "relay.h"
#include "fan.h"
#include "sensor.h"
#include "itag.h"
#include "mqtt.h"

namespace {
AsyncWebServer server(80);

constexpr uint32_t SESSION_IDLE_MS = 5UL * 60UL * 1000UL;
constexpr uint8_t MAX_FAILS = 5;
constexpr uint32_t LOCKOUT_MS = 5UL * 60UL * 1000UL;
constexpr size_t MAX_BODY = 4096;

struct Session {
  String token;
  uint32_t lastMs;
};
Session session;

uint8_t failCount = 0;
uint32_t lockoutUntilMs = 0;

uint32_t restartAt = 0;
bool restartPending = false;

String randomToken() {
  char out[33];
  for (int i = 0; i < 4; i++) {
    snprintf(out + i * 8, 9, "%08lx", (unsigned long)esp_random());
  }
  return String(out);
}

bool sessionValid(AsyncWebServerRequest* req) {
  if (!req->hasHeader("Authorization")) return false;
  String h = req->header("Authorization");
  if (!h.startsWith("Bearer ")) return false;
  String t = h.substring(7);
  if (session.token.length() == 0 || t != session.token) return false;
  if (millis() - session.lastMs > SESSION_IDLE_MS) {
    session.token = "";
    return false;
  }
  session.lastMs = millis();
  return true;
}

void sendJson(AsyncWebServerRequest* req, int code, const String& body) {
  req->send(code, "application/json", body);
}

void sendError(AsyncWebServerRequest* req, int code, const char* msg) {
  JsonDocument d;
  d["error"] = msg;
  String out;
  serializeJson(d, out);
  sendJson(req, code, out);
}

// Wraps an authenticated handler
#define REQUIRE_AUTH(req)                                  \
  do {                                                     \
    if (!sessionValid(req)) {                              \
      sendError(req, 401, "unauthorized");                 \
      return;                                              \
    }                                                      \
  } while (0)

// Body accumulator for POST handlers (single in-flight request assumed for body parsing)
char bodyBuf[MAX_BODY + 1];
size_t bodyLen = 0;

void bodyChunk(AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
  if (total > MAX_BODY) return;
  if (index == 0) bodyLen = 0;
  if (bodyLen + len > MAX_BODY) return;
  memcpy(bodyBuf + bodyLen, data, len);
  bodyLen += len;
  bodyBuf[bodyLen] = '\0';
  (void)req;
}

String statusJson() {
  const AppConfig& cfg = Config::get();
  JsonDocument d;
  d["defaultPin"] = strcmp(cfg.pin, "123456") == 0;
  d["heapInternal"] = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  d["mqtt"] = Mqtt::connected();
  d["itag"] = ITag::connected();
  d["itagEnabled"] = cfg.itagEnabled;

  JsonArray rel = d["relays"].to<JsonArray>();
  for (uint8_t i = 0; i < MAX_RELAYS; i++) {
    if (!Relay::isEnabled(i)) continue;
    JsonObject o = rel.add<JsonObject>();
    o["index"] = i + 1;
    o["name"] = cfg.relays[i].name[0] ? String(cfg.relays[i].name) : String("Relay ") + String(i + 1);
    o["on"] = Relay::get(i);
  }

  if (cfg.fan.enabled) {
    JsonObject f = d["fan"].to<JsonObject>();
    f["name"] = cfg.fan.name;
    f["speed"] = Fan::getSpeed();
    f["maxSpeed"] = Fan::maxSpeed();
  }

  if (cfg.sensor.type != SensorType::None) {
    JsonObject s = d["sensor"].to<JsonObject>();
    s["valid"] = Sensor::valid();
    s["temp"] = Sensor::lastTemp();
    s["hasHumidity"] = Sensor::hasHumidity();
    s["humidity"] = Sensor::lastHumidity();
  }

  String out;
  serializeJson(d, out);
  return out;
}

void registerRoutes() {
  server.on("/api/login", HTTP_POST, [](AsyncWebServerRequest* req) {
    if (lockoutUntilMs && millis() < lockoutUntilMs) {
      sendError(req, 429, "locked out, try again later");
      return;
    }
    JsonDocument in;
    if (deserializeJson(in, bodyBuf) || !in["pin"].is<const char*>()) {
      sendError(req, 400, "bad request");
      return;
    }
    if (strcmp(in["pin"], Config::get().pin) != 0) {
      failCount++;
      if (failCount >= MAX_FAILS) {
        lockoutUntilMs = millis() + LOCKOUT_MS;
        failCount = 0;
        sendError(req, 429, "too many attempts, locked for 5 minutes");
        return;
      }
      sendError(req, 401, "wrong pin");
      return;
    }
    failCount = 0;
    session.token = randomToken();
    session.lastMs = millis();
    JsonDocument out;
    out["token"] = session.token;
    String s;
    serializeJson(out, s);
    sendJson(req, 200, s);
  }, nullptr, bodyChunk);

  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* req) {
    REQUIRE_AUTH(req);
    sendJson(req, 200, statusJson());
  });

  server.on("/api/relay", HTTP_POST, [](AsyncWebServerRequest* req) {
    REQUIRE_AUTH(req);
    JsonDocument in;
    if (deserializeJson(in, bodyBuf)) return sendError(req, 400, "bad json");
    int idx = in["index"] | 0;
    if (idx < 1 || idx > MAX_RELAYS || !in["on"].is<bool>()) return sendError(req, 400, "bad relay");
    if (!Relay::set(idx - 1, in["on"].as<bool>())) return sendError(req, 409, "relay not enabled");
    sendJson(req, 200, "{\"ok\":true}");
  }, nullptr, bodyChunk);

  server.on("/api/fan", HTTP_POST, [](AsyncWebServerRequest* req) {
    REQUIRE_AUTH(req);
    JsonDocument in;
    if (deserializeJson(in, bodyBuf)) return sendError(req, 400, "bad json");
    int sp = in["speed"] | -1;
    if (sp < 0 || sp > 3) return sendError(req, 400, "speed must be 0..3");
    if (!Fan::setSpeed(sp)) return sendError(req, 409, "fan not enabled or speed above max");
    sendJson(req, 200, "{\"ok\":true}");
  }, nullptr, bodyChunk);

  server.on("/api/config", HTTP_GET, [](AsyncWebServerRequest* req) {
    REQUIRE_AUTH(req);
    sendJson(req, 200, Config::toJson(false));
  });

  server.on("/api/config", HTTP_POST, [](AsyncWebServerRequest* req) {
    REQUIRE_AUTH(req);
    AppConfig next = Config::get();
    String err;
    if (!Config::fromJson(bodyBuf, next, err)) return sendError(req, 400, err.c_str());

    std::vector<String> warnings;
    if (!GpioMap::validate(next, err, warnings)) return sendError(req, 400, err.c_str());
    if (!Config::save(next)) return sendError(req, 500, "save failed");

    JsonDocument out;
    out["ok"] = true;
    out["restartInMs"] = 1500;
    JsonArray w = out["warnings"].to<JsonArray>();
    for (auto& s : warnings) w.add(s);
    String s;
    serializeJson(out, s);
    sendJson(req, 200, s);
    Web::requestRestart(1500);
  }, nullptr, bodyChunk);

  server.on("/api/factory-reset", HTTP_POST, [](AsyncWebServerRequest* req) {
    REQUIRE_AUTH(req);
    Config::resetToDefaults();
    sendJson(req, 200, "{\"ok\":true}");
    Web::requestRestart(1500);
  });

  server.on("/api/ble/scan", HTTP_POST, [](AsyncWebServerRequest* req) {
    REQUIRE_AUTH(req);
    ITag::startScan(5000);
    sendJson(req, 200, "{\"ok\":true}");
  });

  server.on("/api/ble/results", HTTP_GET, [](AsyncWebServerRequest* req) {
    REQUIRE_AUTH(req);
    JsonDocument d;
    d["scanning"] = ITag::scanning();
    JsonArray arr = d["devices"].to<JsonArray>();
    for (auto& f : ITag::scanResults()) {
      JsonObject o = arr.add<JsonObject>();
      o["name"] = f.name;
      o["addr"] = f.addr;
      o["rssi"] = f.rssi;
    }
    String s;
    serializeJson(d, s);
    sendJson(req, 200, s);
  });

  // Learn mode: raw notification bytes to identify what the single-button iTag sends
  server.on("/api/ble/raw", HTTP_GET, [](AsyncWebServerRequest* req) {
    REQUIRE_AUTH(req);
    JsonDocument d;
    JsonArray arr = d["events"].to<JsonArray>();
    for (auto& e : ITag::recentRaw()) {
      JsonObject o = arr.add<JsonObject>();
      o["ms"] = e.ms;
      o["hex"] = e.hex;
    }
    String s;
    serializeJson(d, s);
    sendJson(req, 200, s);
  });

  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
  server.onNotFound([](AsyncWebServerRequest* req) { req->send(404, "text/plain", "not found"); });
}
}  // namespace

namespace Web {

void begin() {
  registerRoutes();
  server.begin();
}

void requestRestart(uint32_t delayMs) {
  restartAt = millis() + delayMs;
  restartPending = true;
}

void tick() {
  if (restartPending && (int32_t)(millis() - restartAt) >= 0) {
    restartPending = false;
    ESP.restart();
  }
}

}  // namespace Web
