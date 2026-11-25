#include "gpio_map.h"

namespace {
// Wemos D1 mini ESP32 (ESP-WROOM-32). Usable pins for relays, buttons, fan and sensor.
// Excluded on purpose:
//   0        factory reset (hold to GND)
//   1, 3     serial TX/RX
//   2,5,12,13,14,15  strapping / JTAG
//   6-11     internal SPI flash
//   34-39    input-only, no internal pull-up (cannot be a button)
constexpr int8_t BOARD_PINS[] = {4, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33};
constexpr size_t BOARD_PIN_COUNT = sizeof(BOARD_PINS) / sizeof(BOARD_PINS[0]);
constexpr const char* BOARD_NAME = "Wemos D1 mini ESP32";

bool onBoard(int8_t gpio) {
  for (size_t i = 0; i < BOARD_PIN_COUNT; i++) {
    if (BOARD_PINS[i] == gpio) return true;
  }
  return false;
}
}  // namespace

namespace GpioMap {

const char* boardName() { return BOARD_NAME; }
size_t allowedCount() { return BOARD_PIN_COUNT; }
int8_t allowedPin(size_t index) { return BOARD_PINS[index]; }
bool isAllowed(int8_t gpio) { return onBoard(gpio); }

bool validate(const AppConfig& cfg, String& err, std::vector<String>& warnings) {
  (void)warnings;
  struct Claim { int8_t gpio; String who; };
  std::vector<Claim> used;

  // Every assigned pin must be on the board list and used by at most one role.
  auto claim = [&](int8_t gpio, const String& who) -> bool {
    if (!onBoard(gpio)) {
      err = who + ": GPIO " + String((int)gpio) + " is not available on this board";
      return false;
    }
    for (auto& c : used) {
      if (c.gpio == gpio) {
        err = who + ": GPIO " + String((int)gpio) + " already used by " + c.who;
        return false;
      }
    }
    used.push_back({gpio, who});
    return true;
  };

  // Relays: output plus mandatory button (both must be set when enabled)
  for (uint8_t i = 0; i < MAX_RELAYS; i++) {
    const RelayCfg& r = cfg.relays[i];
    if (!r.enabled) continue;
    String who = "Relay " + String(i + 1);
    if (r.gpio < 0) { err = who + ": output GPIO is not set"; return false; }
    if (r.buttonGpio < 0) { err = who + ": button GPIO is not set"; return false; }
    if (!claim(r.gpio, who + " output")) return false;
    if (!claim(r.buttonGpio, who + " button")) return false;
  }

  // Fan: one output per speed, count must match max speed
  if (cfg.fan.enabled) {
    if (cfg.fan.maxSpeed != 2 && cfg.fan.maxSpeed != 3) { err = "Fan: max speed must be 2 or 3"; return false; }
    for (uint8_t s = 0; s < cfg.fan.maxSpeed; s++) {
      String who = "Fan speed " + String(s + 1);
      if (cfg.fan.speedGpio[s] < 0) { err = who + ": GPIO is not set"; return false; }
      if (!claim(cfg.fan.speedGpio[s], who)) return false;
    }
  }

  // Sensor
  if (cfg.sensor.type != SensorType::None) {
    if (cfg.sensor.gpio < 0) { err = "Sensor: GPIO is not set"; return false; }
    if (!claim(cfg.sensor.gpio, "Sensor")) return false;
  }

  return true;
}

}  // namespace GpioMap
