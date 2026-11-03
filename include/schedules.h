#pragma once

#include <stddef.h>

// Existing device settings occupy 0-511 and Wi-Fi settings start at 512.
constexpr size_t SCHEDULE_SETTINGS_OFFSET = 768;
constexpr size_t SETTINGS_EEPROM_SIZE = 1536;

void loadScheduleSettings();
void beginScheduleClock();
void syncScheduleClock();
void registerScheduleRoutes();
void updateSchedules();
