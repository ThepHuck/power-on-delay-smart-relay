#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <EEPROM.h>
#include <bearssl/bearssl_hmac.h>

// --- UPDATE THESE WITH YOUR WI-FI DETAILS ---
const char* ssid = "SSID";
const char* password = "PASSWORD";

// Web login credentials (separate from the Wi-Fi password).
const char WEB_USERNAME[] = "admin";
// Set this before compiling. A password saved through Config overrides it.
const char DEFAULT_WEB_PASSWORD[] = "ChangeMe!";
static_assert(sizeof(DEFAULT_WEB_PASSWORD) >= 9 && sizeof(DEFAULT_WEB_PASSWORD) <= 129,
              "Web password must be 8-128 bytes");
const size_t MAX_WEB_PASSWORD_LENGTH = 128;
const unsigned long SESSION_LIFETIME = 3600000UL; // One hour; reboots end all sessions.
const unsigned long LOGIN_RETRY_DELAY = 1000UL;
const uint32_t PASSWORD_HASH_ITERATIONS = 10000;

// This working relay board is active LOW on GPIO0.
const int RELAY_PIN = 0;
// ESP-01S onboard LED: GPIO2, active LOW (do not use the relay pin).
const int STATUS_LED_PIN = 2;
static_assert(RELAY_PIN != STATUS_LED_PIN, "Relay and LED need separate pins");
const unsigned long DEFAULT_STARTUP_DELAY_SECONDS = 60UL;
const unsigned long LED_BLINK_INTERVAL = 1000UL;
const unsigned long MAX_TIMER_SECONDS = 4294967UL; // Fits in 32-bit milliseconds.

bool relayState = false;
bool timerActive = true;
unsigned long timerStartedAt = 0; // Startup countdown begins at boot.
unsigned long startupDelaySeconds = DEFAULT_STARTUP_DELAY_SECONDS;
unsigned long timerDuration = DEFAULT_STARTUP_DELAY_SECONDS * 1000UL;
bool ledOn = true;
unsigned long lastLedChange = 0;
bool wifiWasConnected = false;
bool mdnsStarted = false;

ESP8266WebServer server(80);

const size_t MAX_DISPLAY_NAME_LENGTH = 64;
const size_t MAX_MDNS_NAME_LENGTH = 63;
const uint32_t LEGACY_SETTINGS_MAGIC = 0x524C5901;
const uint32_t PASSWORD_SETTINGS_MAGIC = 0x524C5902;
const uint32_t SETTINGS_MAGIC = 0x524C5903; // Version 3 adds the startup delay.
struct LegacyDeviceSettings {
  uint32_t magic;
  uint32_t checksum;
  char displayName[MAX_DISPLAY_NAME_LENGTH + 1];
  char mdnsName[MAX_MDNS_NAME_LENGTH + 1];
};
struct PasswordCredentials {
  uint8_t salt[16];
  uint8_t hash[32];
};
struct PasswordDeviceSettings {
  uint32_t magic;
  uint32_t checksum;
  char displayName[MAX_DISPLAY_NAME_LENGTH + 1];
  char mdnsName[MAX_MDNS_NAME_LENGTH + 1];
  uint8_t hasPasswordOverride;
  PasswordCredentials credentials;
};
struct DeviceSettings {
  uint32_t magic;
  uint32_t checksum;
  char displayName[MAX_DISPLAY_NAME_LENGTH + 1];
  char mdnsName[MAX_MDNS_NAME_LENGTH + 1];
  uint8_t hasPasswordOverride;
  PasswordCredentials credentials;
  uint32_t startupDelaySeconds;
};
String displayName = "ESP-01S Relay Controller";
String mdnsName = "esp-relay";
bool settingsPersisted = false;
bool hasPasswordOverride = false;
PasswordCredentials passwordCredentials = {};

struct WebSession {
  char token[33];
  char csrf[33];
  unsigned long startedAt;
};
WebSession sessions[4] = {};
WebSession* requestSession = nullptr;
bool loginAttemptMade = false;
unsigned long lastLoginAttempt = 0;

void updateTimer();

bool constantTimeEqual(const uint8_t* first, const uint8_t* second, size_t length) {
  uint8_t difference = 0;
  for (size_t i = 0; i < length; ++i) difference |= first[i] ^ second[i];
  return difference == 0;
}

