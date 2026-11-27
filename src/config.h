#pragma once
#include <Arduino.h>

constexpr uint8_t MAX_RELAYS = 8;
constexpr uint8_t FAN_MAX_SPEEDS = 3;
constexpr size_t NAME_LEN = 32;
constexpr size_t HOST_LEN = 64;
constexpr size_t SECRET_LEN = 64;

// Sensor types supported in the GPIO map
enum class SensorType : uint8_t { None = 0, Dht22 = 1, Ds18b20 = 2 };

struct RelayScheduleCfg {
  bool enabled = false;
  char on[6] = "18:00";
  char off[6] = "06:00";
};

struct HumanDetectorCfg {
  bool enabled = false;
  int8_t gpio = -1;
  uint16_t holdSec = 60;
};

struct RelayCfg {
  bool enabled = false;
  char name[NAME_LEN] = "";
  int8_t gpio = -1;
  int8_t buttonGpio = -1;    // optional momentary button
  bool activeHigh = true;
  RelayScheduleCfg schedule;
  HumanDetectorCfg humanDetector;
};

struct FanCfg {
  bool enabled = false;
  char name[NAME_LEN] = "Fan 1";
  uint8_t maxSpeed = 3;      // 2 or 3
  int8_t speedGpio[FAN_MAX_SPEEDS] = {-1, -1, -1};  // speed 1..3 relay pins
  bool activeHigh = true;
};

struct SensorCfg {
  SensorType type = SensorType::None;
  int8_t gpio = -1;
  uint16_t intervalSec = 30;
};

struct MqttCfg {
  char scheme[8] = "wss";    // wss (default, via Cloudflare), ws, mqtts, mqtt
  char host[HOST_LEN] = "mqtt.giebux.my.id";
  uint16_t port = 443;
  char path[HOST_LEN] = "/";
  char user[SECRET_LEN] = "";
  char pass[SECRET_LEN] = "";
  char topicPrefix[HOST_LEN] = "";  // e.g. "home1/"; empty = root
  bool enabled = true;
};

struct WifiCfg {
  char ssid[SECRET_LEN] = "";
  char pass[SECRET_LEN] = "";
};

struct AppConfig {
  char pin[7] = "123456";    // WebUI PIN (default per user request)
  char hostname[NAME_LEN] = "esp32-home";
  char apPass[64] = "11111111";  // softAP password, 8..63 chars (default per user request)
  WifiCfg wifi;
  MqttCfg mqtt;
  RelayCfg relays[MAX_RELAYS];
  FanCfg fan;
  SensorCfg sensor;
  bool itagEnabled = false;
  char itagAddr[18] = "";    // "AA:BB:CC:DD:EE:FF"
  bool caEnabled = false;    // reserved: always false in v1 (bundle TLS by default)
};

namespace Config {
void begin();                          // load from NVS or defaults
AppConfig get();
bool save(const AppConfig& cfg);       // persists to NVS
bool resetToDefaults();                // true only after NVS keys are cleared
String toJson(bool includeSecrets);
bool fromJson(const char* json, AppConfig& out, String& err);
}
