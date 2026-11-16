#include <ESP8266WebServer.h>
#include <ArduinoJson.h>
#include <time.h>

#include "api.h"
#include "api_keys.h"
#include "api_validation.h"
#include "device_services.h"
#include "schedules.h"

extern ESP8266WebServer server;
extern String displayName, mdnsName, wifiSsid, wifiMessage, wifiOperationStatus;
extern uint32_t wifiOperationId;
extern unsigned long startupDelaySeconds, timerStartedAt, timerDuration;
extern bool relayState, timerActive, wifiAttemptPending, testingWifi, wifiSaveFailed;
extern bool validDisplayName(const String&), validMdnsName(const String&);
extern void updateTimer(), setRelay(bool);
extern unsigned long secondsRemaining();

namespace {
constexpr uint32_t MAX_API_BODY = 4096;
constexpr uint32_t MAX_API_TIMER_SECONDS = 4294967;
struct ApiRequest {
  String method, path, authorization, contentType, ifMatch, body;
  uint32_t contentLength = 0;
};
WiFiClient* apiClient = nullptr;

const char* statusText(int code) {
  switch (code) {
    case 200: return "OK"; case 201: return "Created"; case 202: return "Accepted";
    case 400: return "Bad Request"; case 401: return "Unauthorized"; case 403: return "Forbidden";
    case 404: return "Not Found"; case 405: return "Method Not Allowed"; case 408: return "Request Timeout";
    case 409: return "Conflict"; case 411: return "Length Required"; case 412: return "Precondition Failed";
    case 413: return "Content Too Large"; case 415: return "Unsupported Media Type";
    case 428: return "Precondition Required"; case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large"; case 503: return "Service Unavailable";
    default: return "Internal Server Error";
  }
}

void responseHeaders(int status, size_t length, const String& extra = "", bool chunked = false) {
  apiClient->printf("HTTP/1.1 %d %s\r\nContent-Type: application/json; charset=utf-8\r\n", status, statusText(status));
  apiClient->print(F("Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n"));
  if (chunked) apiClient->print(F("Transfer-Encoding: chunked\r\n"));
  else apiClient->printf("Content-Length: %u\r\n", static_cast<unsigned int>(length));
  if (status == 401) apiClient->print(F("WWW-Authenticate: Bearer realm=\"esp-relay\"\r\n"));
  if (status == 429) apiClient->print(F("Retry-After: 1\r\n"));
  if (status == 503) apiClient->print(F("Retry-After: 5\r\n"));
  apiClient->print(extra);
  apiClient->print(F("\r\n"));
}

void jsonResponse(int status, const JsonDocument& document, const String& extra = "") {
  if (document.overflowed()) {
    const char* fallback = "{\"error\":{\"code\":\"out_of_memory\",\"message\":\"Response allocation failed.\"}}";
    responseHeaders(500, strlen(fallback)); apiClient->print(fallback); return;
  }
  String body;
  const size_t size = measureJson(document);
  if (!body.reserve(size + 1)) {
    const char* fallback = "{\"error\":{\"code\":\"out_of_memory\",\"message\":\"Response allocation failed.\"}}";
    responseHeaders(500, strlen(fallback)); apiClient->print(fallback); return;
  }
  serializeJson(document, body);
  responseHeaders(status, body.length(), extra);
  apiClient->print(body);
}

void errorResponse(int status, const char* code, const String& message, const String& extra = "") {
  DynamicJsonDocument output(768);
  output["error"]["code"] = code; output["error"]["message"] = message;
  jsonResponse(status, output, extra);
}

void serviceResponse(int status, const String& error) {
  errorResponse(status, status == 409 ? "conflict" : status == 404 ? "not_found" :
    status == 500 ? "storage_failed" : status == 503 ? "not_ready" :
    status == 429 ? "rate_limited" : status == 403 ? "forbidden" : "invalid_request", error);
}

void pump() { updateTimer(); updateSchedules(); updateApiKeys(); yield(); }

bool readLine(WiFiClient& client, String& line, uint32_t startedAt) {
  line = "";
  while (millis() - startedAt < 5000UL && client.connected()) {
    if (!client.available()) { pump(); continue; }
    const char c = client.read();
    if (c == '\n') {
      if (!line.endsWith("\r")) { errorResponse(400, "invalid_headers", "Headers must use CRLF line endings."); return false; }
      line.remove(line.length() - 1); return true;
    }
    if (!c) { errorResponse(400, "invalid_headers", "Invalid header byte."); return false; }
    if (line.length() >= 512) { errorResponse(431, "headers_too_large", "A header line exceeds 512 bytes."); return false; }
    line += c;
  }
  errorResponse(408, "request_timeout", "Request headers timed out."); return false;
}

// Handle API requests before ESP8266WebServer buffers their bodies. Both headers
// and JSON have fixed limits; unauthenticated bodies are never buffered.
bool readRequest(WiFiClient& client, ApiRequest& request) {
  const uint32_t startedAt = millis();
  unsigned int headerBytes = 0;
  bool hasLength = false, hasAuthorization = false, hasType = false, hasMatch = false;
  while (true) {
    String line;
    if (!readLine(client, line, startedAt)) return false;
    headerBytes += line.length() + 2;
    if (headerBytes > 2048) { errorResponse(431, "headers_too_large", "Headers exceed 2048 bytes."); return false; }
    if (!line.length()) break;
    const int colon = line.indexOf(':');
    if (colon <= 0) { errorResponse(400, "invalid_headers", "Malformed header."); return false; }
    String name = line.substring(0, colon), value = line.substring(colon + 1);
    name.toLowerCase(); value.trim();
    for (unsigned int i = 0; i < name.length(); ++i) {
      const char c = name[i];
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
        errorResponse(400, "invalid_headers", "Invalid header name."); return false;
      }
    }
    bool duplicate = false;
    if (name == "content-length") {
      duplicate = hasLength; hasLength = true;
      uint32_t length;
      if (!parseApiUnsigned(value.c_str(), UINT32_MAX, length)) { errorResponse(400, "invalid_headers", "Invalid Content-Length."); return false; }
      if (length > MAX_API_BODY) { errorResponse(413, "body_too_large", "JSON bodies are limited to 4096 bytes."); return false; }
      request.contentLength = length;
    } else if (name == "authorization") { duplicate = hasAuthorization; hasAuthorization = true; request.authorization = value; }
    else if (name == "content-type") { duplicate = hasType; hasType = true; request.contentType = value; }
    else if (name == "if-match") { duplicate = hasMatch; hasMatch = true; request.ifMatch = value; }
    else if (name == "transfer-encoding" || name == "expect") {
      errorResponse(400, "unsupported_framing", "Use Content-Length without Transfer-Encoding or Expect."); return false;
    }
    if (duplicate) { errorResponse(400, "duplicate_header", "Duplicate authentication, framing or precondition header."); return false; }
  }
  const int authentication = authenticateApiKey(request.authorization);
  if (authentication != 200) {
    errorResponse(authentication, authentication == 503 ? "clock_unavailable" : "unauthorized",
      authentication == 503 ? "This expiring key requires NTP time. Wait for synchronization." : "A valid API bearer key is required.");
    return false;
  }
  const bool writing = request.method == "POST" || request.method == "PUT" || request.method == "PATCH";
  if (writing && !hasLength) { errorResponse(411, "length_required", "Send Content-Length with the JSON body."); return false; }
  if (writing) {
    String type = request.contentType;
    const int separator = type.indexOf(';'); if (separator >= 0) type = type.substring(0, separator);
    type.trim(); type.toLowerCase();
    if (type != "application/json") { errorResponse(415, "unsupported_media_type", "Use Content-Type: application/json."); return false; }
  } else if (request.contentLength) { errorResponse(400, "unexpected_body", "This method does not accept a body."); return false; }
  if (!request.body.reserve(request.contentLength + 1)) { errorResponse(503, "out_of_memory", "Request allocation failed."); return false; }
  while (request.body.length() < request.contentLength && millis() - startedAt < 10000UL && client.connected()) {
    unsigned int available = client.available();
    while (available-- && request.body.length() < request.contentLength) request.body += static_cast<char>(client.read());
    pump();
  }
  if (request.body.length() != request.contentLength) { errorResponse(408, "request_timeout", "Request body timed out."); return false; }
  return true;
}

