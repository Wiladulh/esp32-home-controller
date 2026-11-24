#pragma once
#include <Arduino.h>
#include <functional>

namespace Relay {

using ChangeCb = std::function<void(uint8_t index, bool on)>;

// Apply config: sets all relay outputs to OFF first, then configures enabled relays.
void begin();
// Non-blocking. Debounces buttons and toggles relays on press.
void tick();
bool set(uint8_t index, bool on);      // index 0..7
bool get(uint8_t index);
bool isEnabled(uint8_t index);
void onChange(ChangeCb cb);
void allOff();

}  // namespace Relay
