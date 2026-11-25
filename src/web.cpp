#include "web.h"
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <atomic>
#include <new>
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
constexpr const char* DEFAULT_PIN = "123456";

struct Session {
  String token;
  uint32_t lastMs = 0;
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

// Only the config (PIN-protected) endpoints check the session.
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

#define REQUIRE_AUTH(req)                                  \
  do {                                                     \
    if (!sessionValid(req)) {                              \
      sendError(req, 401, "unauthorized");                 \
      return;                                              \
    }                                                      \
  } while (0)

// Each request owns its body buffer. A small concurrency cap bounds heap use.
struct RequestBody {
  char data[MAX_BODY + 1]{};
  size_t length = 0;
  bool tooLarge = false;
};

constexpr uint8_t MAX_ACTIVE_BODIES = 2;
std::atomic<uint8_t> activeBodies{0};

RequestBody* requestBody(AsyncWebServerRequest* req) {
  return static_cast<RequestBody*>(req->_tempObject);
}

struct RequestBodyGuard {
  AsyncWebServerRequest* request;
  ~RequestBodyGuard() {
    auto* body = requestBody(request);
    if (body) {
      delete body;
      request->_tempObject = nullptr;
      activeBodies.fetch_sub(1, std::memory_order_acq_rel);
    }
  }
};

void bodyChunk(AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
  if (index == 0) {
    if (activeBodies.fetch_add(1, std::memory_order_acq_rel) >= MAX_ACTIVE_BODIES) {
      activeBodies.fetch_sub(1, std::memory_order_acq_rel);
      req->_tempObject = nullptr;
      return;
    }
    auto* state = new (std::nothrow) RequestBody();
    if (!state) {
      activeBodies.fetch_sub(1, std::memory_order_acq_rel);
      req->_tempObject = nullptr;
      return;
    }
    req->_tempObject = state;
    req->onDisconnect([req]() {
      auto* body = requestBody(req);
      if (body) {
        delete body;
        req->_tempObject = nullptr;
        activeBodies.fetch_sub(1, std::memory_order_acq_rel);
      }
    });
    state->tooLarge = total > MAX_BODY;
    Serial.printf("[web] body start, total=%u, heap=%u\n", (unsigned)total, (unsigned)ESP.getFreeHeap());
  }

  auto* state = requestBody(req);
  if (!state || state->tooLarge) return;
  if (index != state->length || state->length + len > MAX_BODY) {
    state->tooLarge = true;
    return;
  }
  memcpy(state->data + state->length, data, len);
  state->length += len;
  state->data[state->length] = '\0';
}

// POST bodies must be application/json. A cross-site form or text/plain request cannot
// send that type without a CORS preflight, and this device never answers preflights,
// so other websites cannot drive the public relay and fan endpoints.
bool bodyUsable(AsyncWebServerRequest* req) {
  const auto* body = requestBody(req);
  Serial.printf("[web] %s content-length=%u received=%u\n", req->url().c_str(),
                (unsigned)req->contentLength(), (unsigned)(body ? body->length : 0));
  if (!req->hasHeader("Content-Type") || !req->header("Content-Type").startsWith("application/json")) {
    sendError(req, 415, "content-type must be application/json");
    return false;
  }
  if (body && body->tooLarge) {
    sendError(req, 413, "body too large");
    return false;
  }
  if (!body && req->contentLength() > 0) {
    sendError(req, 503, "request body capacity unavailable");
    return false;
  }
  if (req->contentLength() != (body ? body->length : 0)) {
    sendError(req, 400, "incomplete body");
    return false;
  }
  return true;
}

String statusJson() {
  const AppConfig& cfg = Config::get();
  JsonDocument d;
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

// Config as shown in the Config tab: secrets blanked, plus board profile and PIN state.
String configJson() {
  JsonDocument d;
  deserializeJson(d, Config::toJson(false).c_str());
  d["defaultPin"] = strcmp(Config::get().pin, DEFAULT_PIN) == 0;
  JsonObject b = d["board"].to<JsonObject>();
  b["name"] = GpioMap::boardName();
  JsonArray pins = b["pins"].to<JsonArray>();
  for (size_t i = 0; i < GpioMap::allowedCount(); i++) {
    pins.add((int)GpioMap::allowedPin(i));
  }
  String out;
  serializeJson(d, out);
  return out;
}

void registerRoutes() {
  // ---- Public: dashboard (works without PIN, so family can switch things when MQTT or iTag fails)
  server.on("/api/login", HTTP_POST, [](AsyncWebServerRequest* req) {
    RequestBodyGuard bodyGuard{req};
    if (lockoutUntilMs && millis() < lockoutUntilMs) {
      sendError(req, 429, "locked out, try again later");
      return;
    }
    if (!bodyUsable(req)) return;
    JsonDocument in;
    if (deserializeJson(in, requestBody(req)->data) || !in["pin"].is<const char*>()) {
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
    sendJson(req, 200, statusJson());
  });

  server.on("/api/relay", HTTP_POST, [](AsyncWebServerRequest* req) {
    RequestBodyGuard bodyGuard{req};
    if (!bodyUsable(req)) return;
    JsonDocument in;
    if (deserializeJson(in, requestBody(req)->data)) return sendError(req, 400, "bad json");
    int idx = in["index"] | 0;
    if (idx < 1 || idx > MAX_RELAYS || !in["on"].is<bool>()) return sendError(req, 400, "bad relay");
    if (!Relay::set(idx - 1, in["on"].as<bool>())) return sendError(req, 409, "relay not enabled");
    sendJson(req, 200, "{\"ok\":true}");
  }, nullptr, bodyChunk);

  server.on("/api/fan", HTTP_POST, [](AsyncWebServerRequest* req) {
    RequestBodyGuard bodyGuard{req};
    if (!bodyUsable(req)) return;
    JsonDocument in;
    if (deserializeJson(in, requestBody(req)->data)) return sendError(req, 400, "bad json");
    int sp = in["speed"] | -1;
    if (sp < 0 || sp > 3) return sendError(req, 400, "speed must be 0..3");
    if (!Fan::setSpeed(sp)) return sendError(req, 409, "fan not enabled or speed above max");
    sendJson(req, 200, "{\"ok\":true}");
  }, nullptr, bodyChunk);

  // ---- PIN protected: config, factory reset, iTag tools
  server.on("/api/config", HTTP_GET, [](AsyncWebServerRequest* req) {
    REQUIRE_AUTH(req);
    sendJson(req, 200, configJson());
  });

  server.on("/api/config", HTTP_POST, [](AsyncWebServerRequest* req) {
    RequestBodyGuard bodyGuard{req};
    REQUIRE_AUTH(req);
    if (!bodyUsable(req)) return;
    AppConfig next = Config::get();
    String err;
    if (!Config::fromJson(requestBody(req)->data, next, err)) {
      Serial.printf("[web] config rejected by fromJson: %s\n", err.c_str());
      return sendError(req, 400, err.c_str());
    }

    std::vector<String> warnings;
    if (!GpioMap::validate(next, err, warnings)) {
      Serial.printf("[web] config rejected by GPIO check: %s\n", err.c_str());
      return sendError(req, 400, err.c_str());
    }
    if (!Config::save(next)) {
      Serial.println("[web] config save FAILED");
      return sendError(req, 500, "save failed, config not changed");
    }
    Serial.printf("[web] config saved, ssid='%s', restart in 1.5 s\n", next.wifi.ssid);

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