bool allowedFields(JsonObjectConst object, std::initializer_list<const char*> allowed) {
  for (JsonPairConst field : object) {
    if (strlen(field.key().c_str()) != field.key().size()) {
      errorResponse(400, "invalid_request", "Field names cannot contain NUL characters."); return false;
    }
    bool known = false;
    for (const char* name : allowed) if (strcmp(field.key().c_str(), name) == 0) known = true;
    if (!known) { errorResponse(400, "unknown_field", String(F("Unknown field: ")) + field.key().c_str()); return false; }
  }
  return true;
}

bool stringField(JsonVariantConst field, String& value) {
  if (!field.is<const char*>()) return false;
  const JsonString text = field.as<JsonString>();
  value = text.c_str();
  return value.length() == text.size(); // Reject embedded NULs, including escaped ones.
}

bool uintField(JsonVariantConst field, uint32_t maximum, uint32_t& value) {
  if (!field.is<uint32_t>()) return false;
  value = field.as<uint32_t>(); return value <= maximum;
}

void relayJson(JsonObject output) {
  updateTimer();
  output["on"] = relayState; output["timerActive"] = timerActive;
  output["secondsRemaining"] = secondsRemaining();
}

void timeJson(JsonObject output) {
  const ScheduleSettings& settings = getScheduleSettings();
  output["ntpServer"] = settings.ntpServer; output["syncHours"] = settings.syncHours;
  output["timeZone"] = settings.timeZone; output["synced"] = scheduleClockSynced();
  if (scheduleClockSynced()) {
    const time_t now = time(nullptr); struct tm local; localtime_r(&now, &local);
    char formatted[80]; strftime(formatted, sizeof(formatted), "%Y-%m-%dT%H:%M:%S%z", &local);
    output["localTime"] = String(formatted); output["unixTime"] = static_cast<int64_t>(now);
  } else { output["localTime"] = nullptr; output["unixTime"] = nullptr; }
}

