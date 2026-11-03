// Compile-time tests for preserving daily schedules when upgrading firmware.
#include "schedule_storage.h"

static_assert(sizeof(LegacyRebootSchedule) == 3, "Preserve the earlier EEPROM entry layout");
static_assert(sizeof(LegacyScheduleSettings) == 412, "Preserve the earlier EEPROM record layout");

constexpr bool preservesLegacySettings() {
  LegacyScheduleSettings legacy = {LEGACY_SCHEDULE_SETTINGS_MAGIC, 0,
    "time.example.org", "America/Chicago", 6, MAX_REBOOT_SCHEDULES, {}};
  for (uint8_t i = 0; i < MAX_REBOOT_SCHEDULES; ++i) {
    legacy.entries[i] = {static_cast<uint8_t>(i / 12), static_cast<uint8_t>((i % 12) * 5),
                         REBOOT_DELAYS[i % 5]};
  }
  legacy.checksum = scheduleChecksum(legacy);
  if (legacy.checksum != 1523877566U) return false; // Earlier firmware's FNV-1a format.
  const ScheduleSettings upgraded = upgradeLegacySchedules(legacy);
  if (upgraded.magic != SCHEDULE_SETTINGS_MAGIC || upgraded.checksum != scheduleChecksum(upgraded) ||
      upgraded.count != legacy.count || upgraded.syncHours != legacy.syncHours) return false;
  for (unsigned int i = 0; i < sizeof(legacy.ntpServer); ++i) {
    if (upgraded.ntpServer[i] != legacy.ntpServer[i]) return false;
  }
  for (unsigned int i = 0; i < sizeof(legacy.timeZone); ++i) {
    if (upgraded.timeZone[i] != legacy.timeZone[i]) return false;
  }
  for (uint8_t i = 0; i < MAX_REBOOT_SCHEDULES; ++i) {
    if (!validRebootSchedule(upgraded.entries[i]) || upgraded.entries[i].weekdays != 0 ||
        upgraded.entries[i].hour != legacy.entries[i].hour ||
        upgraded.entries[i].minute != legacy.entries[i].minute ||
        upgraded.entries[i].delaySeconds != legacy.entries[i].delaySeconds) return false;
    for (int day = 0; day < 7; ++day) {
      if (!scheduleRunsOnWeekday(upgraded.entries[i], day)) return false;
    }
  }
  return true;
}
static_assert(preservesLegacySettings(), "Preserve every old daily schedule and NTP/time-zone settings");

constexpr bool checksumIncludesWeekdays() {
  ScheduleSettings saved = {SCHEDULE_SETTINGS_MAGIC, 0, "pool.ntp.org", "Etc/UTC", 1, 1,
                             {{8, 30, 5, 2}}};
  const uint32_t mondayChecksum = scheduleChecksum(saved);
  saved.entries[0].weekdays = 4;
  return scheduleChecksum(saved) != mondayChecksum;
}
static_assert(checksumIncludesWeekdays(), "Weekday selections must be protected by the stored checksum");
