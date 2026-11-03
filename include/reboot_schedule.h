#pragma once

#include <stdint.h>

constexpr uint8_t MAX_REBOOT_SCHEDULES = 32;
constexpr uint8_t REBOOT_DELAYS[] = {5, 10, 15, 30, 60};
constexpr uint8_t ALL_SCHEDULE_DAYS = 0x7f; // Bit 0 is Sunday; bit 6 is Saturday.

struct RebootSchedule {
  uint8_t hour;
  uint8_t minute;
  uint8_t delaySeconds;
  uint8_t weekdays; // Sunday-first bitmask; 0 and 127 both mean every day.
};

constexpr bool validRebootSchedule(const RebootSchedule& entry) {
  if (entry.hour > 23 || entry.minute > 55 || entry.minute % 5 != 0 ||
      entry.weekdays > ALL_SCHEDULE_DAYS) return false;
  for (uint8_t delay : REBOOT_DELAYS) {
    if (entry.delaySeconds == delay) return true;
  }
  return false;
}

constexpr uint8_t effectiveScheduleDays(const RebootSchedule& entry) {
  return entry.weekdays == 0 ? ALL_SCHEDULE_DAYS : entry.weekdays;
}

constexpr bool scheduleRunsOnWeekday(const RebootSchedule& entry, int weekday) {
  return weekday >= 0 && weekday <= 6 && (effectiveScheduleDays(entry) & (1U << weekday));
}

constexpr bool rebootSchedulesOverlap(const RebootSchedule& first, const RebootSchedule& second) {
  return first.hour == second.hour && first.minute == second.minute &&
    (effectiveScheduleDays(first) & effectiveScheduleDays(second));
}

// A monotonically increasing local calendar day prevents repeats when NTP or
// daylight saving time moves the clock backward. Missed minutes are not replayed.
constexpr bool rebootScheduleDue(const RebootSchedule& entry, int hour, int minute,
                                 int weekday, int day, int& lastRunDay) {
  if (entry.hour != hour || entry.minute != minute || day <= lastRunDay ||
      !scheduleRunsOnWeekday(entry, weekday)) return false;
  lastRunDay = day;
  return true;
}
