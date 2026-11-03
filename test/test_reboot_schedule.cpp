// Compile-time behavioral tests; no board or host C++ runtime is required.
// xtensa-lx106-elf-g++ -std=c++14 -Iinclude -fsyntax-only test/test_reboot_schedule.cpp
#include "reboot_schedule.h"

constexpr bool allScheduleOptionsAreValid() {
  for (uint8_t hour = 0; hour < 24; ++hour) {
    for (uint8_t minute = 0; minute < 60; minute += 5) {
      for (uint8_t delay : REBOOT_DELAYS) {
        if (!validRebootSchedule({hour, minute, delay})) return false;
      }
    }
  }
  return true;
}
static_assert(allScheduleOptionsAreValid(), "All selectable times and delays must be accepted");
static_assert(!validRebootSchedule({24, 0, 5}), "Reject hour 24");
static_assert(!validRebootSchedule({0, 60, 5}), "Reject minute 60");
static_assert(!validRebootSchedule({0, 1, 5}), "Reject minutes between five-minute steps");
static_assert(!validRebootSchedule({0, 0, 0}), "Reject a zero delay");
static_assert(!validRebootSchedule({0, 0, 20}), "Reject an unlisted delay");
static_assert(!validRebootSchedule({255, 255, 255}), "Reject corrupt stored values");

constexpr bool dailyRebootBehavior() {
  const RebootSchedule entry = {1, 30, 10};
  int lastRun = 0;
  if (rebootScheduleDue(entry, 1, 29, 46000, lastRun) || lastRun != 0) return false;
  if (!rebootScheduleDue(entry, 1, 30, 46000, lastRun) || lastRun != 46000) return false;
  // Repeated loop iterations, an NTP correction, or a DST repeated hour cannot repeat it.
  if (rebootScheduleDue(entry, 1, 30, 46000, lastRun)) return false;
  if (rebootScheduleDue(entry, 1, 30, 45999, lastRun) || lastRun != 46000) return false;
  if (!rebootScheduleDue(entry, 1, 30, 46001, lastRun)) return false;
  return true;
}
static_assert(dailyRebootBehavior(), "Run once on each matching local calendar day");

constexpr bool missedTimesAreSkipped() {
  const RebootSchedule entry = {2, 30, 5};
  int lastRun = 0;
  if (rebootScheduleDue(entry, 2, 29, 46000, lastRun)) return false;
  // A forward NTP correction or spring DST jump skips the missed occurrence.
  if (rebootScheduleDue(entry, 3, 0, 46000, lastRun) || lastRun != 0) return false;
  return rebootScheduleDue(entry, 2, 30, 46001, lastRun);
}
static_assert(missedTimesAreSkipped(), "Do not replay missed reboots");

constexpr bool independentSchedulesAndMidnight() {
  int eveningRun = 0, midnightRun = 0;
  if (!rebootScheduleDue({23, 55, 60}, 23, 55, 46000, eveningRun)) return false;
  if (!rebootScheduleDue({0, 0, 5}, 0, 0, 46001, midnightRun)) return false;
  return !rebootScheduleDue({0, 0, 5}, 0, 0, 46001, midnightRun);
}
static_assert(independentSchedulesAndMidnight(), "Schedules keep separate daily guards across midnight");
