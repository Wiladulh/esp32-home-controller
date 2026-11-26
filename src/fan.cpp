#include "fan.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
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
SemaphoreHandle_t stateMutex = nullptr;

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
  stateMutex = xSemaphoreCreateMutex();
  if (!stateMutex) return;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
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
  xSemaphoreGive(stateMutex);
}

void tick() {
  if (!stateMutex) return;
  ChangeCb cb;
  uint8_t completedSpeed = 0;
  bool changed = false;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  if (pendingActive && millis() - pendingAtMs >= TRANSITION_MS) {
    pendingActive = false;
    if (pendingSpeed > 0) writeRelay(pendingSpeed, true);
    speed = pendingSpeed;
    completedSpeed = speed;
    cb = changeCb;
    changed = true;
  }
  xSemaphoreGive(stateMutex);
  if (changed && cb) cb(completedSpeed);
}

bool setSpeed(uint8_t target) {
  if (!stateMutex) return false;
  ChangeCb cb;
  bool notify = false;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  if (!enabled || target > maxSpd) {
    xSemaphoreGive(stateMutex);
    return false;
  }
  if (target == speed && !pendingActive) {
    xSemaphoreGive(stateMutex);
    return true;
  }

  allSpeedOff();
  speed = 0;
  pendingSpeed = target;
  pendingActive = target != 0;
  pendingAtMs = millis();
  if (target == 0) {
    cb = changeCb;
    notify = true;
  }
  xSemaphoreGive(stateMutex);
  if (notify && cb) cb(0);
  return true;
}

uint8_t getSpeed() {
  if (!stateMutex) return 0;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const uint8_t value = speed;
  xSemaphoreGive(stateMutex);
  return value;
}
uint8_t maxSpeed() {
  if (!stateMutex) return 0;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const uint8_t value = maxSpd;
  xSemaphoreGive(stateMutex);
  return value;
}
bool isEnabled() {
  if (!stateMutex) return false;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const bool value = enabled;
  xSemaphoreGive(stateMutex);
  return value;
}
void onChange(ChangeCb cb) {
  if (!stateMutex) return;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  changeCb = cb;
  xSemaphoreGive(stateMutex);
}

void handleClicks(uint8_t clicks) {
  if (!stateMutex) return;
  uint8_t target = 0;
  bool shouldChange = false;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  if (enabled && clicks == 1) {
    target = (speed > 0 || pendingActive) ? 0 : 1;
    shouldChange = true;
  } else if (enabled && clicks == 2) {
    const uint8_t current = pendingActive ? pendingSpeed : speed;
    if (current > 0) {
      target = current >= maxSpd ? 1 : current + 1;
      shouldChange = true;
    }
  }
  xSemaphoreGive(stateMutex);
  if (shouldChange) setSpeed(target);
}

}  // namespace Fan
