#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <coredecls.h>
#include <time.h>

#include "reboot_schedule.h"
#include "schedules.h"
#include "time_zones.h"

extern ESP8266WebServer server;
extern bool timerActive;
extern unsigned long timerStartedAt;
extern unsigned long timerDuration;
extern String displayName;
extern String escapeHtml(const String& value);
extern String csrfInput();
extern void setRelay(bool on);
extern void updateTimer();
extern void redirectTo(const String& location);
extern void addProtectedRoute(const char* uri, HTTPMethod method, std::function<void()> handler);

namespace {
constexpr uint32_t SCHEDULE_SETTINGS_MAGIC = 0x53434801;
struct ScheduleSettings {
  uint32_t magic;
  uint32_t checksum;
  char ntpServer[254];
  char timeZone[49];
  uint8_t syncHours;
  uint8_t count;
  RebootSchedule entries[MAX_REBOOT_SCHEDULES];
};
static_assert(SCHEDULE_SETTINGS_OFFSET + sizeof(ScheduleSettings) <= SETTINGS_EEPROM_SIZE,
              "Schedule settings exceed EEPROM allocation");
ScheduleSettings settings = {};
int lastRunDays[MAX_REBOOT_SCHEDULES] = {};
bool clockSynced = false;
time_t lastCheckedSecond = 0;

// Read the flash table one entry at a time; never copy the database into RAM.
bool nextTimeZone(size_t& position, String& name, String& rule) {
  if (!pgm_read_byte(TIME_ZONE_DATA + position)) return false;
  name = "";
  rule = "";
  char c;
  while ((c = pgm_read_byte(TIME_ZONE_DATA + position++)) != '\t') name += c;
  while ((c = pgm_read_byte(TIME_ZONE_DATA + position++)) != '\n') rule += c;
  return true;
}

bool findTimeZone(const String& selected, String& rule) {
  size_t position = 0;
  String name;
  while (nextTimeZone(position, name, rule)) {
    if (name == selected) return true;
  }
  return false;
}

bool validNtpServer(const String& host) {
  if (!host.length() || host.length() > 253) return false;
  unsigned int labelLength = 0;
  char previous = 0;
  for (unsigned int i = 0; i < host.length(); ++i) {
    const char c = host[i];
    if (c == '.') {
      if (!labelLength || previous == '-') return false;
      labelLength = 0;
    } else {
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-')) return false;
      if ((!labelLength && c == '-') || ++labelLength > 63) return false;
    }
    previous = c;
  }
  return labelLength > 0 && previous != '-';
}

// Reject signs, decimals, whitespace, empty values and integer overflow.
bool parseNumber(const String& input, unsigned int maximum, unsigned int& value) {
  value = 0;
  if (!input.length()) return false;
  for (unsigned int i = 0; i < input.length(); ++i) {
    if (input[i] < '0' || input[i] > '9') return false;
    const unsigned int digit = input[i] - '0';
    if (digit > maximum || value > (maximum - digit) / 10) return false;
    value = value * 10 + digit;
  }
  return true;
}

uint32_t scheduleChecksum(const ScheduleSettings& saved) {
  uint32_t hash = 2166136261UL;
  for (char c : saved.ntpServer) hash = (hash ^ static_cast<uint8_t>(c)) * 16777619UL;
  for (char c : saved.timeZone) hash = (hash ^ static_cast<uint8_t>(c)) * 16777619UL;
  hash = (hash ^ saved.syncHours) * 16777619UL;
  hash = (hash ^ saved.count) * 16777619UL;
  for (const RebootSchedule& entry : saved.entries) {
    hash = (hash ^ entry.hour) * 16777619UL;
    hash = (hash ^ entry.minute) * 16777619UL;
    hash = (hash ^ entry.delaySeconds) * 16777619UL;
  }
  return hash;
}

bool persistSchedules(ScheduleSettings& proposed) {
  proposed.magic = SCHEDULE_SETTINGS_MAGIC;
  proposed.checksum = scheduleChecksum(proposed);
  ScheduleSettings previous = {};
  EEPROM.get(SCHEDULE_SETTINGS_OFFSET, previous);
  EEPROM.put(SCHEDULE_SETTINGS_OFFSET, proposed);
  if (!EEPROM.commit()) {
    // Other settings share this EEPROM buffer; do not accidentally save a failed write later.
    EEPROM.put(SCHEDULE_SETTINGS_OFFSET, previous);
    return false;
  }
  settings = proposed;
  return true;
}

String localClockText() {
  if (!clockSynced) return F("Waiting for NTP sync. Schedules are paused until the time is set.");
  const time_t now = time(nullptr);
  struct tm local;
  localtime_r(&now, &local);
  char formatted[80];
  strftime(formatted, sizeof(formatted), "%Y-%m-%d %H:%M:%S %Z (UTC%z)", &local);
  return String(formatted);
}

String twoDigits(unsigned int value) {
  return (value < 10 ? String('0') : String()) + String(value);
}

void appendOption(String& html, unsigned int value, unsigned int selected, bool padded) {
  html += F("<option value=\"");
  html += String(value);
  html += value == selected ? F("\" selected>") : F("\">");
  html += padded ? twoDigits(value) : String(value);
  html += F("</option>");
}

void sendSchedulesPage(const String& message = "", int status = 200, int edit = -1,
                       bool showNew = false) {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(status, "text/html; charset=utf-8", "");
  String html;
  html.reserve(4000);
  html = F(R"HTML(<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1"><title>Schedules</title>
<style>
body{font-family:Arial,sans-serif;max-width:540px;margin:40px auto;padding:0 16px;background:#f4f4f4}
label{display:block;margin-top:20px}input,select{box-sizing:border-box;width:100%;padding:12px;font-size:18px;margin-top:8px}
button,.button{display:inline-block;padding:12px 20px;font-size:18px;color:white;background:#1976d2;border:0;border-radius:8px;cursor:pointer;text-decoration:none}
.back{background:#555}.delete{background:#b71c1c}.actions{display:flex;gap:10px;flex-wrap:wrap;align-items:center}.actions form{margin:0}
article{padding:16px;background:white;border-radius:8px;margin:16px 0}section{margin:28px 0}.help{color:#555}
#local-time{font-weight:bold;overflow-wrap:anywhere}.error{color:#b71c1c}
</style></head><body><h2>Schedules</h2><p>)HTML");
  html += escapeHtml(displayName);
  html += F("</p>");
  if (message.length()) {
    html += status >= 400 ? F("<p class=\"error\" role=\"alert\">") : F("<p role=\"status\">");
    html += escapeHtml(message);
    html += F("</p>");
  }
  html += F("<section><h3>Time settings</h3><form action=\"/schedules/time\" method=\"post\">");
  html += csrfInput();
  html += F("<label for=\"ntpServer\">NTP server</label><input id=\"ntpServer\" name=\"ntpServer\" type=\"text\" maxlength=\"253\" autocapitalize=\"none\" spellcheck=\"false\" required value=\"");
  html += escapeHtml(settings.ntpServer);
  html += F("\"><label for=\"syncHours\">NTP sync interval (hours)</label><input id=\"syncHours\" name=\"syncHours\" type=\"number\" min=\"1\" max=\"6\" step=\"1\" required value=\"");
  html += String(settings.syncHours);
  html += F("\"><p class=\"help\">Whole numbers from 1 to 6. A value of 1 syncs every hour.</p><label for=\"timeZone\">Time zone</label><select id=\"timeZone\" name=\"timeZone\" required>");
  server.sendContent(html);
  html = "";

  size_t position = 0;
  String zone, rule;
  while (nextTimeZone(position, zone, rule)) {
    html += F("<option value=\"");
    html += escapeHtml(zone);
    html += zone == settings.timeZone ? F("\" selected>") : F("\">");
    html += escapeHtml(zone);
    html += F("</option>");
    if (html.length() >= 768) {
      server.sendContent(html);
      html = "";
      updateTimer();
      yield();
    }
  }
  html += F("</select><p class=\"help\">Save to apply the selected time zone. Daylight saving time follows the selected zone's rules.</p><p>The ESP8266 does not have a battery-backed real-time clock (RTC), and its time can drift significantly between NTP syncs. Wi-Fi and a reachable NTP server are required to sync after power loss.</p><p>Local time (saved time zone):</p><p id=\"local-time\" role=\"status\">");
  html += escapeHtml(localClockText());
  html += F("</p><button type=\"submit\">Save</button></form></section><section><h3>Daily reboot schedules</h3><p>At each saved local time, the relay turns OFF, then ON after the selected power on delay.</p>");
  server.sendContent(html);
  html = "";
  for (uint8_t i = 0; i < settings.count; ++i) {
    const RebootSchedule& entry = settings.entries[i];
    html += F("<article><p><strong>Every day at ");
    html += twoDigits(entry.hour) + ':' + twoDigits(entry.minute);
    html += F("</strong><br>Power on delay: ");
    html += String(entry.delaySeconds);
    html += F(" seconds</p><div class=\"actions\"><a class=\"button\" href=\"/schedules?edit=");
    html += String(i);
    html += F("#scheduleForm\">Edit</a><form action=\"/schedules/delete\" method=\"post\">");
    html += csrfInput();
    html += F("<input type=\"hidden\" name=\"id\" value=\"");
    html += String(i);
    html += F("\"><button class=\"delete\" type=\"submit\">Delete</button></form></div></article>");
    server.sendContent(html);
    html = "";
    updateTimer();
    yield();
  }
  if (settings.count == 0 || showNew || edit >= 0) {
    const RebootSchedule entry = edit >= 0 ? settings.entries[edit] : RebootSchedule{0, 0, 5};
    html += F("<form id=\"scheduleForm\" action=\"/schedules/save\" method=\"post\"><h3>");
    html += edit >= 0 ? F("Edit schedule") : F("New schedule");
    html += F("</h3>");
    html += csrfInput();
    html += F("<input type=\"hidden\" name=\"id\" value=\"");
    html += edit >= 0 ? String(edit) : String("new");
    html += F("\"><label for=\"hour\">Hour (24-hour clock)</label><select id=\"hour\" name=\"hour\">");
    for (unsigned int value = 0; value < 24; ++value) appendOption(html, value, entry.hour, true);
    html += F("</select><label for=\"minute\">Minute</label><select id=\"minute\" name=\"minute\">");
    for (unsigned int value = 0; value < 60; value += 5) appendOption(html, value, entry.minute, true);
    html += F("</select><label for=\"delaySeconds\">Power on delay (seconds)</label><select id=\"delaySeconds\" name=\"delaySeconds\">");
    for (uint8_t value : REBOOT_DELAYS) appendOption(html, value, entry.delaySeconds, false);
    html += F("</select><p><button type=\"submit\">Save</button></p>");
    if (settings.count) html += F("<a href=\"/schedules\">Cancel</a>");
    html += F("</form>");
  } else if (settings.count < MAX_REBOOT_SCHEDULES) {
    html += F("<p><a class=\"button\" href=\"/schedules?new=1#scheduleForm\">New schedule</a></p>");
  } else {
    html += F("<p>All 32 schedule slots are in use. Edit or delete a schedule to make room.</p>");
  }
  html += F(R"HTML(<p class="help">Settings and schedules survive power loss. Schedules wait for NTP after boot. Missed times are skipped. A repeated local time runs once per day. Schedules due during an existing power on countdown are skipped.</p>
</section><p><a class="button back" href="/">Back to relay</a></p>
<script>
async function refreshClock(){
  try{
    const response=await fetch('/clock',{cache:'no-store'});
    if(response.status===401){location.replace('/login');return;}
    if(!response.ok)throw new Error();
    document.getElementById('local-time').textContent=await response.text();
  }catch(error){document.getElementById('local-time').textContent='Clock unavailable. Reconnecting...';}
  setTimeout(refreshClock,1000);
}
refreshClock();
</script></body></html>)HTML");
  server.sendContent(html);
  server.sendContent("");
}

void handleSchedules() {
  if (server.hasArg("edit")) {
    unsigned int id;
    if (!parseNumber(server.arg("edit"), MAX_REBOOT_SCHEDULES - 1, id) || id >= settings.count) {
      sendSchedulesPage("Schedule not found.", 404);
      return;
    }
    sendSchedulesPage("", 200, id);
    return;
  }
  sendSchedulesPage("", 200, -1, settings.count < MAX_REBOOT_SCHEDULES && server.arg("new") == "1");
}

void handleSaveTime() {
  String host = server.arg("ntpServer");
  host.trim();
  const String zone = server.arg("timeZone");
  unsigned int hours;
  String rule;
  if (!validNtpServer(host) || !parseNumber(server.arg("syncHours"), 6, hours) ||
      hours < 1 || !findTimeZone(zone, rule)) {
    sendSchedulesPage("Enter a valid NTP hostname or IPv4 address, a whole number of hours from 1 to 6, and a listed time zone.", 400);
    return;
  }
  ScheduleSettings proposed = settings;
  host.toCharArray(proposed.ntpServer, sizeof(proposed.ntpServer));
  zone.toCharArray(proposed.timeZone, sizeof(proposed.timeZone));
  proposed.syncHours = hours;
  if (!persistSchedules(proposed)) {
    sendSchedulesPage("Could not save time settings to flash. Please try again.", 500);
    return;
  }
  syncScheduleClock(); // Restart SNTP so a changed interval takes effect immediately.
  redirectTo("/schedules");
}

void handleSaveSchedule() {
  const bool adding = server.arg("id") == "new";
  unsigned int id = settings.count, hour, minute, delay;
  if ((!adding && (!parseNumber(server.arg("id"), MAX_REBOOT_SCHEDULES - 1, id) || id >= settings.count)) ||
      (adding && settings.count >= MAX_REBOOT_SCHEDULES)) {
    sendSchedulesPage("Schedule not found, or all schedule slots are in use.", 400);
    return;
  }
  if (!parseNumber(server.arg("hour"), 23, hour) ||
      !parseNumber(server.arg("minute"), 55, minute) ||
      !parseNumber(server.arg("delaySeconds"), 60, delay) ||
      !validRebootSchedule({static_cast<uint8_t>(hour), static_cast<uint8_t>(minute), static_cast<uint8_t>(delay)})) {
    sendSchedulesPage("Choose an hour from 00 to 23, a minute in 5-minute steps, and a power on delay of 5, 10, 15, 30 or 60 seconds.", 400, adding ? -1 : id, adding);
    return;
  }
  for (uint8_t i = 0; i < settings.count; ++i) {
    if (i != id && settings.entries[i].hour == hour && settings.entries[i].minute == minute) {
      sendSchedulesPage("A schedule already exists at that time. Edit that schedule or choose another time.", 409, adding ? -1 : id, adding);
      return;
    }
  }
  ScheduleSettings proposed = settings;
  proposed.entries[id] = {static_cast<uint8_t>(hour), static_cast<uint8_t>(minute), static_cast<uint8_t>(delay)};
  if (adding) ++proposed.count;
  if (!persistSchedules(proposed)) {
    sendSchedulesPage("Could not save the schedule to flash. Please try again.", 500, adding ? -1 : id, adding);
    return;
  }
  // Keep an edited schedule's daily guard, so saving it cannot repeat a power cycle.
  if (adding) lastRunDays[id] = 0;
  redirectTo("/schedules");
}

void handleDeleteSchedule() {
  unsigned int id;
  if (!parseNumber(server.arg("id"), MAX_REBOOT_SCHEDULES - 1, id) || id >= settings.count) {
    sendSchedulesPage("Schedule not found.", 404);
    return;
  }
  ScheduleSettings proposed = settings;
  for (uint8_t i = id + 1; i < proposed.count; ++i) proposed.entries[i - 1] = proposed.entries[i];
  proposed.entries[--proposed.count] = {};
  if (!persistSchedules(proposed)) {
    sendSchedulesPage("Could not delete the schedule from flash. Please try again.", 500);
    return;
  }
  for (uint8_t i = id + 1; i <= settings.count; ++i) lastRunDays[i - 1] = lastRunDays[i];
  lastRunDays[settings.count] = 0;
  redirectTo("/schedules");
}
} // namespace

// ESP8266 lwIP calls this hook after each successful sync (milliseconds).
uint32_t sntp_update_delay_MS_rfc_not_less_than_15000() {
  return static_cast<uint32_t>(settings.syncHours) * 3600000UL;
}

void loadScheduleSettings() {
  strcpy(settings.ntpServer, "pool.ntp.org");
  strcpy(settings.timeZone, "Etc/UTC");
  settings.syncHours = 1;
  ScheduleSettings saved = {};
  EEPROM.get(SCHEDULE_SETTINGS_OFFSET, saved);
  String rule;
  if (saved.magic != SCHEDULE_SETTINGS_MAGIC || saved.checksum != scheduleChecksum(saved) ||
      !memchr(saved.ntpServer, '\0', sizeof(saved.ntpServer)) ||
      !memchr(saved.timeZone, '\0', sizeof(saved.timeZone)) ||
      !validNtpServer(saved.ntpServer) || !findTimeZone(saved.timeZone, rule) ||
      saved.syncHours < 1 || saved.syncHours > 6 || saved.count > MAX_REBOOT_SCHEDULES) return;
  for (uint8_t i = 0; i < saved.count; ++i) {
    if (!validRebootSchedule(saved.entries[i])) return;
    for (uint8_t j = 0; j < i; ++j) {
      if (saved.entries[i].hour == saved.entries[j].hour &&
          saved.entries[i].minute == saved.entries[j].minute) return;
    }
  }
  settings = saved;
}

void beginScheduleClock() {
  settimeofday_cb([](bool fromSntp) {
    if (fromSntp && time(nullptr) >= 1704067200) {
      clockSynced = true;
      Serial.println("NTP time synchronized.");
    }
  });
  syncScheduleClock();
}

void syncScheduleClock() {
  String rule;
  if (findTimeZone(settings.timeZone, rule)) configTime(rule.c_str(), settings.ntpServer);
}

void registerScheduleRoutes() {
  addProtectedRoute("/schedules", HTTP_GET, handleSchedules);
  addProtectedRoute("/schedules/time", HTTP_POST, handleSaveTime);
  addProtectedRoute("/schedules/save", HTTP_POST, handleSaveSchedule);
  addProtectedRoute("/schedules/delete", HTTP_POST, handleDeleteSchedule);
  addProtectedRoute("/clock", HTTP_GET, []() { server.send(200, "text/plain; charset=utf-8", localClockText()); });
}

void updateSchedules() {
  if (!clockSynced) return;
  const time_t now = time(nullptr);
  if (now == lastCheckedSecond) return;
  lastCheckedSecond = now;
  struct tm local;
  localtime_r(&now, &local);
  const int day = local.tm_year * 366 + local.tm_yday + 1;
  for (uint8_t i = 0; i < settings.count; ++i) {
    if (!rebootScheduleDue(settings.entries[i], local.tm_hour, local.tm_min, day, lastRunDays[i])) continue;
    // Never replace an existing countdown, which could leave the load off longer.
    if (timerActive) continue;
    setRelay(false);
    timerStartedAt = millis();
    timerDuration = settings.entries[i].delaySeconds * 1000UL;
    timerActive = true;
    Serial.printf("Scheduled relay reboot: %02u:%02u, power on in %u seconds\n",
                  settings.entries[i].hour, settings.entries[i].minute, settings.entries[i].delaySeconds);
  }
}
