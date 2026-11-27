#include "gpio_map.h"

namespace {
constexpr int8_t BOARD_PINS[] = {4, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33};
constexpr size_t BOARD_PIN_COUNT = sizeof(BOARD_PINS) / sizeof(BOARD_PINS[0]);
constexpr const char* BOARD_NAME = "Wemos D1 mini ESP32";

bool onBoard(int8_t gpio) {
  for (size_t i = 0; i < BOARD_PIN_COUNT; i++) if (BOARD_PINS[i] == gpio) return true;
  return false;
}
bool claim(std::vector<std::pair<int8_t, String>>& used, int8_t gpio, const String& who,
           String& err, bool shareInput = false) {
  if (!onBoard(gpio)) {
    err = who + ": GPIO " + String((int)gpio) + " is not available on this board";
    return false;
  }
  for (const auto& item : used) {
    if (item.first != gpio) continue;
    if (shareInput && item.second == "PIR input") return true;
    err = who + ": GPIO " + String((int)gpio) + " conflicts with " + item.second;
    return false;
  }
  used.push_back({gpio, who});
  return true;
}
bool validTime(const char* value) {
  if (strlen(value) != 5 || value[2] != ':') return false;
  if (value[0] < '0' || value[0] > '2' || value[1] < '0' || value[1] > '9' ||
      value[3] < '0' || value[3] > '5' || value[4] < '0' || value[4] > '9') return false;
  const int hour = (value[0] - '0') * 10 + value[1] - '0';
  return hour < 24;
}
}

namespace GpioMap {
const char* boardName() { return BOARD_NAME; }
size_t allowedCount() { return BOARD_PIN_COUNT; }
int8_t allowedPin(size_t index) { return index < BOARD_PIN_COUNT ? BOARD_PINS[index] : -1; }
bool isAllowed(int8_t gpio) { return onBoard(gpio); }

bool validate(const AppConfig& cfg, String& err, std::vector<String>& warnings) {
  warnings.clear();
  std::vector<std::pair<int8_t, String>> used;
  for (uint8_t i = 0; i < MAX_RELAYS; i++) {
    const RelayCfg& r = cfg.relays[i];
    if (!r.enabled) continue;
    const String label = "Relay " + String(i + 1);
    if (r.gpio < 0 || !claim(used, r.gpio, label + " output", err)) return false;
    if (r.buttonGpio >= 0 && !claim(used, r.buttonGpio, label + " button", err)) return false;
    if (r.schedule.enabled && (!validTime(r.schedule.on) || !validTime(r.schedule.off) ||
                               strcmp(r.schedule.on, r.schedule.off) == 0)) {
      err = label + ": schedule requires valid, different HH:MM times";
      return false;
    }
    if (r.humanDetector.enabled) {
      if (r.humanDetector.gpio < 0 ||
          !claim(used, r.humanDetector.gpio, "PIR input", err, true)) return false;
      if (r.humanDetector.holdSec < 1 || r.humanDetector.holdSec > 3600) {
        err = label + ": PIR hold time must be 1..3600 seconds";
        return false;
      }
    }
  }
  if (cfg.fan.enabled) {
    if (cfg.fan.maxSpeed != 2 && cfg.fan.maxSpeed != 3) {
      err = "Fan: max speed must be 2 or 3"; return false;
    }
    for (uint8_t s = 0; s < cfg.fan.maxSpeed; s++) {
      if (cfg.fan.speedGpio[s] < 0 ||
          !claim(used, cfg.fan.speedGpio[s], "Fan speed " + String(s + 1), err)) return false;
    }
  }
  if (cfg.sensor.type != SensorType::None) {
    if (cfg.sensor.gpio < 0 || !claim(used, cfg.sensor.gpio, "Temperature sensor", err)) return false;
  }
  return true;
}
}