bool secretEquals(const String& first, const String& second) {
  if (first.length() != second.length()) return false;
  return constantTimeEqual(reinterpret_cast<const uint8_t*>(first.c_str()),
                           reinterpret_cast<const uint8_t*>(second.c_str()), first.length());
}

void derivePasswordHash(const String& candidate, const uint8_t* salt, uint8_t* output) {
  // PBKDF2-HMAC-SHA256, one 32-byte output block. BearSSL provides HMAC.
  br_hmac_key_context key;
  br_hmac_key_init(&key, &br_sha256_vtable, candidate.c_str(), candidate.length());
  br_hmac_context context;
  const uint8_t blockIndex[] = {0, 0, 0, 1};
  uint8_t previous[32];
  br_hmac_init(&context, &key, 32);
  br_hmac_update(&context, salt, 16);
  br_hmac_update(&context, blockIndex, sizeof(blockIndex));
  br_hmac_out(&context, previous);
  memcpy(output, previous, sizeof(previous));
  for (uint32_t iteration = 1; iteration < PASSWORD_HASH_ITERATIONS; ++iteration) {
    br_hmac_init(&context, &key, 32);
    br_hmac_update(&context, previous, sizeof(previous));
    br_hmac_out(&context, previous);
    for (size_t i = 0; i < sizeof(previous); ++i) output[i] ^= previous[i];
    if (iteration % 32 == 0) {
      updateTimer(); // Keep relay timing and the watchdog working during hashing.
      yield();
    }
  }
}

bool verifyWebPassword(const String& candidate) {
  if (candidate.length() == 0 || candidate.length() > MAX_WEB_PASSWORD_LENGTH) return false;
  if (!hasPasswordOverride) return secretEquals(candidate, String(DEFAULT_WEB_PASSWORD));
  uint8_t hash[32];
  derivePasswordHash(candidate, passwordCredentials.salt, hash);
  return constantTimeEqual(hash, passwordCredentials.hash, sizeof(hash));
}

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

template <typename T>
uint32_t namesChecksum(const T& settings) {
  // FNV-1a detects incomplete/corrupt stored names; do not hash struct padding.
  uint32_t hash = 2166136261UL;
  for (char c : settings.displayName) hash = (hash ^ static_cast<uint8_t>(c)) * 16777619UL;
  for (char c : settings.mdnsName) hash = (hash ^ static_cast<uint8_t>(c)) * 16777619UL;
  return hash;
}

template <typename T>
uint32_t passwordSettingsChecksum(const T& settings) {
  uint32_t hash = namesChecksum(settings);
  hash = (hash ^ settings.hasPasswordOverride) * 16777619UL;
  for (uint8_t c : settings.credentials.salt) hash = (hash ^ c) * 16777619UL;
  for (uint8_t c : settings.credentials.hash) hash = (hash ^ c) * 16777619UL;
  return hash;
}

uint32_t settingsChecksum(const DeviceSettings& settings) {
  uint32_t hash = passwordSettingsChecksum(settings);
  for (unsigned int shift = 0; shift < 32; shift += 8) {
    hash = (hash ^ ((settings.startupDelaySeconds >> shift) & 0xff)) * 16777619UL;
  }
  return hash;
}

template <typename T>
bool validStoredNames(const T& saved) {
  return memchr(saved.displayName, '\0', sizeof(saved.displayName)) &&
    memchr(saved.mdnsName, '\0', sizeof(saved.mdnsName)) &&
    validDisplayName(String(saved.displayName)) && validMdnsName(String(saved.mdnsName));
}

