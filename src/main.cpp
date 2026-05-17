#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>

// --- UPDATE THESE WITH YOUR WI-FI DETAILS ---
const char* ssid = "SSID";
const char* password = "PASSWORD";

// Most standard ESP-01S Relay modules use GPIO 0 to trigger the relay.
// If yours uses a different pin, change this to 2.
const int RELAY_PIN = 0; 

// Track the current state of the relay (false = off, true = on)
bool relayState = false; 

// Track if the automatic turn-on has happened
bool autoTurnedOn = false;
const unsigned long AUTO_TURN_ON_DELAY = 60000; // 60 seconds in milliseconds

// Create a web server listening on standard HTTP port 80
ESP8266WebServer server(80);

// Function to build the HTML page
String getHTML() {
  String html;
  html.reserve(1500); // Increased memory allocation to accommodate JavaScript
  html = "<!DOCTYPE html><html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">";
  
  // Basic CSS to make it look decent on a phone or desktop
  html += "<style>";
  html += "body { font-family: Arial, sans-serif; text-align: center; margin-top: 50px; background-color: #f4f4f4; }";
  html += ".btn { display: inline-block; padding: 20px 40px; font-size: 24px; cursor: pointer; text-decoration: none; color: white; border-radius: 8px; border: none; transition: 0.3s; }";
  html += ".btn-on { background-color: #f44336; }"; // Red button when ON (to turn it off)
  html += ".btn-off { background-color: #4CAF50; }"; // Green button when OFF (to turn it on)
  html += "</style></head><body>";
  
  html += "<h2>ESP-01S Relay Controller</h2>";

  // Calculate remaining time for the auto-turn-on feature
  long secondsLeft = 0;
  if (!autoTurnedOn && millis() < AUTO_TURN_ON_DELAY) {
    secondsLeft = (AUTO_TURN_ON_DELAY - millis()) / 1000;
  }
  
  // If the timer is still running, display it and add JavaScript to count down
  if (secondsLeft > 0) {
    html += "<p id=\"timerText\">Auto-powering on in: <strong id=\"timer\">" + String(secondsLeft) + "</strong> seconds</p>";
    html += "<script>";
    html += "let timeLeft = " + String(secondsLeft) + ";";
    html += "let countdown = setInterval(function() {";
    html += "  timeLeft--;";
    html += "  if (timeLeft <= 0) {";
    html += "    clearInterval(countdown);";
    html += "    document.getElementById('timerText').innerHTML = '<strong>Powered ON!</strong>';";
    html += "    setTimeout(function() { location.reload(); }, 1500);"; // Reload page to sync the button state
    html += "  } else {";
    html += "    document.getElementById('timer').innerText = timeLeft;";
    html += "  }";
    html += "}, 1000);";
    html += "</script>";
  } else {
    html += "<p>Startup timer: <strong>Completed</strong></p>";
  }

  // Change the button text and color depending on the current state
  if (relayState) {
    html += "<p>Relay is currently: <strong>ON</strong></p>";
    html += "<a href=\"/toggle\" class=\"btn btn-on\">Turn OFF</a>";
  } else {
    html += "<p>Relay is currently: <strong>OFF</strong></p>";
    html += "<a href=\"/toggle\" class=\"btn btn-off\">Turn ON</a>";
  }
  
  html += "</body></html>";
  return html;
}

// What happens when someone goes to the main page (http://<IP_ADDRESS>/)
void handleRoot() {
  server.send(200, "text/html", getHTML());
}

// What happens when someone clicks the button (http://<IP_ADDRESS>/toggle)
void handleToggle() {
  relayState = !relayState; // Flip the state variable
  
  // NOTE: Most ESP-01S relay boards are "Active-LOW". 
  // This means sending a LOW signal turns the relay ON, and HIGH turns it OFF.
  if (relayState) {
    digitalWrite(RELAY_PIN, LOW);  // Turn Relay ON
    Serial.println("Relay toggled: ON");
  } else {
    digitalWrite(RELAY_PIN, HIGH); // Turn Relay OFF
    Serial.println("Relay toggled: OFF");
  }
  
  // After toggling, redirect the browser back to the main page
  server.sendHeader("Location", String("/"), true);
  server.send(302, "text/plain", "");
}

void setup() {
  // Start the serial port for debugging
  Serial.begin(115200);
  delay(100);
  Serial.println("\nStarting up...");
  
  // Set up the relay pin
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH); // Ensure it starts in the OFF state
  
  // Connect to Wi-Fi
  Serial.print("Connecting to Wi-Fi: ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  
  // Wait until connected
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  Serial.println("\nWi-Fi Connected!");
  Serial.print("Go to this IP Address in your browser: ");
  Serial.println(WiFi.localIP()); // Print the IP address to the Serial Monitor
  
  // Tell the web server which functions to run for different URLs
  server.on("/", handleRoot);
  server.on("/toggle", handleToggle);
  
  // Start listening for web traffic
  server.begin();
  Serial.println("Web server running.");
  
  // Start mDNS so you can access the ESP at http://esp-relay.local
  if (MDNS.begin("esp-relay")) {
    Serial.println("MDNS responder started. Go to http://esp-relay.local");
  }
}

void loop() {
  MDNS.update(); // Required to keep mDNS running smoothly
  // Check for incoming web clients constantly
  server.handleClient();

  // Check if 60 seconds have passed since boot and we haven't auto-turned on yet
  if (!autoTurnedOn && millis() >= AUTO_TURN_ON_DELAY) {
    autoTurnedOn = true; // Mark as triggered so it only happens once
    if (!relayState) {   // Only turn on if it's currently off
      relayState = true;
      digitalWrite(RELAY_PIN, LOW); // Turn Relay ON (Active-LOW)
      Serial.println("Relay automatically toggled: ON (60 seconds after boot)");
    }
  }
}