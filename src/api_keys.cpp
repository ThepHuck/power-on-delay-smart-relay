#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <bearssl/bearssl_hash.h>
#include <time.h>

#include "api_keys.h"
#include "schedules.h"

extern ESP8266WebServer server;
extern String displayName, mdnsName;
extern unsigned long startupDelaySeconds;
extern bool constantTimeEqual(const uint8_t*, const uint8_t*, size_t);
extern String escapeHtml(const String&);
extern String csrfInput();
extern void redirectTo(const String&);
extern String getConfigHTML(const String&, bool, const String&, const String&, const String&, const String&);

namespace {
constexpr uint32_t API_KEYS_MAGIC = 0x4b455901;
struct StoredApiKey {
  char id[17];
  char label[33];
  uint8_t digest[32];
  uint64_t createdAt;
  uint64_t expiresAt; // Exclusive UTC deadline; zero means no expiration.
  char expirationDate[11];
  char expirationZone[49];
  ApiKeyExpiration mode;
  uint8_t expired; // Once observed expired, a key cannot become valid after a clock correction.
};
struct StoredApiKeys {
  uint32_t magic;
  uint32_t checksum;
  StoredApiKey entries[MAX_API_KEYS];
};
static_assert(API_KEYS_SETTINGS_OFFSET + sizeof(StoredApiKeys) <= SETTINGS_EEPROM_SIZE,
              "API keys exceed EEPROM allocation");
static_assert(SCHEDULE_SETTINGS_OFFSET + sizeof(ScheduleSettings) <= API_KEYS_SETTINGS_OFFSET,
              "Schedule settings overlap API key storage");
StoredApiKeys keys = {};
uint64_t trustedMilliseconds = 0;
uint32_t lastSecurityTick = 0;
bool expirationSavePending = false;
uint32_t lastExpirationSave = 0;

uint32_t keyChecksum(const StoredApiKeys& saved) {
  uint32_t hash = 2166136261U;
  auto bytes = [&hash](const void* data, size_t size) {
    const uint8_t* input = static_cast<const uint8_t*>(data);
    while (size--) hash = (hash ^ *input++) * 16777619U;
  };
  auto integer = [&hash](uint64_t value) {
    for (unsigned int i = 0; i < 8; ++i) hash = (hash ^ ((value >> (8 * i)) & 255)) * 16777619U;
  };
  for (const StoredApiKey& entry : saved.entries) {
    bytes(entry.id, sizeof(entry.id)); bytes(entry.label, sizeof(entry.label));
    bytes(entry.digest, sizeof(entry.digest));
    integer(entry.createdAt); integer(entry.expiresAt);
    bytes(entry.expirationDate, sizeof(entry.expirationDate));
    bytes(entry.expirationZone, sizeof(entry.expirationZone));
    hash = (hash ^ static_cast<uint8_t>(entry.mode)) * 16777619U;
    hash = (hash ^ entry.expired) * 16777619U;
  }
  return hash;
}

bool persistKeys(StoredApiKeys& proposed) {
  proposed.magic = API_KEYS_MAGIC;
  proposed.checksum = keyChecksum(proposed);
  StoredApiKeys previous = {};
  EEPROM.get(API_KEYS_SETTINGS_OFFSET, previous);
  EEPROM.put(API_KEYS_SETTINGS_OFFSET, proposed);
  if (!EEPROM.commit()) { EEPROM.put(API_KEYS_SETTINGS_OFFSET, previous); return false; }
  keys = proposed;
  expirationSavePending = false;
  return true;
}

bool validLabel(const String& label) {
  if (!label.length() || label.length() > 32) return false;
  for (unsigned int i = 0; i < label.length(); ++i) {
    if (static_cast<uint8_t>(label[i]) < 32 || label[i] == 127) return false;
  }
  return true;
}

void randomHex(char* output, size_t byteCount) {
  uint8_t bytes[32];
  ESP.random(bytes, byteCount);
  constexpr char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < byteCount; ++i) {
    output[2 * i] = hex[bytes[i] >> 4]; output[2 * i + 1] = hex[bytes[i] & 15];
  }
  output[2 * byteCount] = '\0';
}

bool hexString(const String& text, unsigned int length) {
  if (text.length() != length) return false;
  for (unsigned int i = 0; i < length; ++i) {
    if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f'))) return false;
  }
  return true;
}

