#pragma once
#include <Arduino.h>
#include <vector>
#include "config.h"

namespace GpioMap {

// Validate a whole configuration. Returns false and fills `err` on the first hard error.
// Soft issues (strapping pins) are appended to `warnings`.
bool validate(const AppConfig& cfg, String& err, std::vector<String>& warnings);

// Pins that can never be used for any role on classic ESP32.
bool isForbidden(int8_t gpio);

// Pins that cannot drive an output (input-only).
bool isInputOnly(int8_t gpio);

}  // namespace GpioMap
