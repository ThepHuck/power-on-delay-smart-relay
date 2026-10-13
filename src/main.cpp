#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>

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
  return F(R"HTML(<!DOCTYPE html>
<html><head><meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESP-01S Relay Controller</title>
<style>
body { font-family: Arial, sans-serif; text-align: center; margin: 50px 16px; background: #f4f4f4; }
button { padding: 16px 28px; font-size: 20px; cursor: pointer; color: white; border-radius: 8px; border: none; background: #1976d2; }
.btn-on { background: #f44336; } .btn-off { background: #4caf50; }
input { padding: 12px; font-size: 20px; width: 160px; }
form { margin: 24px 0; } #error { color: #b71c1c; }
</style></head><body>
<h2>ESP-01S Relay Controller</h2>
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
      mdnsStarted = MDNS.begin("esp-relay");
      if (mdnsStarted) {
        MDNS.addService("http", "tcp", 80);
        Serial.println("Also available at http://esp-relay.local");
      }
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

  server.on("/", HTTP_GET, []() { server.send(200, "text/html", getHTML()); });
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
