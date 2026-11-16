#pragma once

#include <Arduino.h>

bool saveDeviceConfiguration(const String& name, const String& hostname, unsigned long delaySeconds);
int changeAdminPassword(const String& currentPassword, const String& newPassword, String& error);
int queueWifiConfiguration(const String& ssid, const String& password, String& error);
