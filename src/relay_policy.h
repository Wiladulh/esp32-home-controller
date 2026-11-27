#pragma once

#include <time.h>

namespace RelayPolicy {

inline bool shouldClearManualOverride(bool manualOverride, time_t overrideAt,
                                      time_t boundary) {
  return manualOverride && (overrideAt == 0 || overrideAt < boundary);
}

}  // namespace RelayPolicy
