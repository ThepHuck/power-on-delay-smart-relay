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
  if (rebootScheduleDue(entry, 1, 29, 1, 46000, lastRun) || lastRun != 0) return false;
  if (!rebootScheduleDue(entry, 1, 30, 1, 46000, lastRun) || lastRun != 46000) return false;
  // Repeated loop iterations, an NTP correction, or a DST repeated hour cannot repeat it.
  if (rebootScheduleDue(entry, 1, 30, 1, 46000, lastRun)) return false;
  if (rebootScheduleDue(entry, 1, 30, 0, 45999, lastRun) || lastRun != 46000) return false;
  if (!rebootScheduleDue(entry, 1, 30, 2, 46001, lastRun)) return false;
  return true;
}
static_assert(dailyRebootBehavior(), "Run once on each matching local calendar day");

constexpr bool missedTimesAreSkipped() {
  const RebootSchedule entry = {2, 30, 5};
  int lastRun = 0;
  if (rebootScheduleDue(entry, 2, 29, 1, 46000, lastRun)) return false;
  // A forward NTP correction or spring DST jump skips the missed occurrence.
  if (rebootScheduleDue(entry, 3, 0, 1, 46000, lastRun) || lastRun != 0) return false;
  return rebootScheduleDue(entry, 2, 30, 2, 46001, lastRun);
}
static_assert(missedTimesAreSkipped(), "Do not replay missed reboots");

constexpr bool independentSchedulesAndMidnight() {
  int eveningRun = 0, midnightRun = 0;
  if (!rebootScheduleDue({23, 55, 60}, 23, 55, 1, 46000, eveningRun)) return false;
  if (!rebootScheduleDue({0, 0, 5}, 0, 0, 2, 46001, midnightRun)) return false;
  return !rebootScheduleDue({0, 0, 5}, 0, 0, 2, 46001, midnightRun);
}
static_assert(independentSchedulesAndMidnight(), "Schedules keep separate daily guards across midnight");

constexpr bool allDaySelectionsMatch() {
  for (unsigned int mask = 0; mask <= ALL_SCHEDULE_DAYS; ++mask) {
    const RebootSchedule entry = {12, 0, 5, static_cast<uint8_t>(mask)};
    if (!validRebootSchedule(entry)) return false;
    for (int weekday = 0; weekday < 7; ++weekday) {
      const bool expected = mask == 0 || mask == ALL_SCHEDULE_DAYS || (mask & (1U << weekday));
      if (scheduleRunsOnWeekday(entry, weekday) != expected) return false;
      int lastRun = 0;
      if (rebootScheduleDue(entry, 12, 0, weekday, 46000 + weekday, lastRun) != expected) return false;
      if (lastRun != (expected ? 46000 + weekday : 0)) return false;
    }
  }
  return true;
}
static_assert(allDaySelectionsMatch(), "All 128 checkbox combinations must obey ALL/NONE = daily");
static_assert(scheduleRunsOnWeekday({0, 0, 5, 1}, 0), "The first checkbox is Sunday");
static_assert(!scheduleRunsOnWeekday({0, 0, 5, 1}, 1), "Sunday does not select Monday");
static_assert(scheduleRunsOnWeekday({0, 0, 5, 64}, 6), "The last checkbox is Saturday");
static_assert(!scheduleRunsOnWeekday({0, 0, 5, 0}, -1), "Reject invalid weekdays");
static_assert(!scheduleRunsOnWeekday({0, 0, 5, 0}, 7), "Reject weekdays beyond Saturday");
static_assert(!validRebootSchedule({0, 0, 5, 128}), "Reject corrupt weekday bits");

constexpr bool mondayRunsOncePerWeek() {
  const RebootSchedule entry = {8, 30, 10, 2};
  int lastRun = 0;
  int runs = 0;
  for (int day = 0; day < 14; ++day) {
    const bool ran = rebootScheduleDue(entry, 8, 30, day % 7, 46000 + day, lastRun);
    if (ran != (day == 1 || day == 8)) return false;
    if (ran) ++runs;
    if (rebootScheduleDue(entry, 8, 30, day % 7, 46000 + day, lastRun)) return false;
  }
  return runs == 2;
}
static_assert(mondayRunsOncePerWeek(), "Selecting Monday reboots once each Monday at the given time");

static_assert(!rebootSchedulesOverlap({8, 0, 5, 2}, {8, 0, 10, 4}),
              "Allow the same time on separate weekdays");
static_assert(rebootSchedulesOverlap({8, 0, 5, 2}, {8, 0, 10, 6}),
              "Reject the same time on overlapping weekdays");
static_assert(rebootSchedulesOverlap({8, 0, 5, 0}, {8, 0, 10, 64}),
              "No days selected overlaps every weekday");
static_assert(rebootSchedulesOverlap({8, 0, 5, 127}, {8, 0, 10, 1}),
              "All days selected overlaps every weekday");
static_assert(!rebootSchedulesOverlap({8, 0, 5, 0}, {8, 5, 10, 0}),
              "Allow different times on the same days");