void configJson(JsonObject output) {
  output["displayName"] = displayName; output["mdnsName"] = mdnsName;
  output["startupDelaySeconds"] = startupDelaySeconds;
}

void wifiJson(JsonObject output) {
  output["ssid"] = wifiSsid; output["connected"] = WiFi.status() == WL_CONNECTED;
  output["ip"] = WiFi.localIP().toString(); output["operationId"] = wifiOperationId;
  output["operationStatus"] = wifiOperationStatus; output["message"] = wifiMessage;
  output["pending"] = wifiAttemptPending || testingWifi; output["saveFailed"] = wifiSaveFailed;
}

String scheduleEtag() { return String('"') + String(scheduleChecksum(getScheduleSettings()), HEX) + '"'; }

bool schedulePrecondition(const ApiRequest& request) {
  if (!request.ifMatch.length()) { errorResponse(428, "precondition_required", "Read /schedules and send its ETag in If-Match."); return false; }
  if (request.ifMatch != scheduleEtag()) { errorResponse(412, "precondition_failed", "Schedules changed. Read them again before writing."); return false; }
  return true;
}

void scheduleJson(JsonObject output, unsigned int id, const RebootSchedule& entry) {
  output["id"] = id; output["hour"] = entry.hour; output["minute"] = entry.minute;
  output["delaySeconds"] = entry.delaySeconds;
  JsonArray days = output.createNestedArray("days");
  for (unsigned int i = 0; i < 7; ++i) if (entry.weekdays & (1U << i)) days.add(i);
}

