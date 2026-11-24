#include "gpio_map.h"

namespace GpioMap {

// GPIO0 is reserved for the BOOT button (factory reset).
constexpr int8_t RESERVED_RESET_PIN = 0;

bool isForbidden(int8_t gpio) {
  // 6-11: connected to the internal SPI flash
  return gpio >= 6 && gpio <= 11;
}

bool isInputOnly(int8_t gpio) {
  // 34-39: input-only on ESP32 classic
  return gpio >= 34 && gpio <= 39;
}

static bool isStrapping(int8_t gpio) {
  return gpio == 2 || gpio == 5 || gpio == 12 || gpio == 15;
}

bool validate(const AppConfig& cfg, String& err, std::vector<String>& warnings) {
  // Track which GPIO is already claimed and by what
  struct Claim { int8_t gpio; String who; };
  std::vector<Claim> used;

  auto claim = [&](int8_t gpio, const String& who) -> bool {
    if (gpio < 0) return true;
    if (gpio > 39) { err = who + ": GPIO " + (int)gpio + " does not exist"; return false; }
    if (gpio == RESERVED_RESET_PIN) { err = who + ": GPIO0 is reserved for factory reset"; return false; }
    if (isForbidden(gpio)) { err = who + ": GPIO " + (int)gpio + " is connected to flash"; return false; }
    for (auto& c : used) {
      if (c.gpio == gpio) { err = who + ": GPIO " + (int)gpio + " already used by " + c.who; return false; }
    }
    if (isStrapping(gpio)) warnings.push_back(who + ": GPIO " + (int)gpio + " is a strapping pin, check boot behaviour");
    used.push_back({gpio, who});
    return true;
  };

  auto outputPin = [&](int8_t gpio, const String& who) -> bool {
    if (gpio >= 0 && isInputOnly(gpio)) {
      err = who + ": GPIO " + (int)gpio + " is input-only and cannot drive a relay";
      return false;
    }
    return true;
  };

  // Relays and their buttons
  for (uint8_t i = 0; i < MAX_RELAYS; i++) {
    const RelayCfg& r = cfg.relays[i];
    if (!r.enabled) continue;
    String who = "Relay " + String(i + 1);
    if (r.gpio < 0) { err = who + ": output GPIO is not set"; return false; }
    if (!outputPin(r.gpio, who)) return false;
    if (!claim(r.gpio, who + " output")) return false;
    if (r.buttonGpio < 0) { err = who + ": button GPIO is required"; return false; }
    if (!claim(r.buttonGpio, who + " button")) return false;
  }

  // Fan: one relay per speed, count must match maxSpeed
  if (cfg.fan.enabled) {
    if (cfg.fan.maxSpeed != 2 && cfg.fan.maxSpeed != 3) { err = "Fan: maxSpeed must be 2 or 3"; return false; }
    for (uint8_t s = 0; s < cfg.fan.maxSpeed; s++) {
      String who = "Fan speed " + String(s + 1);
      if (cfg.fan.speedGpio[s] < 0) { err = who + ": GPIO is not set"; return false; }
      if (!outputPin(cfg.fan.speedGpio[s], who)) return false;
      if (!claim(cfg.fan.speedGpio[s], who)) return false;
    }
  }

  // Sensor: needs bidirectional pin (not input-only)
  if (cfg.sensor.type != SensorType::None) {
    if (cfg.sensor.gpio < 0) { err = "Sensor: GPIO is not set"; return false; }
    if (isInputOnly(cfg.sensor.gpio)) { err = "Sensor: GPIO " + String(cfg.sensor.gpio) + " is input-only, pick another pin"; return false; }
    if (!claim(cfg.sensor.gpio, "Sensor")) return false;
  }

  return true;
}

}  // namespace GpioMap