void loadSettings() {
  EEPROM.begin(sizeof(DeviceSettings));
  DeviceSettings saved = {};
  EEPROM.get(0, saved);
  if (saved.magic == SETTINGS_MAGIC && saved.checksum == settingsChecksum(saved) &&
      saved.hasPasswordOverride <= 1 && validStoredNames(saved) &&
      saved.startupDelaySeconds >= 1 && saved.startupDelaySeconds <= MAX_TIMER_SECONDS) {
    displayName = saved.displayName;
    mdnsName = saved.mdnsName;
    hasPasswordOverride = saved.hasPasswordOverride != 0;
    passwordCredentials = saved.credentials;
    startupDelaySeconds = saved.startupDelaySeconds;
    settingsPersisted = true;
  } else {
    PasswordDeviceSettings previous = {};
    EEPROM.get(0, previous);
    if (previous.magic == PASSWORD_SETTINGS_MAGIC &&
        previous.checksum == passwordSettingsChecksum(previous) &&
        previous.hasPasswordOverride <= 1 && validStoredNames(previous)) {
      displayName = previous.displayName;
      mdnsName = previous.mdnsName;
      hasPasswordOverride = previous.hasPasswordOverride != 0;
      passwordCredentials = previous.credentials;
      Serial.println("Loaded previous configuration. Startup delay defaults to 60 seconds.");
      return;
    }
    // Keep names saved by the previous firmware; use the compiled login password.
    LegacyDeviceSettings legacy = {};
    EEPROM.get(0, legacy);
    if (legacy.magic == LEGACY_SETTINGS_MAGIC && legacy.checksum == namesChecksum(legacy) &&
        validStoredNames(legacy)) {
      displayName = legacy.displayName;
      mdnsName = legacy.mdnsName;
      Serial.println("Loaded previous configuration. Password uses compiled default.");
    } else {
      Serial.println("No valid saved configuration. Using defaults.");
    }
  }
}