void hashToken(const String& token, uint8_t* digest) {
  br_sha256_context context;
  br_sha256_init(&context);
  br_sha256_update(&context, token.c_str(), token.length());
  br_sha256_out(&context, digest);
}

String timestamp(uint64_t epoch, bool utc) {
  const time_t value = static_cast<time_t>(epoch);
  struct tm result;
  if (utc) gmtime_r(&value, &result); else localtime_r(&value, &result);
  char formatted[80];
  strftime(formatted, sizeof(formatted), utc ? "%Y-%m-%dT%H:%M:%SZ" : "%Y-%m-%d %H:%M:%S %Z", &result);
  return formatted;
}

const char* modeName(ApiKeyExpiration mode) {
  return mode == ApiKeyExpiration::Days ? "days" : mode == ApiKeyExpiration::Date ? "date" : "none";
}

const char* keyStatus(const StoredApiKey& key, uint64_t now) {
  if (apiKeyExpired(key.expiresAt, key.expired, now)) return "expired";
  return key.expiresAt && !now ? "clock_unavailable" : "active";
}
} // namespace

uint64_t apiSecurityTime() {
  if (!scheduleClockSynced()) return 0;
  const uint32_t tick = millis();
  const uint64_t wall = static_cast<uint64_t>(time(nullptr)) * 1000;
  if (trustedMilliseconds) trustedMilliseconds += static_cast<uint32_t>(tick - lastSecurityTick);
  if (wall > trustedMilliseconds) trustedMilliseconds = wall;
  lastSecurityTick = tick;
  return trustedMilliseconds / 1000;
}

void loadApiKeys() {
  StoredApiKeys saved = {};
  EEPROM.get(API_KEYS_SETTINGS_OFFSET, saved);
  if (saved.magic != API_KEYS_MAGIC || saved.checksum != keyChecksum(saved)) return;
  for (const StoredApiKey& key : saved.entries) {
    if (!key.id[0]) continue;
    if (!memchr(key.id, '\0', sizeof(key.id)) || !hexString(key.id, 16) ||
        !memchr(key.label, '\0', sizeof(key.label)) || !validLabel(key.label) ||
        !memchr(key.expirationDate, '\0', sizeof(key.expirationDate)) ||
        !memchr(key.expirationZone, '\0', sizeof(key.expirationZone)) ||
        static_cast<uint8_t>(key.mode) > 2 || key.expired > 1 ||
        (key.mode == ApiKeyExpiration::None ? key.expiresAt != 0 : key.expiresAt == 0)) return;
  }
  keys = saved;
}

void updateApiKeys() {
  const uint64_t now = apiSecurityTime();
  for (StoredApiKey& key : keys.entries) {
    if (key.id[0] && !key.expired && apiKeyExpired(key.expiresAt, false, now)) {
      key.expired = 1;
      expirationSavePending = true;
      lastExpirationSave = millis() - 60000UL;
    }
  }
  if (expirationSavePending && millis() - lastExpirationSave >= 60000UL) {
    lastExpirationSave = millis();
    StoredApiKeys proposed = keys;
    persistKeys(proposed); // Keep expired flags in RAM even if flash is temporarily unavailable.
  }
}

int authenticateApiKey(const String& authorization) {
  if (!authorization.substring(0, 7).equalsIgnoreCase("Bearer ")) return 401;
  const String token = authorization.substring(7);
  if (token.length() != 68 || !token.startsWith("esp_") || !hexString(token.substring(4), 64)) return 401;
  updateApiKeys();
  uint8_t digest[32];
  hashToken(token, digest);
  const uint64_t now = apiSecurityTime();
  for (const StoredApiKey& key : keys.entries) {
    if (!key.id[0] || !constantTimeEqual(key.digest, digest, sizeof(digest))) continue;
    if (apiKeyExpired(key.expiresAt, key.expired, now)) return 401;
    return apiKeyUsable(key.expiresAt, key.expired, now) ? 200 : 503;
  }
  return 401;
}

