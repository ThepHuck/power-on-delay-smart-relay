#pragma once

#include <stddef.h>
#include <functional>
#include <Arduino.h>
#include "schedule_storage.h"

// Existing device settings occupy 0-511 and Wi-Fi settings start at 512.
constexpr size_t SCHEDULE_SETTINGS_OFFSET = 768;
constexpr size_t API_KEYS_SETTINGS_OFFSET = 1536;
constexpr size_t SETTINGS_EEPROM_SIZE = 3072;

void loadScheduleSettings();
void beginScheduleClock();
void syncScheduleClock();
void registerScheduleRoutes();
void updateSchedules();
bool scheduleClockSynced();
const ScheduleSettings& getScheduleSettings();
int setScheduleTimeSettings(const String& host, unsigned int hours, const String& zone, String& error);
int putRebootSchedule(unsigned int id, const RebootSchedule& entry, String& error);
int removeRebootSchedule(unsigned int id, String& error);
int replaceRebootSchedules(const RebootSchedule* entries, unsigned int count, String& error);
void forEachTimeZone(const std::function<void(const String&)>& visitor);