bool saveSettings(const String& newDisplayName, const String& newMdnsName,
                  unsigned long newStartupDelaySeconds,
                  const PasswordCredentials* newCredentials = nullptr) {
  // Write only on a changed Save, never during polling or at each boot.
  if (!newCredentials && settingsPersisted && newDisplayName == displayName &&
      newMdnsName == mdnsName && newStartupDelaySeconds == startupDelaySeconds) return true;
  DeviceSettings saved = {};
  saved.magic = SETTINGS_MAGIC;
  newDisplayName.toCharArray(saved.displayName, sizeof(saved.displayName));
  newMdnsName.toCharArray(saved.mdnsName, sizeof(saved.mdnsName));
  saved.hasPasswordOverride = newCredentials || hasPasswordOverride;
  saved.credentials = newCredentials ? *newCredentials : passwordCredentials;
  saved.startupDelaySeconds = newStartupDelaySeconds;
  saved.checksum = settingsChecksum(saved);
  EEPROM.put(0, saved);
  if (!EEPROM.commit()) return false;
  displayName = newDisplayName;
  mdnsName = newMdnsName;
  hasPasswordOverride = saved.hasPasswordOverride != 0;
  passwordCredentials = saved.credentials;
  startupDelaySeconds = saved.startupDelaySeconds;
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

void sendPrivateHeaders() {
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("X-Frame-Options", "DENY");
  server.sendHeader("X-Content-Type-Options", "nosniff");
}

void redirectTo(const String& location) {
  sendPrivateHeaders();
  server.sendHeader("Location", location, true);
  server.send(303, "text/plain", "");
}

String getLoginHTML(const String& message = "") {
  String html = F(R"HTML(<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1"><title>Relay login</title>
<style>
body { font-family: Arial, sans-serif; margin: 50px auto; padding: 0 16px; max-width: 360px; background: #f4f4f4; }
label { display: block; margin-top: 20px; } input { box-sizing: border-box; width: 100%; padding: 12px; margin-top: 8px; font-size: 18px; }
button { margin-top: 24px; padding: 12px 24px; font-size: 18px; color: white; background: #1976d2; border: none; border-radius: 8px; cursor: pointer; }
</style></head><body><h2>Relay login</h2>)HTML");
  if (message.length()) {
    html += F("<p role=\"alert\">");
    html += escapeHtml(message);
    html += F("</p>");
  }
  html += F(R"HTML(<form action="/login" method="post">
<label for="username">Username</label><input id="username" name="username" type="text" value="admin" autocomplete="username" required>
<label for="password">Password</label><input id="password" name="password" type="password" maxlength="128" autocomplete="current-password" required>
<button type="submit">Sign in</button></form></body></html>)HTML");
  return html;
}

String sessionCookie() {
  // Parse a complete cookie name/value, rather than accepting substring matches.
  const String cookie = server.header("Cookie");
  unsigned int start = 0;
  while (start < cookie.length()) {
    const int delimiter = cookie.indexOf(';', start);
    const unsigned int end = delimiter < 0 ? cookie.length() : static_cast<unsigned int>(delimiter);
    String part = cookie.substring(start, end);
    part.trim();
    if (part.startsWith("ESPSESSIONID=")) return part.substring(13);
    start = end + 1;
  }
  return "";
}

WebSession* authenticatedSession() {
  const String token = sessionCookie();
  if (token.length() != 32) return nullptr;
  const unsigned long now = millis();
  for (WebSession& session : sessions) {
    if (session.token[0] && now - session.startedAt >= SESSION_LIFETIME) session.token[0] = '\0';
    if (session.token[0] && secretEquals(token, String(session.token))) return &session;
  }
  return nullptr;
}

void randomToken(char* output) {
  uint8_t randomBytes[16];
  ESP.random(randomBytes, sizeof(randomBytes));
  const char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < sizeof(randomBytes); ++i) {
    output[2 * i] = hex[randomBytes[i] >> 4];
    output[2 * i + 1] = hex[randomBytes[i] & 15];
  }
  output[32] = '\0';
}

void clearSessionCookie() {
  server.sendHeader("Set-Cookie", "ESPSESSIONID=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
}

void handleLogin() {
  sendPrivateHeaders();
  if (server.method() == HTTP_GET) {
    if (authenticatedSession()) {
      redirectTo("/");
      return;
    }
    server.send(200, "text/html; charset=utf-8", getLoginHTML());
    return;
  }
  const unsigned long now = millis();
  if (loginAttemptMade && now - lastLoginAttempt < LOGIN_RETRY_DELAY) {
    server.sendHeader("Retry-After", "1");
    server.send(429, "text/html; charset=utf-8", getLoginHTML("Please wait a second before trying again."));
    return;
  }
  loginAttemptMade = true;
  lastLoginAttempt = now;
  if (server.arg("username") != WEB_USERNAME || !verifyWebPassword(server.arg("password"))) {
    server.send(401, "text/html; charset=utf-8", getLoginHTML("Incorrect username or password."));
    return;
  }
  WebSession* selected = &sessions[0];
  for (WebSession& session : sessions) {
    if (!session.token[0] || now - session.startedAt >= SESSION_LIFETIME) {
      selected = &session;
      break;
    }
    if (now - session.startedAt > now - selected->startedAt) selected = &session;
  }
  randomToken(selected->token);
  randomToken(selected->csrf);
  selected->startedAt = millis();
  String cookie = F("ESPSESSIONID=");
  cookie += selected->token;
  cookie += F("; Path=/; HttpOnly; SameSite=Strict; Max-Age=3600");
  server.sendHeader("Set-Cookie", cookie);
  redirectTo("/");
}

String csrfInput() {
  String html = F("<input type=\"hidden\" name=\"csrf\" value=\"");
  html += requestSession->csrf;
  html += F("\">");
  return html;
}

void addProtectedRoute(const char* uri, HTTPMethod method, std::function<void()> handler) {
  server.on(uri, method, [method, handler]() {
    sendPrivateHeaders();
    WebSession* session = authenticatedSession();
    if (!session) {
      clearSessionCookie();
      if (method == HTTP_GET && server.uri() != "/status") redirectTo("/login");
      else server.send(401, "text/plain", "Authentication required.");
      return;
    }
    if (method == HTTP_POST && !secretEquals(server.arg("csrf"), String(session->csrf))) {
      server.send(403, "text/plain", "Session validation failed. Reload the page and try again.");
      return;
    }
    requestSession = session;
    handler();
    requestSession = nullptr;
  });
}

void handleLogout() {
  requestSession->token[0] = '\0';
  clearSessionCookie();
  redirectTo("/login");
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
<form action="/logout" method="post" id="logoutForm"><button type="submit">Sign out</button></form>
<p id="error" role="alert"></p>
<script>
async function refreshStatus() {
  const response = await fetch('/status', {cache: 'no-store'});
  if (response.status === 401) { location.replace('/login'); throw new Error('Please sign in again.'); }
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
      if (response.status === 401) { location.replace('/login'); throw new Error('Please sign in again.'); }
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
  html.replace("<button id=\"toggle\"", csrfInput() + "<button id=\"toggle\"");
  html.replace("<label for=\"seconds\">", csrfInput() + "<label for=\"seconds\">");
  html.replace("<form action=\"/logout\" method=\"post\" id=\"logoutForm\">",
               String(F("<form action=\"/logout\" method=\"post\" id=\"logoutForm\">")) + csrfInput());
  return html;
}

String getConfigHTML(const String& message, bool isError,
                     const String& formDisplayName, const String& formMdnsName,
                     const String& formStartupDelay) {
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
  html += F("<form action=\"/config\" method=\"post\">");
  html += csrfInput();
  html += F("<label for=\"displayName\">WebUI display name</label><input id=\"displayName\" name=\"displayName\" type=\"text\" maxlength=\"64\" required value=\"");
  html += escapeHtml(formDisplayName);
  html += F("\"><p class=\"help\">Up to 64 bytes; spaces are allowed.</p><label for=\"mdnsName\">mDNS hostname</label><input id=\"mdnsName\" name=\"mdnsName\" type=\"text\" maxlength=\"63\" pattern=\"[A-Za-z0-9]([A-Za-z0-9\\-]{0,61}[A-Za-z0-9])?\" autocapitalize=\"none\" spellcheck=\"false\" required value=\"");
  html += escapeHtml(formMdnsName);
  html += F("\"><p class=\"help\">Use letters, digits and hyphens. Enter only the hostname, without .local; for example, garage-relay. Choose a unique name on your network.</p>");
  html += F("<label for=\"startupDelaySeconds\">Startup delay (seconds)</label><input id=\"startupDelaySeconds\" name=\"startupDelaySeconds\" type=\"number\" min=\"1\" max=\"4294967\" step=\"1\" required value=\"");
  html += escapeHtml(formStartupDelay);
  html += F("\"><p class=\"help\">Time before the relay turns ON after boot. Default: 60 seconds. Changes apply on the next reboot.</p><button type=\"submit\">Save</button></form><p>Settings are retained after reboot or power loss.</p>");
  html += F("<h3>Change admin password</h3><form action=\"/password\" method=\"post\">");
  html += csrfInput();
  html += F(R"HTML(<label for="currentPassword">Current password</label>
<input id="currentPassword" name="currentPassword" type="password" maxlength="128" autocomplete="current-password" required>
<label for="newPassword">New password</label>
<input id="newPassword" name="newPassword" type="password" minlength="8" maxlength="128" autocomplete="new-password" required>
<label for="confirmPassword">Confirm new password</label>
<input id="confirmPassword" name="confirmPassword" type="password" minlength="8" maxlength="128" autocomplete="new-password" required>
<p class="help">Use 8-128 bytes. Changing the password signs out all sessions.</p>
<button type="submit">Update password</button></form><p>Device address: <a href="http://)HTML");
  html += mdnsName;
  html += F(".local/\">http://");
  html += mdnsName;
  html += F(".local/</a></p><p class=\"help\">After changing the hostname, open the new address above. It may take a moment to become available. Opening a different address may require signing in again.</p><p><a class=\"back\" href=\"http://");
  // The IP link remains usable even when the page was opened through the old hostname.
  html += WiFi.localIP().toString();
  html += F("/\">Back to relay</a></p></body></html>");
  return html;
}

void handleConfig() {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html; charset=utf-8", getConfigHTML("", false, displayName, mdnsName, String(startupDelaySeconds)));
}

bool parseTimerSeconds(const String& input, unsigned long& seconds) {
  seconds = 0;
  if (input.length() == 0) return false;
  for (unsigned int i = 0; i < input.length(); ++i) {
    const char digit = input[i];
    if (digit < '0' || digit > '9') return false;
    const unsigned long value = digit - '0';
    if (seconds > (MAX_TIMER_SECONDS - value) / 10UL) return false;
    seconds = seconds * 10UL + value;
  }
  return seconds > 0;
}

void handleSaveConfig() {
  String newDisplayName = server.arg("displayName");
  String newMdnsName = server.arg("mdnsName");
  const String newStartupDelay = server.arg("startupDelaySeconds");
  unsigned long newStartupDelaySeconds = 0;
  newDisplayName.trim();
  newMdnsName.trim();
  newMdnsName.toLowerCase();
  server.sendHeader("Cache-Control", "no-store");
  if (!validDisplayName(newDisplayName) || !validMdnsName(newMdnsName)) {
    server.send(400, "text/html; charset=utf-8", getConfigHTML(
      "Enter a display name of 1-64 bytes and a hostname of 1-63 letters, digits or hyphens. The hostname cannot start or end with a hyphen or contain .local.",
      true, newDisplayName, newMdnsName, newStartupDelay));
    return;
  }
  if (!parseTimerSeconds(newStartupDelay, newStartupDelaySeconds)) {
    server.send(400, "text/html; charset=utf-8", getConfigHTML(
      "Enter a startup delay in whole seconds from 1 to 4294967.",
      true, newDisplayName, newMdnsName, newStartupDelay));
    return;
  }
  const bool hostnameChanged = newMdnsName != mdnsName;
  if (!saveSettings(newDisplayName, newMdnsName, newStartupDelaySeconds)) {
    server.send(500, "text/html; charset=utf-8", getConfigHTML(
      "Could not save settings to flash. Please try again.", true, newDisplayName, newMdnsName, newStartupDelay));
    return;
  }
  if (hostnameChanged || !mdnsStarted) restartMdns();
  const String message = mdnsStarted
    ? "Settings saved. The display name and mDNS address are updated. The startup delay applies on the next reboot."
    : "Settings saved. The startup delay applies on the next reboot. mDNS is unavailable right now; use Back to relay to connect by IP address.";
  server.send(200, "text/html; charset=utf-8", getConfigHTML(message, false, displayName, mdnsName, String(startupDelaySeconds)));
}

void handleChangePassword() {
  const String currentPassword = server.arg("currentPassword");
  const String newPassword = server.arg("newPassword");
  const String confirmation = server.arg("confirmPassword");
  if (newPassword.length() < 8 || newPassword.length() > MAX_WEB_PASSWORD_LENGTH ||
      !secretEquals(newPassword, confirmation)) {
    server.send(400, "text/html; charset=utf-8", getConfigHTML(
      "The new password must contain 8-128 bytes and match its confirmation.", true, displayName, mdnsName, String(startupDelaySeconds)));
    return;
  }
  const unsigned long now = millis();
  if (loginAttemptMade && now - lastLoginAttempt < LOGIN_RETRY_DELAY) {
    server.sendHeader("Retry-After", "1");
    server.send(429, "text/html; charset=utf-8", getConfigHTML(
      "Please wait a second before trying again.", true, displayName, mdnsName, String(startupDelaySeconds)));
    return;
  }
  loginAttemptMade = true;
  lastLoginAttempt = now;
  if (!verifyWebPassword(currentPassword)) {
    server.send(403, "text/html; charset=utf-8", getConfigHTML(
      "The current password is incorrect.", true, displayName, mdnsName, String(startupDelaySeconds)));
    return;
  }
  PasswordCredentials updated = {};
  ESP.random(updated.salt, sizeof(updated.salt));
  derivePasswordHash(newPassword, updated.salt, updated.hash);
  if (!saveSettings(displayName, mdnsName, startupDelaySeconds, &updated)) {
    server.send(500, "text/html; charset=utf-8", getConfigHTML(
      "Could not save the password to flash. Please try again.", true, displayName, mdnsName, String(startupDelaySeconds)));
    return;
  }
  for (WebSession& session : sessions) session.token[0] = '\0';
  clearSessionCookie();
  server.send(200, "text/html; charset=utf-8", getLoginHTML("Password updated. Sign in with your new password."));
}

void redirectHome() {
  redirectTo("/");
}

void handleToggle() {
  timerActive = false;
  setRelay(!relayState);
  redirectHome();
}

void handleStartTimer() {
  unsigned long seconds = 0;
  if (!parseTimerSeconds(server.arg("seconds"), seconds)) {
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
  timerDuration = startupDelaySeconds * 1000UL;

  server.collectHeaders("Cookie");
  server.on("/login", HTTP_GET, handleLogin);
  server.on("/login", HTTP_POST, handleLogin);
  addProtectedRoute("/", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/html; charset=utf-8", getHTML());
  });
  addProtectedRoute("/config", HTTP_GET, handleConfig);
  addProtectedRoute("/config", HTTP_POST, handleSaveConfig);
  addProtectedRoute("/password", HTTP_POST, handleChangePassword);
  addProtectedRoute("/logout", HTTP_POST, handleLogout);
  addProtectedRoute("/toggle", HTTP_POST, handleToggle);
  addProtectedRoute("/timer", HTTP_POST, handleStartTimer);
  addProtectedRoute("/status", HTTP_GET, handleStatus);
  server.onNotFound([]() { server.send(404, "text/plain", "Not found."); });
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
