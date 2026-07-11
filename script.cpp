#include <WiFi.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <time.h>

// ---- PIN ASSIGNMENTS ----
const int flowSensorPin = 27;
const int buttonPin     = 25;
const int relayPin      = 26;
const int resetPin      = 14; // hold to GND on boot to reset all settings

// ---- FLOW CALIBRATION ----
const float pulsesPerLiter = 374.0; // calibrated from real-world testing
const float targetGallons  = 5.0;
const float targetLiters   = targetGallons * 3.78541;
long targetPulses;

// ---- RUNTIME VARIABLES ----
volatile long pulseCount = 0;
volatile unsigned long lastPulseTime = 0;
bool filling = false;

// ---- PERSISTENT STORAGE ----
Preferences prefs;
long fillCount       = 0;
long pendingRetries  = 0;
unsigned long lastEventId = 0;
char webhookUrl[200] = "";

// ---- DEVICE IDENTITY (auto from MAC) ----
String machineId;
String hotspotName;

// ---- LCD ----
LiquidCrystal_I2C lcd(0x27, 16, 2);

// =====================
// INTERRUPT
// =====================
void IRAM_ATTR countPulse() {
  unsigned long now = micros();
  if (now - lastPulseTime > 1000) { // debounce: ignore pulses faster than 1ms
    pulseCount++;
    lastPulseTime = now;
  }
}

// =====================
// SETUP
// =====================
void setup() {
  Serial.begin(115200);

  // LCD early init
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Water Dispenser ");
  lcd.setCursor(0, 1);
  lcd.print("Starting up...  ");

  // Generate unique machine ID from MAC address
  // Must call WiFi.macAddress() after WiFi is initialized
  WiFi.mode(WIFI_STA);
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  String macSuffix = mac.substring(6); // last 6 chars e.g. "D17AE4"
  machineId   = "dispenser-" + macSuffix;
  hotspotName = "WaterDispenser-" + macSuffix;

  Serial.println("Machine ID: " + machineId);
  Serial.println("Hotspot:    " + hotspotName);

  // Pin setup
  pinMode(buttonPin,     INPUT_PULLUP);
  pinMode(resetPin,      INPUT_PULLUP);
  pinMode(relayPin,      OUTPUT);
  digitalWrite(relayPin, LOW); // ensure valve is closed on boot
  pinMode(flowSensorPin, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(flowSensorPin), countPulse, FALLING);

  // Calculate target pulse count for 5 gallons
  targetPulses = (long)(targetLiters * pulsesPerLiter);
  Serial.print("Target pulses for 5 gal: ");
  Serial.println(targetPulses);

  // Load saved values from flash
  prefs.begin("waterstation", false);
  fillCount      = prefs.getLong("fillCount", 0);
  pendingRetries = prefs.getLong("pendingRetries", 0);
  lastEventId    = prefs.getULong("lastEventId", 0);
  String savedUrl = prefs.getString("webhookUrl", "");
  savedUrl.toCharArray(webhookUrl, 200);

  Serial.print("Loaded fill count: ");
  Serial.println(fillCount);
  Serial.print("Pending retries: ");
  Serial.println(pendingRetries);
  Serial.print("Webhook URL: ");
  Serial.println(webhookUrl);

  // Check if reset pin held LOW on boot → clear all saved settings
  if (digitalRead(resetPin) == LOW) {
    Serial.println("Reset triggered - clearing settings...");
    lcd.setCursor(0, 1);
    lcd.print("Resetting...    ");
    WiFiManager wm;
    wm.resetSettings();
    prefs.remove("webhookUrl");
    delay(2000);
    ESP.restart();
  }

  // Connect to WiFi (or open setup portal if not configured)
  setupWiFi();

  // Sync time via NTP
  setupTime();

  // Show ready screen
  updateLCDReady();

  Serial.println("System ready.");
}

// =====================
// WIFI SETUP
// =====================
void setupWiFi() {
  lcd.setCursor(0, 1);
  lcd.print("Connecting WiFi ");

  WiFiManager wm;

  // Add webhook URL as a custom config field in the portal
  WiFiManagerParameter webhookParam(
    "webhook",       // internal key
    "Webhook URL",   // label shown in portal
    webhookUrl,      // current/default value
    200              // max length
  );
  wm.addParameter(&webhookParam);

  // Save webhook URL to flash when user submits the portal form
  wm.setSaveParamsCallback([&]() {
    String url = webhookParam.getValue();
    url.trim();
    url.toCharArray(webhookUrl, 200);
    prefs.putString("webhookUrl", url);
    Serial.println("Webhook URL saved: " + url);
  });

  // What to show on LCD when portal is open
  wm.setAPCallback([](WiFiManager* wm) {
    Serial.println("Setup portal open.");
    lcd.setCursor(0, 0);
    lcd.print("Connect to WiFi:");
    // Truncate hotspot name to fit 16 chars
    String display = hotspotName;
    if (display.length() > 16) display = display.substring(0, 16);
    lcd.setCursor(0, 1);
    lcd.print(display);
  });

  // Set portal timeout (300 seconds = 5 minutes)
  wm.setConfigPortalTimeout(300);

  // Try saved credentials; if fail, open portal
  bool connected = wm.autoConnect(
    hotspotName.c_str(), // hotspot SSID (unique per device)
    "setup1234"          // hotspot password
  );

  if (connected) {
    Serial.println("WiFi connected: " + WiFi.localIP().toString());
    lcd.setCursor(0, 1);
    lcd.print("WiFi OK!        ");
    delay(1500);
  } else {
    Serial.println("WiFi failed - running offline.");
    lcd.setCursor(0, 1);
    lcd.print("Offline mode    ");
    delay(1500);
  }
}