int createApiKey(const String& label, ApiKeyExpiration mode, unsigned int days,
                 const String& date, String& token, String& id, String& error) {
  token = ""; id = "";
  if (!validLabel(label)) { error = "Key labels must contain 1-32 bytes without control characters."; return 400; }
  unsigned int slot = MAX_API_KEYS;
  for (unsigned int i = 0; i < MAX_API_KEYS; ++i) if (!keys.entries[i].id[0]) { slot = i; break; }
  if (slot == MAX_API_KEYS) { error = "All five key slots are in use. Revoke a key to make room."; return 409; }
  const uint64_t now = apiSecurityTime();
  uint64_t deadline = 0;
  if (mode != ApiKeyExpiration::None && !now) {
    error = "Wait for a successful NTP sync before creating an expiring key."; return 503;
  }
  if (mode == ApiKeyExpiration::Days) {
    if (!validApiKeyDays(days)) { error = "Expiration days must be a whole number from 1 to 30."; return 400; }
    deadline = now + static_cast<uint64_t>(days) * 86400;
  } else if (mode == ApiKeyExpiration::Date) {
    int year, month, day;
    if (!parseApiDate(date.c_str(), year, month, day)) { error = "Use a valid expiration date in YYYY-MM-DD format."; return 400; }
    struct tm midnight = {};
    midnight.tm_year = year - 1900; midnight.tm_mon = month - 1;
    midnight.tm_mday = day + 1; midnight.tm_isdst = -1;
    const time_t end = mktime(&midnight);
    if (end < 0 || static_cast<uint64_t>(end) <= now) { error = "The selected expiration date has already ended."; return 400; }
    deadline = static_cast<uint64_t>(end);
  } else if (mode != ApiKeyExpiration::None) { error = "Invalid expiration mode."; return 400; }
  StoredApiKeys proposed = keys;
  StoredApiKey& entry = proposed.entries[slot];
  entry = {};
  bool duplicate;
  do {
    randomHex(entry.id, 8); duplicate = false;
    for (const StoredApiKey& existing : keys.entries) if (String(existing.id) == entry.id) duplicate = true;
  } while (duplicate);
  label.toCharArray(entry.label, sizeof(entry.label));
  char secret[69] = "esp_";
  randomHex(secret + 4, 32);
  hashToken(secret, entry.digest);
  entry.createdAt = now; entry.expiresAt = deadline; entry.mode = mode;
  if (mode == ApiKeyExpiration::Date) {
    date.toCharArray(entry.expirationDate, sizeof(entry.expirationDate));
    strncpy(entry.expirationZone, getScheduleSettings().timeZone, sizeof(entry.expirationZone) - 1);
  }
  const String newId = entry.id;
  if (!persistKeys(proposed)) { memset(secret, 0, sizeof(secret)); error = "Could not save the key to flash."; return 500; }
  token = secret; id = newId;
  memset(secret, 0, sizeof(secret));
  return 201;
}

int revokeApiKey(const String& id, String& error) {
  for (unsigned int i = 0; i < MAX_API_KEYS; ++i) {
    if (!keys.entries[i].id[0] || id != keys.entries[i].id) continue;
    StoredApiKeys proposed = keys;
    proposed.entries[i] = {};
    if (!persistKeys(proposed)) { error = "Could not revoke the key in flash."; return 500; }
    return 200;
  }
  error = "API key not found."; return 404;
}

void apiKeysJson(JsonArray output) {
  updateApiKeys();
  const uint64_t now = apiSecurityTime();
  for (const StoredApiKey& key : keys.entries) {
    if (!key.id[0]) continue;
    JsonObject row = output.createNestedObject();
    row["id"] = key.id; row["label"] = key.label;
    row["expirationMode"] = modeName(key.mode);
    row["status"] = keyStatus(key, now);
    if (key.createdAt) row["createdAt"] = timestamp(key.createdAt, true); else row["createdAt"] = nullptr;
    if (key.expiresAt) row["expiresAt"] = timestamp(key.expiresAt, true); else row["expiresAt"] = nullptr;
    if (key.mode == ApiKeyExpiration::Date) { row["expiresOn"] = key.expirationDate; row["expirationTimeZone"] = key.expirationZone; }
  }
}

