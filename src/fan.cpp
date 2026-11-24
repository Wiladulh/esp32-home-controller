#include "fan.h"
#include "config.h"

namespace {
constexpr uint32_t TRANSITION_MS = 500;  // dead time between relays

bool enabled = false;
uint8_t speed = 0;
uint8_t maxSpd = 3;
int8_t pins[FAN_MAX_SPEEDS] = {-1, -1, -1};
bool activeHigh = true;

bool pendingActive = false;
uint8_t pendingSpeed = 0;
uint32_t pendingAtMs = 0;

Fan::ChangeCb changeCb;

void writeRelay(uint8_t s, bool on) {  // s = 1..maxSpd
  int8_t p = pins[s - 1];
  if (p < 0) return;
  digitalWrite(p, (on == activeHigh) ? HIGH : LOW);
}

void allSpeedOff() {
  for (uint8_t s = 1; s <= maxSpd; s++) writeRelay(s, false);
}
}  // namespace

namespace Fan {

void begin() {
  const FanCfg& f = Config::get().fan;
  enabled = f.enabled;
  maxSpd = f.maxSpeed;
  activeHigh = f.activeHigh;
  for (uint8_t i = 0; i < FAN_MAX_SPEEDS; i++) {
    pins[i] = enabled ? f.speedGpio[i] : -1;
    if (pins[i] >= 0) {
      digitalWrite(pins[i], activeHigh ? LOW : HIGH);
      pinMode(pins[i], OUTPUT);
      digitalWrite(pins[i], activeHigh ? LOW : HIGH);
    }
  }
  speed = 0;
  pendingActive = false;
}

void tick() {
  if (pendingActive && millis() - pendingAtMs >= TRANSITION_MS) {
    pendingActive = false;
    if (pendingSpeed > 0) writeRelay(pendingSpeed, true);
    speed = pendingSpeed;
    if (changeCb) changeCb(speed);
  }
}

bool setSpeed(uint8_t target) {
  if (!enabled || target > maxSpd) return false;
  if (target == speed && !pendingActive) return true;

  // Interlock: turn everything off first, then wait before energising the new speed
  allSpeedOff();
  speed = 0;
  pendingSpeed = target;
  pendingActive = true;
  pendingAtMs = millis();
  if (target == 0) {
    pendingActive = false;
    if (changeCb) changeCb(0);
  }
  return true;
}

uint8_t getSpeed() { return speed; }
uint8_t maxSpeed() { return maxSpd; }
bool isEnabled() { return enabled; }
void onChange(ChangeCb cb) { changeCb = cb; }

void handleClicks(uint8_t clicks) {
  if (!enabled) return;
  if (clicks == 1) {
    setSpeed(speed > 0 || pendingActive ? 0 : 1);
  } else if (clicks == 2) {
    uint8_t current = pendingActive ? pendingSpeed : speed;
    if (current == 0) return;  // double-click only changes speed while ON
    uint8_t next = current >= maxSpd ? 1 : current + 1;
    setSpeed(next);
  }
  // 3+ clicks: no action
}

}  // namespace Fan
