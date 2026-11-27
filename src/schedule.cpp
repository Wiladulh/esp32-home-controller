#include "schedule.h"
#include "config.h"
#include "presence.h"
#include "relay.h"
#include <time.h>

namespace {
bool scheduleActive(const RelayScheduleCfg& cfg, int minuteOfDay) {
  const int on = (cfg.on[0] - '0') * 600 + (cfg.on[1] - '0') * 60 +
                 (cfg.on[3] - '0') * 10 + cfg.on[4] - '0';
  const int off = (cfg.off[0] - '0') * 600 + (cfg.off[1] - '0') * 60 +
                  (cfg.off[3] - '0') * 10 + cfg.off[4] - '0';
  if (on < off) return minuteOfDay >= on && minuteOfDay < off;
  return minuteOfDay >= on || minuteOfDay < off;
}

bool lastScheduled[MAX_RELAYS] = {};
bool scheduledKnown[MAX_RELAYS] = {};
bool lastPresence[MAX_RELAYS] = {};
bool presenceKnown[MAX_RELAYS] = {};
bool initialized = false;

time_t boundaryTime(const tm& local, const RelayScheduleCfg& cfg, bool scheduled) {
  tm boundary = local;
  const char* value = scheduled ? cfg.on : cfg.off;
  boundary.tm_hour = (value[0] - '0') * 10 + value[1] - '0';
  boundary.tm_min = (value[3] - '0') * 10 + value[4] - '0';
  boundary.tm_sec = 0;
  boundary.tm_isdst = -1;
  return mktime(&boundary);
}
}

namespace Schedule {
void begin() {
  for (uint8_t i = 0; i < MAX_RELAYS; ++i) {
    lastScheduled[i] = false;
    scheduledKnown[i] = false;
    lastPresence[i] = false;
    presenceKnown[i] = false;
  }
  initialized = true;
}

void tick() {
  if (!initialized) begin();

  const time_t now = time(nullptr);
  struct tm local {};
  const bool timeValid = now >= 1700000000 && localtime_r(&now, &local);
  const int minuteOfDay = timeValid ? local.tm_hour * 60 + local.tm_min : 0;
  const AppConfig cfg = Config::get();

  for (uint8_t i = 0; i < MAX_RELAYS; ++i) {
    const auto& relay = cfg.relays[i];
    if (!relay.enabled || (!relay.schedule.enabled && !relay.humanDetector.enabled)) continue;

    const bool scheduled = relay.schedule.enabled &&
                           (timeValid ? scheduleActive(relay.schedule, minuteOfDay)
                                      : (scheduledKnown[i] && lastScheduled[i]));
    const bool presenceNow = relay.humanDetector.enabled && Presence::detected(i);
    const bool scheduleBoundary = timeValid && relay.schedule.enabled && scheduledKnown[i] &&
                                  scheduled != lastScheduled[i];
    const bool scheduleStateChanged = timeValid && relay.schedule.enabled &&
                                      (!scheduledKnown[i] || scheduled != lastScheduled[i]);
    const bool presenceChanged = !presenceKnown[i] || presenceNow != lastPresence[i];

    if (scheduleBoundary) {
      const time_t boundary = boundaryTime(local, relay.schedule, scheduled);
      Relay::clearManualOverrideAtBoundary(i, boundary);
    }

    if (scheduleStateChanged || presenceChanged) {
      Relay::setAutomatic(i, scheduled || presenceNow);
    }

    if (timeValid && relay.schedule.enabled) {
      lastScheduled[i] = scheduled;
      scheduledKnown[i] = true;
    }
    lastPresence[i] = presenceNow;
    presenceKnown[i] = true;
  }
}
}
