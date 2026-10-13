#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <EEPROM.h>

// --- UPDATE THESE WITH YOUR WI-FI DETAILS ---
const char* ssid = "SSID";
const char* password = "PASSWORD";

// This working relay board is active LOW on GPIO0.
const int RELAY_PIN = 0;
// ESP-01S onboard LED: GPIO2, active LOW (do not use the relay pin).
const int STATUS_LED_PIN = 2;
static_assert(RELAY_PIN != STATUS_LED_PIN, "Relay and LED need separate pins");
const unsigned long AUTO_TURN_ON_DELAY = 60000UL;
const unsigned long LED_BLINK_INTERVAL = 1000UL;
const unsigned long MAX_TIMER_SECONDS = 4294967UL; // Fits in 32-bit milliseconds.

bool relayState = false;
bool timerActive = true;
unsigned long timerStartedAt = 0; // Startup countdown begins at boot.
unsigned long timerDuration = AUTO_TURN_ON_DELAY;
bool ledOn = true;
unsigned long lastLedChange = 0;
bool wifiWasConnected = false;
bool mdnsStarted = false;

ESP8266WebServer server(80);

const size_t MAX_DISPLAY_NAME_LENGTH = 64;
const size_t MAX_MDNS_NAME_LENGTH = 63;
const uint32_t SETTINGS_MAGIC = 0x524C5901; // RLY, settings format version 1.
struct DeviceSettings {
  uint32_t magic;
  uint32_t checksum;
  char displayName[MAX_DISPLAY_NAME_LENGTH + 1];
  char mdnsName[MAX_MDNS_NAME_LENGTH + 1];
};
String displayName = "ESP-01S Relay Controller";
String mdnsName = "esp-relay";
bool settingsPersisted = false;

bool validDisplayName(const String& name) {
  if (name.length() == 0 || name.length() > MAX_DISPLAY_NAME_LENGTH) return false;
  for (unsigned int i = 0; i < name.length(); ++i) {
    const uint8_t c = static_cast<uint8_t>(name[i]);
    if (c < 32 || c == 127) return false;
  }
  return true;
}