// =====================
// NTP TIME
// =====================
void setupTime() {
  configTime(8 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  // Wait up to 5 seconds for time sync
  struct tm timeinfo;
  int attempts = 0;
  while (!getLocalTime(&timeinfo) && attempts < 10) {
    delay(500);
    attempts++;
  }
}

String getTimestamp() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "1970-01-01T00:00:00+08:00";
  }
  char buf[30];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S+08:00", &timeinfo);
  return String(buf);
}

// =====================
// LCD HELPERS
// =====================
void updateLCDReady() {
  lcd.setCursor(0, 0);
  lcd.print("Press to fill   ");
  lcd.setCursor(0, 1);
  lcd.print("Total:");
  lcd.print(fillCount);
  lcd.print(" jugs   ");
}

void updateLCDFilling() {
  lcd.setCursor(0, 0);
  lcd.print("Filling...      ");
  lcd.setCursor(0, 1);
  int percent = (int)((pulseCount * 100) / targetPulses);
  if (percent > 100) percent = 100;
  lcd.print("Progress:");
  lcd.print(percent);
  lcd.print("%    ");
}

void updateLCDDone() {
  lcd.setCursor(0, 0);
  lcd.print("Done! Jug #");
  lcd.print(fillCount);
  lcd.print("  ");
  lcd.setCursor(0, 1);
  lcd.print("Remove jug...   ");
  delay(2000);
  updateLCDReady();
}

// =====================
// FILL CYCLE
// =====================
void startFill() {
  pulseCount = 0;
  filling    = true;
  digitalWrite(relayPin, HIGH); // open solenoid valve
  Serial.println("Fill started...");
}

void stopFill() {
  filling = false;
  digitalWrite(relayPin, LOW); // close solenoid valve

  fillCount++;
  prefs.putLong("fillCount", fillCount);

  Serial.print("Fill complete. Total dispensed: ");
  Serial.println(fillCount);

  // Send webhook to POS
  if (!sendWebhook(fillCount)) {
    pendingRetries++;
    prefs.putLong("pendingRetries", pendingRetries);
    Serial.println("Webhook failed - queued for retry.");
  }

  updateLCDDone();
}

// =====================
// WEBHOOK
// =====================
bool sendWebhook(long countAtEvent) {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (strlen(webhookUrl) == 0) {
    Serial.println("No webhook URL configured.");
    return false;
  }

  // Unique event ID = machineId + incrementing counter
  // Prevents duplicate inventory count if webhook is retried
  lastEventId++;
  prefs.putULong("lastEventId", lastEventId);
  String eventId = machineId + "-" + String(lastEventId);

  HTTPClient http;
  http.begin(webhookUrl);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(10000); // 10 second timeout

  String payload = "{";
  payload += "\"event_id\":\""       + eventId             + "\",";
  payload += "\"machine_id\":\""     + machineId           + "\",";
  payload += "\"quantity\":1,";
  payload += "\"unit\":\"5gal\",";
  payload += "\"total_dispensed\":"  + String(countAtEvent) + ",";
  payload += "\"timestamp\":\""      + getTimestamp()       + "\"";
  payload += "}";

  Serial.println("Sending webhook: " + payload);

  int httpCode = http.POST(payload);
  http.end();

  bool success = (httpCode == 200 || httpCode == 201 || httpCode == 204);
  if (success) {
    Serial.println("Webhook sent OK. HTTP " + String(httpCode));
  } else {
    Serial.println("Webhook failed. HTTP " + String(httpCode));
  }
  return success;
}

// =====================
// MAIN LOOP
// =====================
void loop() {

  // Reconnect WiFi if dropped
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi dropped - reconnecting...");
    WiFi.reconnect();
    delay(5000);
  }

  // Retry any pending failed webhooks
  if (pendingRetries > 0 && WiFi.status() == WL_CONNECTED) {
    Serial.print("Retrying pending webhook. Remaining: ");
    Serial.println(pendingRetries);
    if (sendWebhook(fillCount)) {
      pendingRetries--;
      prefs.putLong("pendingRetries", pendingRetries);
    }
    delay(5000); // wait 5 sec between retries
  }

  // Button press → start fill (only if not already filling)
  if (digitalRead(buttonPin) == LOW && !filling) {
    delay(50); // debounce
    if (digitalRead(buttonPin) == LOW) {
      startFill();
    }
  }

  // Update LCD progress bar while filling
  if (filling) {
    updateLCDFilling();
  }

  // Auto-stop when target volume reached
  if (filling && pulseCount >= targetPulses) {
    stopFill();
  }
}