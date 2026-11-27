#include "relay.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "config.h"
#include "relay_policy.h"
#include <time.h>

namespace {
constexpr uint32_t DEBOUNCE_MS = 40;

struct State {
  bool on = false;
  bool enabled = false;
  bool manualOverride = false;
  time_t manualOverrideAt = 0;
  int8_t gpio = -1;
  int8_t button = -1;
  bool activeHigh = true;
  bool lastRaw = true;
  bool stable = true;
  uint32_t lastChangeMs = 0;
};

State st[MAX_RELAYS];
Relay::ChangeCb changeCb;
SemaphoreHandle_t stateMutex = nullptr;

void drive(State& s, bool on) {
  s.on = on;
  if (s.gpio < 0) return;
  const bool level = (on == s.activeHigh);
  digitalWrite(s.gpio, level ? HIGH : LOW);
}
}  // namespace

namespace Relay {

void begin() {
  stateMutex = xSemaphoreCreateMutex();
  if (!stateMutex) return;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const AppConfig& cfg = Config::get();
  for (uint8_t i = 0; i < MAX_RELAYS; i++) {
    State& s = st[i];
    const RelayCfg& r = cfg.relays[i];
    s = State{};
    s.enabled = r.enabled;
    s.gpio = r.enabled ? r.gpio : -1;
    s.button = r.enabled ? r.buttonGpio : -1;
    s.activeHigh = r.activeHigh;
    if (s.gpio >= 0) {
      pinMode(s.gpio, OUTPUT);
      digitalWrite(s.gpio, s.activeHigh ? LOW : HIGH);
      s.on = false;
    }
    if (s.button >= 0) {
      pinMode(s.button, INPUT_PULLUP);
      s.lastRaw = digitalRead(s.button);
      s.stable = s.lastRaw;
    }
  }
  xSemaphoreGive(stateMutex);
}

void tick() {
  if (!stateMutex) return;
  const uint32_t now = millis();
  for (uint8_t i = 0; i < MAX_RELAYS; i++) {
    ChangeCb cb;
    bool changed = false;
    bool newState = false;
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    State& state = st[i];
    if (state.enabled && state.button >= 0) {
      const bool raw = digitalRead(state.button);
      if (raw != state.lastRaw) {
        state.lastRaw = raw;
        state.lastChangeMs = now;
      }
      if (raw != state.stable && now - state.lastChangeMs >= DEBOUNCE_MS) {
        state.stable = raw;
        if (state.stable == LOW) {
          newState = !state.on;
          state.manualOverride = true;
          state.manualOverrideAt = time(nullptr);
          drive(state, newState);
          cb = changeCb;
          changed = true;
        }
      }
    }
    xSemaphoreGive(stateMutex);
    if (changed && cb) cb(i, newState);
  }
}

bool set(uint8_t index, bool on) {
  if (index >= MAX_RELAYS || !stateMutex) return false;
  ChangeCb cb;
  bool changed = false;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  if (st[index].enabled) {
    st[index].manualOverride = true;
    st[index].manualOverrideAt = time(nullptr);
    changed = st[index].on != on;
    if (changed) {
      drive(st[index], on);
      cb = changeCb;
    }
  }
  const bool enabled = st[index].enabled;
  xSemaphoreGive(stateMutex);
  if (changed && cb) cb(index, on);
  return enabled;
}

bool setAutomatic(uint8_t index, bool on) {
  if (index >= MAX_RELAYS || !stateMutex) return false;
  ChangeCb cb;
  bool changed = false;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  if (st[index].enabled && !st[index].manualOverride) {
    changed = st[index].on != on;
    if (changed) {
      drive(st[index], on);
      cb = changeCb;
    }
  }
  const bool enabled = st[index].enabled;
  xSemaphoreGive(stateMutex);
  if (changed && cb) cb(index, on);
  return enabled;
}

void clearManualOverrideAtBoundary(uint8_t index, time_t boundary) {
  if (index >= MAX_RELAYS || !stateMutex) return;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  State& state = st[index];
  if (RelayPolicy::shouldClearManualOverride(state.manualOverride,
                                             state.manualOverrideAt, boundary)) {
    state.manualOverride = false;
    state.manualOverrideAt = 0;
  }
  xSemaphoreGive(stateMutex);
}

bool get(uint8_t index) {
  if (index >= MAX_RELAYS || !stateMutex) return false;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const bool value = st[index].on;
  xSemaphoreGive(stateMutex);
  return value;
}

bool isEnabled(uint8_t index) {
  if (index >= MAX_RELAYS || !stateMutex) return false;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const bool value = st[index].enabled;
  xSemaphoreGive(stateMutex);
  return value;
}

void onChange(ChangeCb cb) {
  if (!stateMutex) return;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  changeCb = cb;
  xSemaphoreGive(stateMutex);
}

void allOff() {
  for (uint8_t i = 0; i < MAX_RELAYS; i++) set(i, false);
}

}  // namespace Relay
