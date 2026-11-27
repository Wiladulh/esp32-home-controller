#pragma once
#include <Arduino.h>
#include <vector>
#include "config.h"

// Board profile. Only these GPIOs are offered in the WebUI and accepted by the firmware.
// Firmware is built for one board model (Wemos D1 mini ESP32); changing the board means
// changing the pin list in gpio_map.cpp and rebuilding.
namespace GpioMap {

const char* boardName();
size_t allowedCount();
int8_t allowedPin(size_t index);
bool isAllowed(int8_t gpio);

// Validate configured GPIO roles. Shared PIR inputs are allowed; conflicting roles are rejected.
// `warnings` is kept for future soft checks.
bool validate(const AppConfig& cfg, String& err, std::vector<String>& warnings);

}  // namespace GpioMap
