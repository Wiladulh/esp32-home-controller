#pragma once
#include <Arduino.h>
#include <functional>
#include <time.h>

namespace Relay {

using ChangeCb = std::function<void(uint8_t index, bool on)>;

void begin();
void tick();
bool set(uint8_t index, bool on);
bool setAutomatic(uint8_t index, bool on);
void clearManualOverrideAtBoundary(uint8_t index, time_t boundary);
bool get(uint8_t index);
bool isEnabled(uint8_t index);
void onChange(ChangeCb cb);
void allOff();

}  // namespace Relay
