#pragma once

#include <time.h>

namespace RelayPolicy {

inline bool shouldClearManualOverride(bool manualOverride, time_t overrideAt,
                                      time_t boundary) {
  return manualOverride && (overrideAt == 0 || overrideAt < boundary);
}

inline bool shouldApplyAutomatic(bool relayEnabled, bool manualOverride,
                                 bool currentState, bool requestedState) {
  return relayEnabled && !manualOverride && currentState != requestedState;
}

}  // namespace RelayPolicy