bool validMdnsName(const String& name) {
  if (name.length() == 0 || name.length() > MAX_MDNS_NAME_LENGTH) return false;
  if (name[0] == '-' || name[name.length() - 1] == '-') return false;
  for (unsigned int i = 0; i < name.length(); ++i) {
    const char c = name[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
  }
  return true;
}

uint32_t settingsChecksum(const DeviceSettings& settings) {
  // FNV-1a detects incomplete/corrupt stored names; do not hash struct padding.
  uint32_t hash = 2166136261UL;
  for (char c : settings.displayName) hash = (hash ^ static_cast<uint8_t>(c)) * 16777619UL;
  for (char c : settings.mdnsName) hash = (hash ^ static_cast<uint8_t>(c)) * 16777619UL;
  return hash;
}

void loadSettings() {
  EEPROM.begin(sizeof(DeviceSettings));
  DeviceSettings saved = {};
  EEPROM.get(0, saved);
  if (saved.magic != SETTINGS_MAGIC || saved.checksum != settingsChecksum(saved) ||
      !memchr(saved.displayName, '\0', sizeof(saved.displayName)) ||
      !memchr(saved.mdnsName, '\0', sizeof(saved.mdnsName)) ||
      !validDisplayName(String(saved.displayName)) || !validMdnsName(String(saved.mdnsName))) {
    Serial.println("No valid saved configuration. Using default names.");
    return;
  }
  displayName = saved.displayName;
  mdnsName = saved.mdnsName;
  settingsPersisted = true;
}

bool saveSettings(const String& newDisplayName, const String& newMdnsName) {
  // Write only on a changed Save, never during polling or at each boot.
  if (settingsPersisted && newDisplayName == displayName && newMdnsName == mdnsName) return true;
  DeviceSettings saved = {};
  saved.magic = SETTINGS_MAGIC;
  newDisplayName.toCharArray(saved.displayName, sizeof(saved.displayName));
  newMdnsName.toCharArray(saved.mdnsName, sizeof(saved.mdnsName));
  saved.checksum = settingsChecksum(saved);
  EEPROM.put(0, saved);
  if (!EEPROM.commit()) return false;
  displayName = newDisplayName;
  mdnsName = newMdnsName;
  settingsPersisted = true;
  return true;
}

String escapeHtml(const String& value) {
  String escaped;
  escaped.reserve(value.length() + 16);
  for (unsigned int i = 0; i < value.length(); ++i) {
    switch (value[i]) {
      case '&': escaped += F("&amp;"); break;
      case '<': escaped += F("&lt;"); break;
      case '>': escaped += F("&gt;"); break;
      case '"': escaped += F("&quot;"); break;
      case '\'': escaped += F("&#39;"); break;
      default: escaped += value[i];
    }
  }
  return escaped;
}

void restartMdns() {
  if (mdnsStarted) MDNS.close();
  mdnsStarted = false;
  if (WiFi.status() != WL_CONNECTED) return;
  mdnsStarted = MDNS.begin(mdnsName.c_str());
  if (mdnsStarted) {
    MDNS.addService("http", "tcp", 80);
    Serial.print("Also available at http://");
    Serial.print(mdnsName);
    Serial.println(".local");
  } else {
    Serial.println("mDNS could not start. Use the device IP address.");
  }
}

void setRelay(bool on) {
  relayState = on;
  digitalWrite(RELAY_PIN, on ? LOW : HIGH);
  Serial.println(on ? "Relay: ON" : "Relay: OFF");
}

void updateTimer() {
  // Unsigned subtraction also works when millis() wraps around.
  if (timerActive && millis() - timerStartedAt >= timerDuration) {
    timerActive = false;
    setRelay(true);
    Serial.println("Relay timer completed.");
  }
}

unsigned long secondsRemaining() {
  if (!timerActive) return 0;
  const unsigned long elapsed = millis() - timerStartedAt;
  if (elapsed >= timerDuration) return 0;
  const unsigned long remaining = timerDuration - elapsed;
  return remaining / 1000UL + (remaining % 1000UL != 0);
}

String getHTML() {
  String html;
  html.reserve(5000);
  html = F(R"HTML(<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>)HTML");
  html += escapeHtml(displayName);
  html += F(R"HTML(</title>
<style>
body { font-family: Arial, sans-serif; text-align: center; margin: 50px 16px; background: #f4f4f4; }
button { padding: 16px 28px; font-size: 20px; cursor: pointer; color: white; border-radius: 8px; border: none; background: #1976d2; }
.btn-on { background: #f44336; } .btn-off { background: #4caf50; }
input { padding: 12px; font-size: 20px; width: 160px; }
form { margin: 24px 0; } #error { color: #b71c1c; }
.config-button { display: inline-block; padding: 12px 24px; color: white; background: #555; border-radius: 8px; text-decoration: none; }
</style></head><body>
<h2>)HTML");
  html += escapeHtml(displayName);
  html += F(R"HTML(</h2>
<p>Relay is currently: <strong id="relay">Loading...</strong></p>
<form action="/toggle" method="post" id="toggleForm">
<button id="toggle" disabled>Loading...</button>
</form>
<p id="timerText" aria-live="polite">Loading timer...</p>
<form action="/timer" method="post" id="timerForm">
<label for="seconds">Timer duration (seconds)</label><br><br>
<input id="seconds" name="seconds" type="number" min="1" max="4294967" step="1" value="60" required>
<button type="submit">Start</button>
</form>
<p>Start turns the relay OFF, then ON when the timer finishes.<br>
Starting again replaces the timer. Manual toggling cancels it.</p>
<p><a class="config-button" href="/config">Config</a></p>
<p id="error" role="alert"></p>
<script>
async function refreshStatus() {
  const response = await fetch('/status', {cache: 'no-store'});
  if (!response.ok) throw new Error('Could not read relay status.');
  const state = await response.json();
  document.getElementById('relay').textContent = state.on ? 'ON' : 'OFF';
  const toggle = document.getElementById('toggle');
  toggle.disabled = false;
  toggle.textContent = state.on ? 'Turn OFF' : 'Turn ON';
  toggle.className = state.on ? 'btn-on' : 'btn-off';
  document.getElementById('timerText').textContent = state.active
    ? 'Turning ON in: ' + state.remaining + ' seconds' : 'No timer running.';
}
async function pollStatus() {
  try {
    await refreshStatus();
    document.getElementById('error').textContent = '';
  } catch (error) {
    document.getElementById('error').textContent = 'Connection lost. Retrying...';
  }
  setTimeout(pollStatus, 1000);
}
for (const id of ['toggleForm', 'timerForm']) {
  const form = document.getElementById(id);
  form.addEventListener('submit', async event => {
    event.preventDefault();
    const button = form.querySelector('button');
    button.disabled = true;
    try {
      const response = await fetch(form.action, {
        method: 'POST', body: new URLSearchParams(new FormData(form))
      });
      if (!response.ok) throw new Error(await response.text());
      await refreshStatus();
      document.getElementById('error').textContent = '';
    } catch (error) {
      document.getElementById('error').textContent = error.message;
    } finally {
      button.disabled = false;
    }
  });
}
pollStatus();
</script></body></html>)HTML");
  return html;
}

String getConfigHTML(const String& message, bool isError,
                     const String& formDisplayName, const String& formMdnsName) {
  String html;
  html.reserve(3000);
  html = F("<!DOCTYPE html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"><title>Config - ");
  html += escapeHtml(displayName);
  html += F(R"HTML(</title><style>
body { font-family: Arial, sans-serif; margin: 40px auto; padding: 0 16px; max-width: 480px; background: #f4f4f4; }
label { display: block; margin-top: 24px; } input { box-sizing: border-box; width: 100%; padding: 12px; font-size: 18px; margin-top: 8px; }
button, .back { display: inline-block; padding: 12px 24px; font-size: 18px; color: white; background: #1976d2; border: none; border-radius: 8px; cursor: pointer; text-decoration: none; }
.back { background: #555; } .help { color: #555; } .error { color: #b71c1c; } .success { color: #256b2b; }
</style></head><body><h2>Device configuration</h2><p>)HTML");
  html += escapeHtml(displayName);
  html += F("</p>");
  if (message.length()) {
    html += isError ? F("<p class=\"error\" role=\"alert\">") : F("<p class=\"success\" role=\"status\">");
    html += escapeHtml(message);
    html += F("</p>");
  }
  html += F("<form action=\"/config\" method=\"post\"><label for=\"displayName\">WebUI display name</label><input id=\"displayName\" name=\"displayName\" type=\"text\" maxlength=\"64\" required value=\"");
  html += escapeHtml(formDisplayName);
  html += F("\"><p class=\"help\">Up to 64 bytes; spaces are allowed.</p><label for=\"mdnsName\">mDNS hostname</label><input id=\"mdnsName\" name=\"mdnsName\" type=\"text\" maxlength=\"63\" pattern=\"[A-Za-z0-9]([A-Za-z0-9\\-]{0,61}[A-Za-z0-9])?\" autocapitalize=\"none\" spellcheck=\"false\" required value=\"");
  html += escapeHtml(formMdnsName);
  html += F("\"><p class=\"help\">Use letters, digits and hyphens. Enter only the hostname, without .local; for example, garage-relay. Choose a unique name on your network.</p><button type=\"submit\">Save</button></form><p>Settings are retained after reboot or power loss.</p><p>Device address: <a href=\"http://");
  html += mdnsName;
  html += F(".local/\">http://");
  html += mdnsName;
  html += F(".local/</a></p><p class=\"help\">After changing the hostname, open the new address above. It may take a moment to become available.</p><p><a class=\"back\" href=\"http://");
  // The IP link remains usable even when the page was opened through the old hostname.
  html += WiFi.localIP().toString();
  html += F("/\">Back to relay</a></p></body></html>");
  return html;
}

void handleConfig() {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html; charset=utf-8", getConfigHTML("", false, displayName, mdnsName));
}

void handleSaveConfig() {
  String newDisplayName = server.arg("displayName");
  String newMdnsName = server.arg("mdnsName");
  newDisplayName.trim();
  newMdnsName.trim();
  newMdnsName.toLowerCase();
  server.sendHeader("Cache-Control", "no-store");
  if (!validDisplayName(newDisplayName) || !validMdnsName(newMdnsName)) {
    server.send(400, "text/html; charset=utf-8", getConfigHTML(
      "Enter a display name of 1-64 bytes and a hostname of 1-63 letters, digits or hyphens. The hostname cannot start or end with a hyphen or contain .local.",
      true, newDisplayName, newMdnsName));
    return;
  }
  const bool hostnameChanged = newMdnsName != mdnsName;
  if (!saveSettings(newDisplayName, newMdnsName)) {
    server.send(500, "text/html; charset=utf-8", getConfigHTML(
      "Could not save settings to flash. Please try again.", true, newDisplayName, newMdnsName));
    return;
  }
  if (hostnameChanged || !mdnsStarted) restartMdns();
  const String message = mdnsStarted
    ? "Settings saved. The display name and mDNS address are updated; no reboot is needed."
    : "Settings saved, but mDNS is unavailable right now. Use Back to relay to connect by IP address.";
  server.send(200, "text/html; charset=utf-8", getConfigHTML(message, false, displayName, mdnsName));
}

void redirectHome() {
  server.sendHeader("Location", "/", true);
  server.send(303, "text/plain", "");
}

void handleToggle() {
  timerActive = false;
  setRelay(!relayState);
  redirectHome();
}

void handleStartTimer() {
  const String input = server.arg("seconds");
  unsigned long seconds = 0;
  bool valid = input.length() > 0;
  for (unsigned int i = 0; valid && i < input.length(); ++i) {
    const char digit = input[i];
    if (digit < '0' || digit > '9') {
      valid = false;
      break;
    }
    const unsigned long value = digit - '0';
    if (seconds > (MAX_TIMER_SECONDS - value) / 10UL) {
      valid = false;
      break;
    }
    seconds = seconds * 10UL + value;
  }
  if (!valid || seconds == 0) {
    server.send(400, "text/plain", "Enter a whole number of seconds from 1 to 4294967.");
    return;
  }
  setRelay(false);
  timerStartedAt = millis();
  timerDuration = seconds * 1000UL;
  timerActive = true;
  Serial.printf("Relay timer started: %lu seconds\n", seconds);
  redirectHome();
}

void handleStatus() {
  updateTimer();
  String json = F("{\"on\":");
  json += relayState ? "true" : "false";
  json += F(",\"active\":");
  json += timerActive ? "true" : "false";
  json += F(",\"remaining\":");
  json += String(secondsRemaining());
  json += '}';
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

void updateWifiAndLed() {
  const bool connected = WiFi.status() == WL_CONNECTED;
  const unsigned long now = millis();
  if (connected != wifiWasConnected) {
    wifiWasConnected = connected;
    ledOn = true;
    lastLedChange = now;
    if (connected) {
      Serial.print("Wi-Fi connected. Open http://");
      Serial.println(WiFi.localIP());
      restartMdns();
    } else {
      if (mdnsStarted) MDNS.close();
      mdnsStarted = false;
      Serial.println("Wi-Fi disconnected. Reconnecting...");
    }
  }
  if (connected) {
    ledOn = true;
  } else if (now - lastLedChange >= LED_BLINK_INTERVAL) {
    ledOn = !ledOn;
    lastLedChange = now;
  }
  digitalWrite(STATUS_LED_PIN, ledOn ? LOW : HIGH);
  if (mdnsStarted) MDNS.update();
}

void setup() {
  // Set output levels before enabling the pins to avoid a relay activation glitch.
  digitalWrite(RELAY_PIN, HIGH);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, LOW);
  pinMode(STATUS_LED_PIN, OUTPUT);
  lastLedChange = millis();

  Serial.begin(115200);
  Serial.println("\nStarting up...");
  loadSettings();

  server.on("/", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/html; charset=utf-8", getHTML());
  });
  server.on("/config", HTTP_GET, handleConfig);
  server.on("/config", HTTP_POST, handleSaveConfig);
  server.on("/toggle", HTTP_POST, handleToggle);
  server.on("/timer", HTTP_POST, handleStartTimer);
  server.on("/status", HTTP_GET, handleStatus);
  server.begin();

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to Wi-Fi: ");
  Serial.println(ssid);
  // Connection is handled in loop(), so the timer and LED keep working offline.
}

void loop() {
  updateTimer();
  updateWifiAndLed();
  server.handleClient();
}
