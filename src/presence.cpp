#include "presence.h"
#include "config.h"
#include <string.h>

namespace {
struct RelayPresence {
  int8_t gpio = -1;
  uint16_t holdSec = 60;
  uint32_t lastMotionMs = 0;
  bool active = false;
};
RelayPresence states[MAX_RELAYS];
int8_t initializedPins[MAX_RELAYS];
uint8_t initializedCount = 0;
}

namespace Presence {
void begin() {
  const AppConfig cfg = Config::get();
  initializedCount = 0;
  for (uint8_t i = 0; i < MAX_RELAYS; ++i) {
    states[i] = RelayPresence{};
    const auto& detector = cfg.relays[i].humanDetector;
    if (!cfg.relays[i].enabled || !detector.enabled || detector.gpio < 0) continue;
    states[i].gpio = detector.gpio;
    states[i].holdSec = detector.holdSec;
    bool configured = false;
    for (uint8_t p = 0; p < initializedCount; ++p) {
      if (initializedPins[p] == detector.gpio) configured = true;
    }
    if (!configured) {
      pinMode(detector.gpio, INPUT);
      initializedPins[initializedCount++] = detector.gpio;
    }
  }
}

void tick() {
  const uint32_t now = millis();
  bool pinHigh[MAX_RELAYS] = {};
  for (uint8_t p = 0; p < initializedCount; ++p) {
    pinHigh[p] = digitalRead(initializedPins[p]) == HIGH;
  }
  for (uint8_t i = 0; i < MAX_RELAYS; ++i) {
    auto& state = states[i];
    if (state.gpio < 0) continue;
    bool high = false;
    for (uint8_t p = 0; p < initializedCount; ++p) {
      if (initializedPins[p] == state.gpio) { high = pinHigh[p]; break; }
    }
    if (high) state.lastMotionMs = now;
    state.active = high || (state.lastMotionMs != 0 &&
                            now - state.lastMotionMs < (uint32_t)state.holdSec * 1000UL);
  }
}

bool detected(uint8_t relayIndex) {
  return relayIndex < MAX_RELAYS && states[relayIndex].active;
}
}