void schedulesResponse(int status = 200) {
  // Stream individual rows so a full replacement request and its response do
  // not require two large JSON documents in the ESP-01S heap at the same time.
  responseHeaders(status, 0, String(F("ETag: ")) + scheduleEtag() + F("\r\n"), true);
  auto sendPart = [](const String& body) {
    apiClient->printf("%x\r\n", static_cast<unsigned int>(body.length()));
    apiClient->print(body); apiClient->print(F("\r\n"));
  };
  sendPart("{\"schedules\":[");
  const ScheduleSettings& settings = getScheduleSettings();
  for (unsigned int i = 0; i < settings.count; ++i) {
    DynamicJsonDocument row(512);
    scheduleJson(row.to<JsonObject>(), i, settings.entries[i]);
    String part = i ? "," : "";
    serializeJson(row, part); sendPart(part); pump();
  }
  sendPart("]}"); apiClient->print(F("0\r\n\r\n"));
}

bool parseSchedule(JsonVariantConst value, RebootSchedule& entry) {
  if (!value.is<JsonObjectConst>()) { errorResponse(400, "invalid_schedule", "A schedule must be an object."); return false; }
  const JsonObjectConst object = value.as<JsonObjectConst>();
  if (!allowedFields(object, {"hour", "minute", "delaySeconds", "days"})) return false;
  uint32_t hour, minute, delay;
  if (!uintField(object["hour"], 23, hour) || !uintField(object["minute"], 55, minute) ||
      !uintField(object["delaySeconds"], 60, delay) || !object["days"].is<JsonArrayConst>()) {
    errorResponse(400, "invalid_schedule", "Provide integer hour, minute, delaySeconds and a days array."); return false;
  }
  const JsonArrayConst days = object["days"].as<JsonArrayConst>();
  if (days.size() > 7) { errorResponse(400, "invalid_days", "Use at most seven distinct day numbers, Sunday=0 through Saturday=6."); return false; }
  uint8_t mask = 0;
  for (JsonVariantConst day : days) {
    uint32_t number;
    if (!uintField(day, 6, number) || (mask & (1U << number))) { errorResponse(400, "invalid_days", "Use distinct day numbers, Sunday=0 through Saturday=6."); return false; }
    mask |= 1U << number;
  }
  entry = {static_cast<uint8_t>(hour), static_cast<uint8_t>(minute), static_cast<uint8_t>(delay), mask};
  if (!validRebootSchedule(entry)) { errorResponse(400, "invalid_schedule", "Minutes must use five-minute steps; delays are 5, 10, 15, 30 or 60 seconds."); return false; }
  return true;
}

void chunk(const String& body) {
  apiClient->printf("%x\r\n", static_cast<unsigned int>(body.length()));
  apiClient->print(body); apiClient->print(F("\r\n"));
}

