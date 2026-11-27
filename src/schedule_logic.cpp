#include "schedule_logic.h"

namespace {
constexpr time_t MIN_VALID_TIME = 1700000000;

bool isActive(const char* onText, const char* offText, int minuteOfDay) {
  const int on = (onText[0] - '0') * 600 + (onText[1] - '0') * 60 +
                 (onText[3] - '0') * 10 + onText[4] - '0';
  const int off = (offText[0] - '0') * 600 + (offText[1] - '0') * 60 +
                  (offText[3] - '0') * 10 + offText[4] - '0';
  if (on < off) return minuteOfDay >= on && minuteOfDay < off;
  return minuteOfDay >= on || minuteOfDay < off;
}

time_t makeBoundary(const tm& local, const char* text) {
  tm boundary = local;
  boundary.tm_hour = (text[0] - '0') * 10 + text[1] - '0';
  boundary.tm_min = (text[3] - '0') * 10 + text[4] - '0';
  boundary.tm_sec = 0;
  boundary.tm_isdst = -1;
  return mktime(&boundary);
}
}  // namespace

namespace ScheduleLogic {

Result evaluate(bool scheduleEnabled, const char* on, const char* off,
                bool presenceEnabled, bool presenceActive, time_t now,
                State& state) {
  Result result;
  tm local {};
  result.timeValid = now >= MIN_VALID_TIME && localtime_r(&now, &local) != nullptr;

  if (scheduleEnabled) {
    if (result.timeValid) {
      const int minuteOfDay = local.tm_hour * 60 + local.tm_min;
      result.scheduled = isActive(on, off, minuteOfDay);
      result.boundary = state.scheduledKnown && result.scheduled != state.lastScheduled;
      if (result.boundary) {
        result.boundaryAt = makeBoundary(local, result.scheduled ? on : off);
      }
      result.shouldApply = !state.scheduledKnown || result.scheduled != state.lastScheduled;
      state.lastScheduled = result.scheduled;
      state.scheduledKnown = true;
    } else {
      result.scheduled = state.scheduledKnown && state.lastScheduled;
    }
  }

  result.presence = presenceEnabled && presenceActive;
  result.shouldApply = result.shouldApply ||
                       !state.presenceKnown || result.presence != state.lastPresence;
  result.automaticOn = result.scheduled || result.presence;

  state.lastPresence = result.presence;
  state.presenceKnown = true;
  return result;
}

}  // namespace ScheduleLogic
