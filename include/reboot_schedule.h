#pragma once

#include <stdint.h>

constexpr uint8_t MAX_REBOOT_SCHEDULES = 32;
constexpr uint8_t REBOOT_DELAYS[] = {5, 10, 15, 30, 60};

struct RebootSchedule {
  uint8_t hour;
  uint8_t minute;
  uint8_t delaySeconds;
};

constexpr bool validRebootSchedule(const RebootSchedule& entry) {
  if (entry.hour > 23 || entry.minute > 55 || entry.minute % 5 != 0) return false;
  for (uint8_t delay : REBOOT_DELAYS) {
    if (entry.delaySeconds == delay) return true;
  }
  return false;
}

// A monotonically increasing local calendar day prevents repeats when NTP or
// daylight saving time moves the clock backward. Missed minutes are not replayed.
constexpr bool rebootScheduleDue(const RebootSchedule& entry, int hour, int minute,
                                 int day, int& lastRunDay) {
  if (entry.hour != hour || entry.minute != minute || day <= lastRunDay) return false;
  lastRunDay = day;
  return true;
}