void dispatch(const ApiRequest& request, JsonObjectConst input) {
  const String& path = request.path; const String& method = request.method;
  String error;
  if (path == "/api/v1" || path == "/api/v1/") {
    if (method != "GET") { errorResponse(405, "method_not_allowed", "Use GET.", "Allow: GET\r\n"); return; }
    DynamicJsonDocument output(768);
    output["version"] = 1; output["maxKeys"] = MAX_API_KEYS; output["maxSchedules"] = MAX_REBOOT_SCHEDULES;
    output["maxRequestBytes"] = MAX_API_BODY; output["authentication"] = "Bearer";
    output["resources"] = "/status, /config, /password, /wifi, /time, /timezones, /schedules, /relay, /timer, /keys";
    jsonResponse(200, output); return;
  }
  if (path == "/api/v1/status") {
    if (method != "GET") { errorResponse(405, "method_not_allowed", "Use GET.", "Allow: GET\r\n"); return; }
    DynamicJsonDocument output(2048);
    relayJson(output.createNestedObject("relay")); timeJson(output.createNestedObject("time"));
    wifiJson(output.createNestedObject("wifi")); output["uptimeSeconds"] = millis() / 1000UL;
    jsonResponse(200, output); return;
  }
  if (path == "/api/v1/config") {
    if (method != "GET" && method != "PATCH") { errorResponse(405, "method_not_allowed", "Use GET or PATCH.", "Allow: GET, PATCH\r\n"); return; }
    if (method == "PATCH") {
      if (!input.size() || !allowedFields(input, {"displayName", "mdnsName", "startupDelaySeconds"})) {
        if (!input.size()) errorResponse(400, "invalid_request", "Provide at least one setting.");
        return;
      }
      String name = displayName, host = mdnsName; uint32_t delay = startupDelaySeconds;
      if ((input.containsKey("displayName") && !stringField(input["displayName"], name)) ||
          (input.containsKey("mdnsName") && !stringField(input["mdnsName"], host)) ||
          (input.containsKey("startupDelaySeconds") && !uintField(input["startupDelaySeconds"], MAX_API_TIMER_SECONDS, delay))) {
        errorResponse(400, "invalid_request", "Use string names and an integer startupDelaySeconds."); return;
      }
      name.trim(); host.trim(); host.toLowerCase();
      if (!validDisplayName(name) || !validMdnsName(host) || !delay) { errorResponse(400, "invalid_request", "Invalid displayName, mdnsName or startupDelaySeconds."); return; }
      if (!saveDeviceConfiguration(name, host, delay)) { serviceResponse(500, "Could not save settings to flash."); return; }
    }
    DynamicJsonDocument output(512); configJson(output.to<JsonObject>()); jsonResponse(200, output); return;
  }
  if (path == "/api/v1/password") {
    if (method != "PUT") { errorResponse(405, "method_not_allowed", "Use PUT.", "Allow: PUT\r\n"); return; }
    if (!allowedFields(input, {"currentPassword", "newPassword"})) return;
    String current, replacement;
    if (!stringField(input["currentPassword"], current) || !stringField(input["newPassword"], replacement)) { errorResponse(400, "invalid_request", "Provide currentPassword and newPassword strings."); return; }
    const int status = changeAdminPassword(current, replacement, error);
    if (status != 200) { serviceResponse(status, error); return; }
    DynamicJsonDocument output(256); output["updated"] = true; jsonResponse(200, output); return;
  }
  if (path == "/api/v1/wifi") {
    if (method != "GET" && method != "PUT") { errorResponse(405, "method_not_allowed", "Use GET or PUT.", "Allow: GET, PUT\r\n"); return; }
    int status = 200;
    if (method == "PUT") {
      if (!allowedFields(input, {"ssid", "password"})) return;
      String ssid, password;
      if (!stringField(input["ssid"], ssid) || !stringField(input["password"], password)) { errorResponse(400, "invalid_request", "Provide ssid and password strings. Use an empty password for an open network."); return; }
      status = queueWifiConfiguration(ssid, password, error);
      if (status != 202) { serviceResponse(status, error); return; }
    }
    DynamicJsonDocument output(1024); wifiJson(output.to<JsonObject>()); jsonResponse(status, output); return;
  }
  if (path == "/api/v1/time") {
    if (method != "GET" && method != "PATCH") { errorResponse(405, "method_not_allowed", "Use GET or PATCH.", "Allow: GET, PATCH\r\n"); return; }
    if (method == "PATCH") {
      if (!input.size() || !allowedFields(input, {"ntpServer", "syncHours", "timeZone"})) {
        if (!input.size()) errorResponse(400, "invalid_request", "Provide at least one time setting.");
        return;
      }
      const ScheduleSettings& settings = getScheduleSettings();
      String host = settings.ntpServer, zone = settings.timeZone; uint32_t hours = settings.syncHours;
      if ((input.containsKey("ntpServer") && !stringField(input["ntpServer"], host)) ||
          (input.containsKey("timeZone") && !stringField(input["timeZone"], zone)) ||
          (input.containsKey("syncHours") && !uintField(input["syncHours"], 6, hours))) {
        errorResponse(400, "invalid_request", "Use string ntpServer/timeZone and an integer syncHours from 1 to 6."); return;
      }
      host.trim(); const int status = setScheduleTimeSettings(host, hours, zone, error);
      if (status != 200) { serviceResponse(status, error); return; }
    }
    DynamicJsonDocument output(1024); timeJson(output.to<JsonObject>()); jsonResponse(200, output); return;
  }
  if (path == "/api/v1/timezones") {
    if (method != "GET") { errorResponse(405, "method_not_allowed", "Use GET.", "Allow: GET\r\n"); return; }
    responseHeaders(200, 0, "", true); chunk("{\"timeZones\":[");
    bool first = true; String buffer;
    forEachTimeZone([&](const String& zone) {
      if (!first) buffer += ',';
      first = false;
      buffer += '"'; buffer += zone; buffer += '"'; // Database names contain no JSON metacharacters.
      if (buffer.length() >= 768) { chunk(buffer); buffer = ""; }
    });
    buffer += "]}"; chunk(buffer); apiClient->print(F("0\r\n\r\n")); return;
  }
  if (path == "/api/v1/schedules" || path.startsWith("/api/v1/schedules/")) {
    const bool collection = path == "/api/v1/schedules";
    uint32_t id = 0;
    if (!collection && (!parseApiUnsigned(path.substring(18).c_str(), MAX_REBOOT_SCHEDULES - 1, id) || id >= getScheduleSettings().count)) {
      errorResponse(404, "not_found", "Schedule not found."); return;
    }
    const bool allowed = method == "GET" || method == "PUT" || (collection ? method == "POST" : method == "DELETE");
    if (!allowed) { errorResponse(405, "method_not_allowed", "Unsupported schedule method.", collection ? "Allow: GET, POST, PUT\r\n" : "Allow: GET, PUT, DELETE\r\n"); return; }
    if (method == "GET") {
      if (collection) schedulesResponse();
      else { DynamicJsonDocument output(512); scheduleJson(output.to<JsonObject>(), id, getScheduleSettings().entries[id]); jsonResponse(200, output, String(F("ETag: ")) + scheduleEtag() + F("\r\n")); }
      return;
    }
    if (!schedulePrecondition(request)) return;
    int status;
    if (method == "DELETE") status = removeRebootSchedule(id, error);
    else if (collection && method == "PUT") {
      if (!allowedFields(input, {"schedules"})) return;
      if (!input["schedules"].is<JsonArrayConst>() || input["schedules"].size() > MAX_REBOOT_SCHEDULES) { errorResponse(400, "invalid_request", "Provide a schedules array of at most 32 entries."); return; }
      RebootSchedule entries[MAX_REBOOT_SCHEDULES] = {}; unsigned int count = 0;
      for (JsonVariantConst entry : input["schedules"].as<JsonArrayConst>()) if (!parseSchedule(entry, entries[count++])) return;
      status = replaceRebootSchedules(entries, count, error);
    } else {
      RebootSchedule entry;
      if (!parseSchedule(input, entry)) return;
      status = putRebootSchedule(collection ? getScheduleSettings().count : id, entry, error);
    }
    if (status >= 400) { serviceResponse(status, error); return; }
    schedulesResponse(status); return;
  }
  if (path == "/api/v1/relay" || path == "/api/v1/timer") {
    const bool relay = path.endsWith("/relay");
    if ((relay && method != "GET" && method != "PUT") || (!relay && method != "POST" && method != "DELETE")) {
      errorResponse(405, "method_not_allowed", "Unsupported control method.", relay ? "Allow: GET, PUT\r\n" : "Allow: POST, DELETE\r\n"); return;
    }
    if (relay && method == "PUT") {
      if (!allowedFields(input, {"on"})) return;
      if (!input["on"].is<bool>()) { errorResponse(400, "invalid_request", "Provide on as a JSON boolean."); return; }
      timerActive = false; setRelay(input["on"].as<bool>());
    } else if (!relay && method == "POST") {
      if (!allowedFields(input, {"seconds"})) return;
      uint32_t seconds;
      if (!uintField(input["seconds"], MAX_API_TIMER_SECONDS, seconds) || !seconds) { errorResponse(400, "invalid_request", "seconds must be an integer from 1 to 4294967."); return; }
      setRelay(false); timerStartedAt = millis(); timerDuration = seconds * 1000UL; timerActive = true;
    } else if (!relay && method == "DELETE") timerActive = false;
    DynamicJsonDocument output(256); relayJson(output.to<JsonObject>()); jsonResponse(200, output); return;
  }
  if (path == "/api/v1/keys" || path.startsWith("/api/v1/keys/")) {
    const bool collection = path == "/api/v1/keys";
    if ((collection && method != "GET" && method != "POST") || (!collection && method != "DELETE")) {
      errorResponse(405, "method_not_allowed", "Unsupported key method.", collection ? "Allow: GET, POST\r\n" : "Allow: DELETE\r\n"); return;
    }
    String token, id;
    int status = 200;
    if (!collection) {
      status = revokeApiKey(path.substring(13), error);
      if (status != 200) { serviceResponse(status, error); return; }
    } else if (method == "POST") {
      if (!allowedFields(input, {"label", "expiration"})) return;
      String label, mode, date; uint32_t days = 0; ApiKeyExpiration expiration;
      if (!stringField(input["label"], label) || !input["expiration"].is<JsonObjectConst>()) { errorResponse(400, "invalid_request", "Provide a label and expiration object."); return; }
      const JsonObjectConst choice = input["expiration"].as<JsonObjectConst>();
      if (!stringField(choice["mode"], mode)) { errorResponse(400, "invalid_expiration", "expiration.mode must be none, days or date."); return; }
      if (mode == "none") { if (!allowedFields(choice, {"mode"})) return; expiration = ApiKeyExpiration::None; }
      else if (mode == "days") {
        if (!allowedFields(choice, {"mode", "days"})) return;
        if (!uintField(choice["days"], 30, days) || !validApiKeyDays(days)) { errorResponse(400, "invalid_expiration", "days must be an integer from 1 to 30."); return; }
        expiration = ApiKeyExpiration::Days;
      } else if (mode == "date") {
        if (!allowedFields(choice, {"mode", "date"})) return;
        if (!stringField(choice["date"], date)) { errorResponse(400, "invalid_expiration", "Provide expiration.date as YYYY-MM-DD."); return; }
        expiration = ApiKeyExpiration::Date;
      } else { errorResponse(400, "invalid_expiration", "expiration.mode must be none, days or date."); return; }
      status = createApiKey(label, expiration, days, date, token, id, error);
      if (status != 201) { serviceResponse(status, error); return; }
    }
    DynamicJsonDocument output(4096); apiKeysJson(output.createNestedArray("keys"));
    if (token.length()) { output["token"] = token; output["id"] = id; }
    jsonResponse(status, output); return;
  }
  errorResponse(404, "not_found", "Unknown API resource.");
}
} // namespace

