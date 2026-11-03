#pragma once

#include "reboot_schedule.h"

constexpr uint32_t LEGACY_SCHEDULE_SETTINGS_MAGIC = 0x53434801;
constexpr uint32_t SCHEDULE_SETTINGS_MAGIC = 0x53434802;

struct LegacyRebootSchedule {
  uint8_t hour;
  uint8_t minute;
  uint8_t delaySeconds;
};

template <typename Entry>
struct StoredScheduleSettings {
  uint32_t magic;
  uint32_t checksum;
  char ntpServer[254];
  char timeZone[49];
  uint8_t syncHours;
  uint8_t count;
  Entry entries[MAX_REBOOT_SCHEDULES];
};
using LegacyScheduleSettings = StoredScheduleSettings<LegacyRebootSchedule>;
using ScheduleSettings = StoredScheduleSettings<RebootSchedule>;

constexpr uint32_t scheduleEntryChecksum(uint32_t hash, const LegacyRebootSchedule& entry) {
  hash = (hash ^ entry.hour) * 16777619U;
  hash = (hash ^ entry.minute) * 16777619U;
  return (hash ^ entry.delaySeconds) * 16777619U;
}

constexpr uint32_t scheduleEntryChecksum(uint32_t hash, const RebootSchedule& entry) {
  hash = scheduleEntryChecksum(hash, LegacyRebootSchedule{entry.hour, entry.minute, entry.delaySeconds});
  return (hash ^ entry.weekdays) * 16777619U;
}

template <typename Entry>
constexpr uint32_t scheduleChecksum(const StoredScheduleSettings<Entry>& saved) {
  uint32_t hash = 2166136261U;
  for (char c : saved.ntpServer) hash = (hash ^ static_cast<uint8_t>(c)) * 16777619U;
  for (char c : saved.timeZone) hash = (hash ^ static_cast<uint8_t>(c)) * 16777619U;
  hash = (hash ^ saved.syncHours) * 16777619U;
  hash = (hash ^ saved.count) * 16777619U;
  for (const Entry& entry : saved.entries) hash = scheduleEntryChecksum(hash, entry);
  return hash;
}

// Earlier firmware stored only daily times. Preserve them as NONE (every day),
// retaining the NTP settings and all schedule values. Persist on the next Save.
constexpr ScheduleSettings upgradeLegacySchedules(const LegacyScheduleSettings& legacy) {
  ScheduleSettings upgraded = {};
  upgraded.magic = SCHEDULE_SETTINGS_MAGIC;
  for (unsigned int i = 0; i < sizeof(legacy.ntpServer); ++i) upgraded.ntpServer[i] = legacy.ntpServer[i];
  for (unsigned int i = 0; i < sizeof(legacy.timeZone); ++i) upgraded.timeZone[i] = legacy.timeZone[i];
  upgraded.syncHours = legacy.syncHours;
  upgraded.count = legacy.count;
  for (unsigned int i = 0; i < MAX_REBOOT_SCHEDULES; ++i) {
    const LegacyRebootSchedule& entry = legacy.entries[i];
    upgraded.entries[i] = {entry.hour, entry.minute, entry.delaySeconds, 0};
  }
  upgraded.checksum = scheduleChecksum(upgraded);
  return upgraded;
}
