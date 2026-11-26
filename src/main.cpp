// ESP32 Home Controller
// Boot order: config -> GPIO validation -> outputs OFF -> WiFi (STA, AP fallback)
//             -> NTP -> MQTT (WSS, bundle verification) -> WebUI -> iTag / sensor.
#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <time.h>
#include "config.h"
#include "gpio_map.h"
#include "relay.h"
#include "fan.h"
#include "sensor.h"
#include "itag.h"
#include "mqtt.h"
#include "web.h"

namespace {
constexpr uint32_t WIFI_TIMEOUT_MS = 60000;
constexpr uint32_t NTP_TIMEOUT_MS = 30000;
constexpr uint32_t FACTORY_RESET_HOLD_MS = 5000;
constexpr uint8_t FACTORY_RESET_PIN = 0;   // BOOT button on DevKit boards

enum class NetState { Connecting, Ap, Online };
NetState net = NetState::Connecting;
uint32_t wifiStartMs = 0;
uint32_t timeWaitStartMs = 0;
bool timeSynced = false;
bool mqttStarted = false;
bool safeMode = false;
uint32_t resetHoldStart = 0;

void logChipInfo() {
  esp_chip_info_t info;
  esp_chip_info(&info);
  Serial.printf("Chip: %s rev %d, cores %d\n", ESP.getChipModel(), info.revision, info.cores);
  Serial.printf("Flash: %u bytes, PSRAM: %s\n", ESP.getFlashChipSize(), psramFound() ? "yes" : "no");
  if (psramFound()) {
    Serial.println("WARNING: PSRAM detected (WROVER). GPIO16/17 may be in use by PSRAM.");
  }
  if (ESP.getFlashChipSize() < 4UL * 1024 * 1024) {
    Serial.println("ERROR: flash smaller than 4 MB, partition table will not fit.");
  }
}

void startAp() {
  // Open AP is never allowed. Password comes from config (default 11111111).
  const AppConfig& cfg = Config::get();
  const char* pw = strlen(cfg.apPass) >= 8 ? cfg.apPass : "11111111";
  String ssid = String("ESP32-Setup-") + String((uint32_t)(ESP.getEfuseMac() & 0xFFFF), HEX);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssid.c_str(), pw);
  net = NetState::Ap;
  Serial.printf("AP mode: SSID %s, open http://192.168.4.1\n", ssid.c_str());
}

void startWifi() {
  const AppConfig& cfg = Config::get();
  if (cfg.wifi.ssid[0] == '\0') {
    Serial.println("No WiFi configured, starting AP.");
    startAp();
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(cfg.hostname);
  WiFi.setAutoReconnect(true);
  WiFi.begin(cfg.wifi.ssid, cfg.wifi.pass);
  wifiStartMs = millis();
  net = NetState::Connecting;
  Serial.printf("WiFi: connecting to %s\n", cfg.wifi.ssid);
}

void onRelayChange(uint8_t index, bool on) {
  Mqtt::publishRelay(index, on);
}

void onFanChange(uint8_t speed) {
  Mqtt::publishFan(speed);
}

void onSensor(float t, float h, bool hasH) {
  Mqtt::publishSensor(t, h, hasH);
}

void onClicks(uint8_t clicks) {
  Serial.printf("iTag clicks: %u\n", clicks);
  Fan::handleClicks(clicks);
}

void checkFactoryReset() {
  // hold BOOT (GPIO0, active LOW) for 5 seconds
  if (digitalRead(FACTORY_RESET_PIN) == LOW) {
    if (resetHoldStart == 0) resetHoldStart = millis();
    if (millis() - resetHoldStart >= FACTORY_RESET_HOLD_MS) {
      Serial.println("Factory reset: clearing config.");
      if (!Config::resetToDefaults()) {
        Serial.println("Factory reset failed; configuration was not changed.");
        resetHoldStart = 0;
        return;
      }
      delay(200);
      ESP.restart();
    }
  } else {
    resetHoldStart = 0;
  }
}

void tickNetwork() {
  if (net == NetState::Connecting) {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("WiFi connected, IP %s\n", WiFi.localIP().toString().c_str());
      net = NetState::Online;
      configTzTime("WIB-7", "pool.ntp.org", "time.google.com");
      timeWaitStartMs = millis();
    } else if (millis() - wifiStartMs > WIFI_TIMEOUT_MS) {
      Serial.println("WiFi timeout, falling back to AP.");
      WiFi.disconnect(true);
      startAp();
    }
  }

  // Start MQTT once the clock is valid. TLS certificate validity needs correct time.
  if (net == NetState::Online && !mqttStarted) {
    time_t now = time(nullptr);
    if (now > 1700000000) {  // after Nov 2023
      if (!timeSynced) {
        timeSynced = true;
        Serial.println("NTP synced.");
      }
      Mqtt::begin();
      mqttStarted = true;
    } else if (millis() - timeWaitStartMs > NTP_TIMEOUT_MS) {
      // keep waiting, but do not block forever; retry NTP
      configTzTime("WIB-7", "pool.ntp.org", "time.google.com");
      timeWaitStartMs = millis();
    }
  }
}
}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nESP32 Home Controller starting");
  logChipInfo();

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed, WebUI unavailable.");
  }

  pinMode(FACTORY_RESET_PIN, INPUT_PULLUP);

  Config::begin();

  // Safety: validate the GPIO map before touching any pin.
  String err;
  std::vector<String> warnings;
  if (!GpioMap::validate(Config::get(), err, warnings)) {
    Serial.printf("GPIO config invalid, running in safe mode: %s\n", err.c_str());
    safeMode = true;
  }
  for (auto& w : warnings) Serial.printf("GPIO warning: %s\n", w.c_str());

  // Outputs go OFF before anything else. Relay::begin drives every relay to OFF first.
  if (!safeMode) {
    Relay::begin();
    Fan::begin();
    Sensor::begin();
    ITag::begin(onClicks);
  }
  Relay::onChange(onRelayChange);
  Fan::onChange(onFanChange);
  Sensor::onReading(onSensor);

  if (Config::get().pin[0] && strcmp(Config::get().pin, "123456") == 0) {
    Serial.println("WARNING: default WebUI PIN 123456 is still in use. Change it in Settings.");
  }

  startWifi();
  if (!Web::begin()) {
    Serial.println("WebUI unavailable: startup synchronization failed.");
  } else {
    Serial.println("WebUI ready.");
  }
}

void loop() {
  checkFactoryReset();
  tickNetwork();

  if (!safeMode) {
    Relay::tick();
    Fan::tick();
    Sensor::tick();
    ITag::tick();
  }
  Web::tick();
  delay(5);
}
