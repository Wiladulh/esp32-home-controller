#include "schedule_logic.h"
#include "relay_policy.h"

#include <cstdlib>
#include <ctime>
#include <iostream>

namespace {
int failures = 0;

void check(bool condition, const char* name) {
  if (condition) {
    std::cout << "PASS: " << name << '\n';
  } else {
    std::cerr << "FAIL: " << name << '\n';
    ++failures;
  }
}

time_t localTime(int year, int month, int day, int hour, int minute) {
  tm value {};
  value.tm_year = year - 1900;
  value.tm_mon = month - 1;
  value.tm_mday = day;
  value.tm_hour = hour;
  value.tm_min = minute;
  value.tm_isdst = -1;
  return mktime(&value);
}
}

int main() {
  setenv("TZ", "WIB-7", 1);
  tzset();

  ScheduleLogic::State overnightState;
  auto result = ScheduleLogic::evaluate(
      true, "18:00", "06:00", false, false,
      localTime(2026, 1, 15, 17, 59), overnightState);
  check(result.timeValid && !result.scheduled, "overnight schedule is OFF before 18:00");

  result = ScheduleLogic::evaluate(
      true, "18:00", "06:00", false, false,
      localTime(2026, 1, 15, 18, 0), overnightState);
  check(result.scheduled && result.boundary, "overnight schedule turns ON at 18:00");
  check(result.boundaryAt == localTime(2026, 1, 15, 18, 0),
        "ON transition reports exact 18:00 boundary");

  result = ScheduleLogic::evaluate(
      true, "18:00", "06:00", false, false,
      localTime(2026, 1, 16, 5, 59), overnightState);
  check(result.scheduled && !result.boundary, "overnight schedule remains ON before 06:00");

  result = ScheduleLogic::evaluate(
      true, "18:00", "06:00", false, false,
      localTime(2026, 1, 16, 6, 0), overnightState);
  check(!result.scheduled && result.boundary, "overnight schedule turns OFF at 06:00");
  check(result.boundaryAt == localTime(2026, 1, 16, 6, 0),
        "OFF transition reports exact 06:00 boundary");

  ScheduleLogic::State noNtpState;
  result = ScheduleLogic::evaluate(
      false, "18:00", "06:00", true, true, 0, noNtpState);
  check(!result.timeValid && result.presence && result.automaticOn,
        "PIR turns relay ON while NTP time is invalid");
  check(!result.boundary, "invalid NTP cannot produce a schedule boundary");

  result = ScheduleLogic::evaluate(
      false, "18:00", "06:00", true, false, 0, noNtpState);
  check(!result.automaticOn && result.shouldApply,
        "PIR hold expiry turns relay OFF while NTP remains invalid");

  ScheduleLogic::State combinedState;
  result = ScheduleLogic::evaluate(
      true, "18:00", "06:00", true, true,
      localTime(2026, 1, 15, 12, 0), combinedState);
  check(result.presence && result.automaticOn && !result.scheduled,
        "PIR keeps relay ON outside schedule window");

  result = ScheduleLogic::evaluate(
      true, "18:00", "06:00", true, false,
      localTime(2026, 1, 15, 12, 1), combinedState);
  check(!result.automaticOn, "relay turns OFF when schedule and PIR are both inactive");

  ScheduleLogic::State syncState;
  result = ScheduleLogic::evaluate(
      true, "18:00", "06:00", false, false, 0, syncState);
  check(!syncState.scheduledKnown && !result.boundary,
        "invalid NTP does not initialize or advance schedule history");

  result = ScheduleLogic::evaluate(
      true, "18:00", "06:00", false, false,
      localTime(2026, 1, 15, 20, 0), syncState);
  check(result.scheduled && !result.boundary,
        "first valid NTP sample establishes state without a false boundary");

  check(RelayPolicy::shouldClearManualOverride(true, 100, 200),
        "manual command before next schedule boundary is released");
  check(!RelayPolicy::shouldClearManualOverride(true, 200, 200),
        "manual command in the same second as boundary remains authoritative");
  check(!RelayPolicy::shouldClearManualOverride(true, 201, 200),
        "manual command after boundary is not released by an earlier boundary");
  check(!RelayPolicy::shouldClearManualOverride(false, 100, 200),
        "inactive manual override remains inactive");

  check(!RelayPolicy::shouldApplyAutomatic(true, true, false, true),
        "manual OFF blocks PIR from turning relay ON");
  check(!RelayPolicy::shouldApplyAutomatic(true, true, true, false),
        "manual ON blocks schedule from turning relay OFF");
  check(RelayPolicy::shouldApplyAutomatic(true, false, false, true),
        "automatic ON is allowed after override is released");
  check(!RelayPolicy::shouldApplyAutomatic(false, false, false, true),
        "disabled relay rejects automatic state changes");

  if (failures != 0) {
    std::cerr << failures << " test(s) failed\n";
    return 1;
  }
  std::cout << "All schedule behavior tests passed\n";
  return 0;
}
