#pragma once

#include <stdint.h>

constexpr uint8_t MAX_API_KEYS = 5;
enum class ApiKeyExpiration : uint8_t { None, Days, Date };

constexpr bool validApiKeyDays(unsigned int days) { return days >= 1 && days <= 30; }
constexpr bool apiKeyExpired(uint64_t expiresAt, bool permanentlyExpired, uint64_t now) {
  return permanentlyExpired || (expiresAt != 0 && now != 0 && now >= expiresAt);
}
constexpr bool apiKeyUsable(uint64_t expiresAt, bool permanentlyExpired, uint64_t now) {
  return !apiKeyExpired(expiresAt, permanentlyExpired, now) && (expiresAt == 0 || now != 0);
}

constexpr bool apiLeapYear(int year) {
  return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}
constexpr int apiDaysInMonth(int year, int month) {
  return month == 2 ? (apiLeapYear(year) ? 29 : 28) :
    (month == 4 || month == 6 || month == 9 || month == 11 ? 30 : 31);
}
constexpr bool parseApiDate(const char* date, int& year, int& month, int& day) {
  if (!date) return false;
  for (int i = 0; i < 10; ++i) {
    if (i == 4 || i == 7) { if (date[i] != '-') return false; }
    else if (date[i] < '0' || date[i] > '9') return false;
  }
  if (date[10] != '\0') return false;
  year = (date[0] - '0') * 1000 + (date[1] - '0') * 100 + (date[2] - '0') * 10 + date[3] - '0';
  month = (date[5] - '0') * 10 + date[6] - '0';
  day = (date[8] - '0') * 10 + date[9] - '0';
  return year >= 1970 && month >= 1 && month <= 12 && day >= 1 && day <= apiDaysInMonth(year, month);
}
