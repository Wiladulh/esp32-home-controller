#pragma once
#include <Arduino.h>
#include <functional>

namespace Fan {

using ChangeCb = std::function<void(uint8_t speed)>;

void begin();                       // configure relay outputs from config, all OFF
void tick();                        // completes pending speed transitions (non-blocking)
bool setSpeed(uint8_t speed);       // 0 = OFF, 1..maxSpeed
bool setSpeedIfCurrent(uint8_t expectedSpeed, uint8_t targetSpeed);
uint8_t getSpeed();
uint8_t maxSpeed();
bool isEnabled();
void onChange(ChangeCb cb);

// iTag click handling: 1x = on/off, 2x = next speed (wraps to 1), 3x+ = ignored
void handleClicks(uint8_t clicks);

}  // namespace Fan
