#include "web.h"
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <atomic>
#include <new>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
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
SemaphoreHandle_t authMutex = nullptr;

uint8_t failCount = 0;
uint32_t lockoutUntilMs = 0;

std::atomic<uint32_t> restartAt{0};
std::atomic<bool> restartPending{false};

String randomToken() {
  char out[33];
  for (int i = 0; i < 4; i++) {
    snprintf(out + i * 8, 9, "%08lx", (unsigned long)esp_random());
  }
  return String(out);
}

// Only the config (PIN-protected) endpoints check the session.
bool sessionValid(AsyncWebServerRequest* req) {
  if (!authMutex || !req->hasHeader("Authorization")) return false;
  String header = req->header("Authorization");
  if (!header.startsWith("Bearer ")) return false;
  const String supplied = header.substring(7);

  xSemaphoreTake(authMutex, portMAX_DELAY);
  bool valid = session.token.length() > 0 && supplied == session.token;
  if (valid && millis() - session.lastMs > SESSION_IDLE_MS) {
    session.token = "";
    valid = false;
  }
  if (valid) session.lastMs = millis();
  xSemaphoreGive(authMutex);
  return valid;
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

SemaphoreHandle_t bodyMutex = nullptr;

RequestBody* requestBody(AsyncWebServerRequest* req) {
  return static_cast<RequestBody*>(req->_tempObject);
}

RequestBody* takeRequestBody(AsyncWebServerRequest* req) {
  if (!bodyMutex) return nullptr;
  xSemaphoreTake(bodyMutex, portMAX_DELAY);
  auto* body = requestBody(req);
  req->_tempObject = nullptr;
  xSemaphoreGive(bodyMutex);
  return body;
}

void releaseRequestBody(AsyncWebServerRequest* req) {
  auto* body = takeRequestBody(req);
  if (!body) return;
  delete body;
  activeBodies.fetch_sub(1, std::memory_order_acq_rel);
}

struct RequestBodyGuard {
  RequestBody* body;
  ~RequestBodyGuard() {
    if (!body) return;
    delete body;
    activeBodies.fetch_sub(1, std::memory_order_acq_rel);
  }
};

enum class ConfigJobState : uint8_t { Idle, Queued, Processing, Succeeded, Failed };
enum class ConfigJobType : uint8_t { Save, FactoryReset };

struct ConfigJob {
  char body[MAX_BODY + 1]{};
  char error[128]{};
  ConfigJobType type = ConfigJobType::Save;
  ConfigJobState state = ConfigJobState::Idle;
};

ConfigJob configJob;
SemaphoreHandle_t configJobMutex = nullptr;

bool queueConfigSave(const RequestBody* body) {
  if (!configJobMutex || !body) return false;
  xSemaphoreTake(configJobMutex, portMAX_DELAY);
  if (configJob.state == ConfigJobState::Queued || configJob.state == ConfigJobState::Processing ||
      configJob.state == ConfigJobState::Succeeded) {
    xSemaphoreGive(configJobMutex);
    return false;
  }
  memcpy(configJob.body, body->data, body->length + 1);
  configJob.type = ConfigJobType::Save;
  configJob.error[0] = '\0';
  configJob.state = ConfigJobState::Queued;
  xSemaphoreGive(configJobMutex);
  return true;
}

bool queueFactoryReset() {
  if (!configJobMutex) return false;
  xSemaphoreTake(configJobMutex, portMAX_DELAY);
  if (configJob.state == ConfigJobState::Queued || configJob.state == ConfigJobState::Processing ||
      configJob.state == ConfigJobState::Succeeded) {
    xSemaphoreGive(configJobMutex);
    return false;
  }
  configJob.type = ConfigJobType::FactoryReset;
  configJob.body[0] = '\0';
  configJob.error[0] = '\0';
  configJob.state = ConfigJobState::Queued;
  xSemaphoreGive(configJobMutex);
  return true;
}

void bodyChunk(AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
  if (!bodyMutex) return;
  if (index == 0) {
    if (activeBodies.fetch_add(1, std::memory_order_acq_rel) >= MAX_ACTIVE_BODIES) {
      activeBodies.fetch_sub(1, std::memory_order_acq_rel);
      return;
    }
    auto* state = new (std::nothrow) RequestBody();
    if (!state) {
      activeBodies.fetch_sub(1, std::memory_order_acq_rel);
      return;
    }
    state->tooLarge = total > MAX_BODY;
    xSemaphoreTake(bodyMutex, portMAX_DELAY);
    req->_tempObject = state;
    req->onDisconnect([req]() { releaseRequestBody(req); });
    xSemaphoreGive(bodyMutex);
    Serial.printf("[web] body start, total=%u, heap=%u\n", (unsigned)total, (unsigned)ESP.getFreeHeap());
  }

  xSemaphoreTake(bodyMutex, portMAX_DELAY);
  auto* state = requestBody(req);
  if (state && !state->tooLarge) {
    if (index != state->length || state->length + len > MAX_BODY) {
      state->tooLarge = true;
    } else {
      memcpy(state->data + state->length, data, len);
      state->length += len;
      state->data[state->length] = '\0';
    }
  }
  xSemaphoreGive(bodyMutex);
}

// POST bodies must be application/json. A cross-site form or text/plain request cannot
// send that type without a CORS preflight, and this device never answers preflights,
// so other websites cannot drive the public relay and fan endpoints.
bool bodyUsable(AsyncWebServerRequest* req, const RequestBody* body) {
  const size_t received = body ? body->length : 0;
  Serial.printf("[web] %s content-length=%u received=%u\n", req->url().c_str(),
                (unsigned)req->contentLength(), (unsigned)received);
  String contentType = req->header("Content-Type");
  const int parameterStart = contentType.indexOf(';');
  if (parameterStart >= 0) contentType = contentType.substring(0, parameterStart);
  contentType.trim();
  if (!req->hasHeader("Content-Type") || !contentType.equalsIgnoreCase("application/json")) {
    sendError(req, 415, "content-type must be application/json");
    return false;
  }
  if (!body) {
    sendError(req, req->contentLength() == 0 ? 400 : 503,
              req->contentLength() == 0 ? "empty request body" : "request body capacity unavailable");
    return false;
  }
  if (body->tooLarge) {
    sendError(req, 413, "body too large");
    return false;
  }
  if (received == 0 || req->contentLength() != received) {
    sendError(req, 400, received == 0 ? "empty request body" : "incomplete body");
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
    RequestBodyGuard bodyGuard{takeRequestBody(req)};
    if (!bodyUsable(req, bodyGuard.body)) return;
    JsonDocument in;
    if (deserializeJson(in, bodyGuard.body->data) || !in["pin"].is<const char*>()) {
      sendError(req, 400, "bad request");
      return;
    }

    const AppConfig cfg = Config::get();
    int errorCode = 0;
    const char* errorMessage = nullptr;
    String token;
    xSemaphoreTake(authMutex, portMAX_DELAY);
    const uint32_t now = millis();
    if (lockoutUntilMs && static_cast<int32_t>(now - lockoutUntilMs) < 0) {
      errorCode = 429;
      errorMessage = "locked out, try again later";
    } else if (lockoutUntilMs) {
      lockoutUntilMs = 0;
    }
    if (!errorCode && strcmp(in["pin"], cfg.pin) != 0) {
      ++failCount;
      if (failCount >= MAX_FAILS) {
        lockoutUntilMs = now + LOCKOUT_MS;
        failCount = 0;
        errorCode = 429;
        errorMessage = "too many attempts, locked for 5 minutes";
      } else {
        errorCode = 401;
        errorMessage = "wrong pin";
      }
    } else {
      failCount = 0;
      session.token = randomToken();
      session.lastMs = now;
      token = session.token;
    }
    xSemaphoreGive(authMutex);

    if (errorCode) {
      sendError(req, errorCode, errorMessage);
      return;
    }
    JsonDocument out;
    out["token"] = token;
    String response;
    serializeJson(out, response);
    sendJson(req, 200, response);
  }, nullptr, bodyChunk);

  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* req) {
    sendJson(req, 200, statusJson());
  });

  server.on("/api/relay", HTTP_POST, [](AsyncWebServerRequest* req) {
    RequestBodyGuard bodyGuard{takeRequestBody(req)};
    if (!bodyUsable(req, bodyGuard.body)) return;
    JsonDocument in;
    if (deserializeJson(in, bodyGuard.body->data)) return sendError(req, 400, "bad json");
    int idx = in["index"] | 0;
    if (idx < 1 || idx > MAX_RELAYS || !in["on"].is<bool>()) return sendError(req, 400, "bad relay");
    if (!Relay::set(idx - 1, in["on"].as<bool>())) return sendError(req, 409, "relay not enabled");
    sendJson(req, 200, "{\"ok\":true}");
  }, nullptr, bodyChunk);

  server.on("/api/fan", HTTP_POST, [](AsyncWebServerRequest* req) {
    RequestBodyGuard bodyGuard{takeRequestBody(req)};
    if (!bodyUsable(req, bodyGuard.body)) return;
    JsonDocument in;
    if (deserializeJson(in, bodyGuard.body->data)) return sendError(req, 400, "bad json");
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
    RequestBodyGuard bodyGuard{takeRequestBody(req)};
    REQUIRE_AUTH(req);
    if (!bodyUsable(req, bodyGuard.body)) return;
    if (!queueConfigSave(bodyGuard.body)) {
      sendError(req, 503, "configuration save queue busy");
      return;
    }
    sendJson(req, 202, "{\"queued\":true}");
  }, nullptr, bodyChunk);

  server.on("/api/config/status", HTTP_GET, [](AsyncWebServerRequest* req) {
    REQUIRE_AUTH(req);
    if (!configJobMutex) return sendError(req, 503, "configuration worker unavailable");
    xSemaphoreTake(configJobMutex, portMAX_DELAY);
    const char* state = "idle";
    switch (configJob.state) {
      case ConfigJobState::Queued: state = "queued"; break;
      case ConfigJobState::Processing: state = "processing"; break;
      case ConfigJobState::Succeeded: state = "succeeded"; break;
      case ConfigJobState::Failed: state = "failed"; break;
      default: break;
    }
    JsonDocument out;
    out["state"] = state;
    if (configJob.state == ConfigJobState::Failed) out["error"] = configJob.error;
    String response;
    serializeJson(out, response);
    xSemaphoreGive(configJobMutex);
    sendJson(req, 200, response);
  });

  server.on("/api/factory-reset", HTTP_POST, [](AsyncWebServerRequest* req) {
    REQUIRE_AUTH(req);
    if (!queueFactoryReset()) {
      sendError(req, 503, "configuration save queue busy");
      return;
    }
    sendJson(req, 202, "{\"queued\":true}");
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
void processConfigJob() {
  if (!configJobMutex || xSemaphoreTake(configJobMutex, 0) != pdTRUE) return;
  if (configJob.state != ConfigJobState::Queued) {
    xSemaphoreGive(configJobMutex);
    return;
  }
  const ConfigJobType type = configJob.type;
  configJob.state = ConfigJobState::Processing;
  xSemaphoreGive(configJobMutex);

  String err;
  bool saved = false;
  if (type == ConfigJobType::FactoryReset) {
    saved = Config::resetToDefaults();
    if (!saved) err = "factory reset failed";
  } else {
    AppConfig next = Config::get();
    if (!Config::fromJson(configJob.body, next, err)) {
      saved = false;
    } else {
      std::vector<String> warnings;
      if (!GpioMap::validate(next, err, warnings)) {
        saved = false;
      } else {
        saved = Config::save(next);
        if (!saved) err = "save failed, config not changed";
      }
    }
  }

  xSemaphoreTake(configJobMutex, portMAX_DELAY);
  if (saved) {
    configJob.state = ConfigJobState::Succeeded;
  configJob.error[0] = '\0';
  } else {
    configJob.state = ConfigJobState::Failed;
    strlcpy(configJob.error, err.c_str(), sizeof(configJob.error));
  }
  xSemaphoreGive(configJobMutex);
  if (saved) Web::requestRestart(3000);
}

}  // namespace

namespace Web {

bool begin() {
  bodyMutex = xSemaphoreCreateMutex();
  configJobMutex = xSemaphoreCreateMutex();
  authMutex = xSemaphoreCreateMutex();
  if (!bodyMutex || !configJobMutex || !authMutex) {
    Serial.println("[web] failed to create required mutexes; server not started");
    return false;
  }
  registerRoutes();
  server.begin();
  return true;
}

void requestRestart(uint32_t delayMs) {
  restartAt.store(millis() + delayMs, std::memory_order_relaxed);
  restartPending.store(true, std::memory_order_release);
}

void tick() {
  processConfigJob();
  if (restartPending.load(std::memory_order_acquire) &&
      static_cast<int32_t>(millis() - restartAt.load(std::memory_order_relaxed)) >= 0) {
    bool expected = true;
    if (restartPending.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
      ESP.restart();
    }
  }
}

}  // namespace Web
