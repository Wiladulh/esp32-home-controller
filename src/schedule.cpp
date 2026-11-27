#include "schedule.h"
#include "config.h"
#include "presence.h"
#include "relay.h"
#include "schedule_logic.h"

namespace {
ScheduleLogic::State states[MAX_RELAYS];
bool initialized = false;
}

namespace Schedule {
void begin() {
  for (uint8_t i = 0; i < MAX_RELAYS; ++i) {
    states[i] = ScheduleLogic::State{};
  }
  initialized = true;
}

void tick() {
  if (!initialized) begin();

  const time_t now = time(nullptr);
  const AppConfig cfg = Config::get();

  for (uint8_t i = 0; i < MAX_RELAYS; ++i) {
    const auto& relay = cfg.relays[i];
    if (!relay.enabled || (!relay.schedule.enabled && !relay.humanDetector.enabled)) continue;

    const bool presenceActive = relay.humanDetector.enabled && Presence::detected(i);
    const auto result = ScheduleLogic::evaluate(
        relay.schedule.enabled, relay.schedule.on, relay.schedule.off,
        relay.humanDetector.enabled, presenceActive, now, states[i]);

    if (result.boundary) {
      Relay::clearManualOverrideAtBoundary(i, result.boundaryAt);
    }
    if (result.shouldApply) {
      Relay::setAutomatic(i, result.automaticOn);
    }
  }
}
}
