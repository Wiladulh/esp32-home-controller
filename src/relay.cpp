#include "relay.h"
#include "config.h"

namespace {
constexpr uint32_t DEBOUNCE_MS = 40;

struct State {
  bool on = false;
  bool enabled = false;
  int8_t gpio = -1;
  int8_t button = -1;
  bool activeHigh = true;
  bool lastRaw = true;       // INPUT_PULLUP: HIGH = released
  bool stable = true;
  uint32_t lastChangeMs = 0;
};

State st[MAX_RELAYS];
Relay::ChangeCb changeCb;

void drive(State& s, bool on) {
  s.on = on;
  if (s.gpio < 0) return;
  bool level = (on == s.activeHigh);
  digitalWrite(s.gpio, level ? HIGH : LOW);
}
}  // namespace

namespace Relay {

void begin() {
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
      // drive OFF before switching to output to avoid a glitch
      digitalWrite(s.gpio, s.activeHigh ? LOW : HIGH);
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
}

void tick() {
  uint32_t now = millis();
  for (uint8_t i = 0; i < MAX_RELAYS; i++) {
    State& s = st[i];
    if (!s.enabled || s.button < 0) continue;
    bool raw = digitalRead(s.button);
    if (raw != s.lastRaw) {
      s.lastRaw = raw;
      s.lastChangeMs = now;
    }
    if (raw != s.stable && (now - s.lastChangeMs) >= DEBOUNCE_MS) {
      s.stable = raw;
      if (s.stable == LOW) {  // pressed (active low to GND)
        set(i, !s.on);
      }
    }
  }
}

bool set(uint8_t index, bool on) {
  if (index >= MAX_RELAYS || !st[index].enabled) return false;
  if (st[index].on == on) return true;
  drive(st[index], on);
  if (changeCb) changeCb(index, on);
  return true;
}

bool get(uint8_t index) {
  if (index >= MAX_RELAYS) return false;
  return st[index].on;
}

bool isEnabled(uint8_t index) {
  return index < MAX_RELAYS && st[index].enabled;
}

void onChange(ChangeCb cb) { changeCb = cb; }

void allOff() {
  for (uint8_t i = 0; i < MAX_RELAYS; i++) {
    if (st[i].enabled) set(i, false);
  }
}

}  // namespace Relay