String apiKeysConfigHTML(const String& newToken) {
  updateApiKeys();
  String html = F("<section id=\"apiKeys\"><h3>API keys</h3><p>Generate up to five keys for programmatic access. Each key can read and change all settings. Copy a new key when it appears; it is shown only once.</p>");
  if (newToken.length()) {
    html += F("<p role=\"status\">API key created. Copy it now:</p><input aria-label=\"New API key\" type=\"text\" readonly spellcheck=\"false\" value=\"");
    html += escapeHtml(newToken);
    html += F("\">");
  }
  const uint64_t now = apiSecurityTime();
  unsigned int count = 0;
  for (const StoredApiKey& key : keys.entries) {
    if (!key.id[0]) continue;
    ++count;
    html += F("<article><p><strong>"); html += escapeHtml(key.label);
    html += F("</strong><br>Key ID: <code>"); html += key.id;
    html += F("</code><br>Status: "); html += keyStatus(key, now);
    html += F("<br>Expires: ");
    html += key.expiresAt ? escapeHtml(timestamp(key.expiresAt, false)) + F(" (exclusive)") : String(F("No expiration"));
    html += F("</p><form action=\"/config/api-keys/revoke\" method=\"post\">");
    html += csrfInput();
    html += F("<input type=\"hidden\" name=\"id\" value=\""); html += key.id;
    html += F("\"><button type=\"submit\">Revoke</button></form></article>");
  }
  if (count < MAX_API_KEYS) {
    html += F("<form action=\"/config/api-keys\" method=\"post\">"); html += csrfInput();
    html += F(R"HTML(<label for="api-label">Key name</label><input id="api-label" name="label" maxlength="32" required>
<label for="api-expiration">Expiration</label><select id="api-expiration" name="expirationMode" style="width:100%;padding:12px;font-size:18px">
<option value="none">No expiration</option><option value="days">Number of days</option><option value="date">Expiration date</option></select>
<div id="api-days-field" hidden><label for="api-days">Days (1-30)</label><input id="api-days" name="days" type="number" min="1" max="30" step="1" value="1" disabled></div>
<div id="api-date-field" hidden><label for="api-date">Expiration date</label><input id="api-date" name="date" type="date" disabled></div>
<p class="help">Days are 24-hour periods from creation. A date remains valid through 23:59 and expires at 00:00 the following day in the saved time zone. Expiring keys require NTP time after every controller restart. No-expiration keys work offline.</p>
<button type="submit">Generate API key</button></form>
<script>
document.getElementById('api-expiration').addEventListener('change',function(){
  for(const mode of ['days','date']){
    const selected=this.value===mode;
    document.getElementById('api-'+mode+'-field').hidden=!selected;
    const input=document.getElementById('api-'+mode);input.disabled=!selected;input.required=selected;
  }
});
</script>)HTML");
  } else html += F("<p>All five slots are in use. Revoke a key to generate another.</p>");
  html += F("<p class=\"help\">Send keys in the Authorization: Bearer header to /api/v1 endpoints. Revoking a key takes effect immediately.</p></section>");
  return html;
}

void handleCreateApiKey() {
  const String mode = server.arg("expirationMode");
  ApiKeyExpiration expiration;
  unsigned int days = 0;
  String error, token, id;
  int status = 400;
  bool valid = true;
  if (mode == "none") expiration = ApiKeyExpiration::None;
  else if (mode == "date") expiration = ApiKeyExpiration::Date;
  else if (mode == "days") {
    expiration = ApiKeyExpiration::Days;
    const String input = server.arg("days");
    valid = input.length() > 0 && input.length() <= 2;
    for (unsigned int i = 0; valid && i < input.length(); ++i) {
      if (input[i] < '0' || input[i] > '9') valid = false;
      else days = days * 10 + input[i] - '0';
    }
    valid = valid && validApiKeyDays(days);
  } else valid = false;
  if (valid) status = createApiKey(server.arg("label"), expiration, days, server.arg("date"), token, id, error);
  else error = "Select an expiration mode and, for days, a whole number from 1 to 30.";
  server.send(status, "text/html; charset=utf-8", getConfigHTML(error, status >= 400,
    displayName, mdnsName, String(startupDelaySeconds), token));
}

void handleRevokeApiKey() {
  String error;
  const int status = revokeApiKey(server.arg("id"), error);
  if (status == 200) redirectTo("/config#apiKeys");
  else server.send(status, "text/html; charset=utf-8", getConfigHTML(error, true,
    displayName, mdnsName, String(startupDelaySeconds), ""));
}
