// Compile-time tests for API credential policy and bounded input validation.
#include "api_key_policy.h"
#include "api_validation.h"

static_assert(MAX_API_KEYS == 5, "Exactly five credential slots");
static_assert(!validApiKeyDays(0) && validApiKeyDays(1) && validApiKeyDays(30) && !validApiKeyDays(31),
              "Days must stay within 1-30 inclusive");
static_assert(apiKeyUsable(0, false, 0), "No-expiration keys work before NTP");
static_assert(!apiKeyUsable(100, false, 0), "Expiring keys fail closed before NTP");
static_assert(apiKeyUsable(100, false, 99), "A key is valid before its exclusive deadline");
static_assert(!apiKeyUsable(100, false, 100), "Expire exactly at the deadline");
static_assert(!apiKeyUsable(100, false, 101), "Never accept a key after expiry");
static_assert(!apiKeyUsable(100, true, 99), "Clock rollback cannot revive a latched expired key");
static_assert(!apiKeyUsable(0, true, 0), "An expired/revoked credential never becomes usable");

constexpr bool calendarValidation() {
  int year = 0, month = 0, day = 0;
  if (!parseApiDate("2028-02-29", year, month, day) || year != 2028 || month != 2 || day != 29) return false;
  if (parseApiDate("2027-02-29", year, month, day)) return false;
  if (parseApiDate("2100-02-29", year, month, day)) return false;
  if (!parseApiDate("2000-02-29", year, month, day)) return false;
  if (!parseApiDate("2099-12-31", year, month, day)) return false;
  if (parseApiDate("2026-04-31", year, month, day)) return false;
  if (parseApiDate("2026-00-01", year, month, day)) return false;
  if (parseApiDate("2026-13-01", year, month, day)) return false;
  if (parseApiDate("2026-11-00", year, month, day)) return false;
  if (parseApiDate("2026-1-01", year, month, day)) return false;
  if (parseApiDate("2026-11-01x", year, month, day)) return false;
  if (parseApiDate("", year, month, day)) return false;
  return true;
}
static_assert(calendarValidation(), "Reject malformed and impossible calendar dates");

constexpr bool numericValidation() {
  uint32_t value = 0;
  if (!parseApiUnsigned("4096", 4096, value) || value != 4096) return false;
  if (!parseApiUnsigned("4294967295", UINT32_MAX, value) || value != UINT32_MAX) return false;
  if (parseApiUnsigned("4097", 4096, value)) return false;
  if (parseApiUnsigned("4294967296", UINT32_MAX, value)) return false;
  if (parseApiUnsigned("-1", 30, value)) return false;
  if (parseApiUnsigned("+1", 30, value)) return false;
  if (parseApiUnsigned("1.5", 30, value)) return false;
  if (parseApiUnsigned(" 1", 30, value)) return false;
  if (parseApiUnsigned("", 30, value)) return false;
  return true;
}
static_assert(numericValidation(), "Reject numeric coercion and overflow");

template <unsigned int Size>
constexpr bool envelope(const char (&input)[Size]) { return apiJsonObjectEnvelope(input, Size - 1); }
static_assert(envelope("{}"), "Accept an empty JSON object");
static_assert(envelope(" \n{\"schedules\":[{\"days\":[0,6]}]}\r\n"), "Allow whitespace and nested schedules");
static_assert(envelope("{\"text\":\"a } bracket and \\\" quote\"}"), "Ignore brackets and escaped quotes in strings");
static_assert(!envelope("{}{}"), "Reject multiple root objects");
static_assert(!envelope("{}x"), "Reject trailing junk");
static_assert(!envelope("{\"text\":\"unfinished}"), "Reject unclosed strings");
static_assert(!envelope("[]"), "API input roots must be objects");
static_assert(!envelope("{\"x\":\"a\0b\"}"), "Reject literal NUL bytes");
