#pragma once
#include <Arduino.h>

namespace Presence {
void begin();
void tick();
bool detected(uint8_t relayIndex);
}
