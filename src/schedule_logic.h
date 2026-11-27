#pragma once

#include <time.h>

namespace ScheduleLogic {

struct State {
  bool lastScheduled = false;
  bool scheduledKnown = false;
  bool lastPresence = false;
  bool presenceKnown = false;
};

struct Result {
  bool timeValid = false;
  bool scheduled = false;
  bool presence = false;
  bool automaticOn = false;
  bool shouldApply = false;
  bool boundary = false;
  time_t boundaryAt = 0;
};

Result evaluate(bool scheduleEnabled, const char* on, const char* off,
                bool presenceEnabled, bool presenceActive, time_t now,
                State& state);

}  // namespace ScheduleLogic
