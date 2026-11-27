#include "schedule.h"
#include "config.h"
#include "presence.h"
#include "relay.h"
#include <time.h>
#include <string.h>

namespace {
bool scheduleActive(const RelayScheduleCfg& cfg, int minuteOfDay) {
  const int on = (cfg.on[0] - '0') * 600 + (cfg.on[1] - '0') * 60 +
                 (cfg.on[3] - '0') * 10 + cfg.on[4] - '0';
  const int off = (cfg.off[0] - '0') * 600 + (cfg.off[1] - '0') * 60 +
                  (cfg.off[3] - '0') * 10 + cfg.off[4] - '0';
  if (on < off) return minuteOfDay >= on && minuteOfDay < off;
  return minuteOfDay >= on || minuteOfDay < off;
}
int lastMinute = -1;
bool lastPresence[MAX_RELAYS] = {};
bool initialized = false;
}

namespace Schedule {
void begin() {
  lastMinute = -1;
  memset(lastPresence, 0, sizeof(lastPresence));
  initialized = true;
}

void tick() {
  if (!initialized) begin();
  const time_t now = time(nullptr);
  if (now < 1700000000) return;
  struct tm local {};
  if (!localtime_r(&now, &local)) return;
  const int minuteOfDay = local.tm_hour * 60 + local.tm_min;
  const bool newMinute = minuteOfDay != lastMinute;
  const AppConfig cfg = Config::get();
  for (uint8_t i = 0; i < MAX_RELAYS; ++i) {
    const auto& relay = cfg.relays[i];
    if (!relay.enabled || (!relay.schedule.enabled && !relay.humanDetector.enabled)) continue;
    const bool hasPresence = relay.humanDetector.enabled;
    const bool presenceNow = hasPresence && Presence::detected(i);
    if (!newMinute && presenceNow == lastPresence[i]) continue;
    const bool scheduled = relay.schedule.enabled && scheduleActive(relay.schedule, minuteOfDay);
    const bool desired = scheduled || presenceNow;
    Relay::setAutomatic(i, desired);
    lastPresence[i] = presenceNow;
  }
  lastMinute = minuteOfDay;
}
}
