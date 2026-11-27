#pragma once
#include <Arduino.h>
#include <functional>

namespace Relay {

using ChangeCb = std::function<void(uint8_t index, bool on)>;

void begin();
void tick();
bool set(uint8_t index, bool on);
bool setAutomatic(uint8_t index, bool on);
void clearManualOverride(uint8_t index);
bool get(uint8_t index);
bool isEnabled(uint8_t index);
void onChange(ChangeCb cb);
void allOff();

}  // namespace Relay