void registerApi() {
  server.addHook([](const String& method, const String& path, WiFiClient* client,
                    ESP8266WebServer::ContentTypeFunction) {
    if (path != "/api" && !path.startsWith("/api/")) return ESP8266WebServer::CLIENT_REQUEST_CAN_CONTINUE;
    apiClient = client;
    ApiRequest request; request.method = method; request.path = path;
    if (readRequest(*client, request)) {
      const size_t capacity = request.body.length() ?
        std::min<size_t>(8192, std::max<size_t>(512, request.body.length() * 3)) : 0;
      DynamicJsonDocument input(capacity);
      bool valid = true;
      if (request.body.length()) {
        const DeserializationError result = deserializeJson(input, request.body, DeserializationOption::NestingLimit(5));
        if (result || !input.is<JsonObject>() || !apiJsonObjectEnvelope(request.body.c_str(), request.body.length())) {
          errorResponse(400, "invalid_json", "Send one valid JSON object with at most five nesting levels."); valid = false;
        }
      } else if (method == "POST" || method == "PUT" || method == "PATCH") {
        errorResponse(400, "invalid_json", "A JSON object body is required."); valid = false;
      }
      if (valid) {
        const int authentication = authenticateApiKey(request.authorization);
        if (authentication != 200) errorResponse(authentication, authentication == 503 ? "clock_unavailable" : "unauthorized", "The API key is no longer usable.");
        else dispatch(request, input.as<JsonObjectConst>());
      }
    }
    client->flush(1000); client->stop(); apiClient = nullptr;
    return ESP8266WebServer::CLIENT_REQUEST_IS_HANDLED;
  });
}
