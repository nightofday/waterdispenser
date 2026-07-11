#include <WiFi.h>
#include <WiFiClientSecure.h>
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
// NOTE: 374.0 was calibrated by hand-pouring. Pressurized line flow will
// differ slightly. Once plumbed: run one measured fill, compare the
// "CALIBRATION" line in Serial output against actual volume dispensed,
// and update this constant.
const float pulsesPerLiter = 374.0; // calibrated from real-world testing
const float targetGallons  = 5.0;
const float targetLiters   = targetGallons * 3.78541;
long targetPulses;

// ---- SAFETY LIMITS ----
// NOTE: 3 min is a placeholder until the system is plumbed under pressure.
// After the first real fill, read the duration from the "CALIBRATION" line
// in Serial output and set this to roughly 2x that value, so a slow day
// (low line pressure) doesn't trip the timeout on legitimate fills.
const unsigned long maxFillTimeMs   = 180000; // hard cap: abort if fill takes > 3 min
const unsigned long noFlowTimeoutMs = 10000;  // abort if no pulses for 10 s mid-fill

// ---- NON-BLOCKING TIMERS ----
const unsigned long lcdIntervalMs       = 250;   // LCD refresh rate while filling
const unsigned long retryIntervalMs     = 15000; // gap between webhook retry attempts
const unsigned long wifiRetryIntervalMs = 15000; // gap between WiFi reconnect attempts

// ---- RUNTIME VARIABLES ----
volatile long pulseCount = 0;
volatile unsigned long lastPulseTime = 0;
bool filling = false;
bool buttonWasReleased = true; // require release between presses
unsigned long fillStartMs     = 0;
long          lastFlowPulses  = 0;
unsigned long lastFlowMs      = 0;
unsigned long lastLcdMs       = 0;
unsigned long lastRetryMs     = 0;
unsigned long lastWifiTryMs   = 0;

// ---- PERSISTENT STORAGE ----
Preferences prefs;
long fillCount = 0;
unsigned long lastEventId = 0;
char webhookUrl[200] = "";

// ---- PENDING WEBHOOK QUEUE ----
// Each record: "eventId|fillCountAtEvent|timestamp"
// The event ID is generated ONCE per fill and reused on every retry,
// so the POS can deduplicate if a response was lost but the POST landed.
const int MAX_PENDING = 10;
String pendingQueue[MAX_PENDING];
int pendingCount = 0;

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
// PENDING QUEUE PERSISTENCE
// =====================
void savePendingQueue() {
  String joined = "";
  for (int i = 0; i < pendingCount; i++) {
    if (i > 0) joined += '\n';
    joined += pendingQueue[i];
  }
  prefs.putString("pendingQ", joined);
}

void loadPendingQueue() {
  pendingCount = 0;
  String joined = prefs.getString("pendingQ", "");
  int start = 0;
  while (start < (int)joined.length() && pendingCount < MAX_PENDING) {
    int nl = joined.indexOf('\n', start);
    if (nl < 0) nl = joined.length();
    String rec = joined.substring(start, nl);
    if (rec.length() > 0) pendingQueue[pendingCount++] = rec;
    start = nl + 1;
  }
}

void enqueuePending(const String& eventId, long countAtEvent, const String& timestamp) {
  // If full, drop the oldest record to make room
  if (pendingCount >= MAX_PENDING) {
    for (int i = 1; i < MAX_PENDING; i++) pendingQueue[i - 1] = pendingQueue[i];
    pendingCount = MAX_PENDING - 1;
    Serial.println("Pending queue full - dropped oldest event.");
  }
  pendingQueue[pendingCount++] = eventId + "|" + String(countAtEvent) + "|" + timestamp;
  savePendingQueue();
}

void dequeuePending() {
  for (int i = 1; i < pendingCount; i++) pendingQueue[i - 1] = pendingQueue[i];
  pendingCount--;
  savePendingQueue();
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
  fillCount   = prefs.getLong("fillCount", 0);
  lastEventId = prefs.getULong("lastEventId", 0);
  String savedUrl = prefs.getString("webhookUrl", "");
  savedUrl.toCharArray(webhookUrl, 200);
  prefs.remove("pendingRetries"); // migrate away from old counter-based retry key
  loadPendingQueue();

  Serial.print("Loaded fill count: ");
  Serial.println(fillCount);
  Serial.print("Pending webhooks: ");
  Serial.println(pendingCount);
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
    prefs.remove("pendingQ");
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

void updateLCDAborted(const char* reason) {
  lcd.setCursor(0, 0);
  lcd.print("Fill stopped!   ");
  lcd.setCursor(0, 1);
  char line[17];
  snprintf(line, sizeof(line), "%-16s", reason);
  lcd.print(line);
  delay(3000);
  updateLCDReady();
}

// =====================
// FILL CYCLE
// =====================
void startFill() {
  pulseCount     = 0;
  filling        = true;
  fillStartMs    = millis();
  lastFlowPulses = 0;
  lastFlowMs     = millis();
  lastLcdMs      = 0;
  digitalWrite(relayPin, HIGH); // open solenoid valve
  Serial.println("Fill started...");
}

// Abort without counting a jug (cancel, no-flow, or timeout)
void abortFill(const char* reason) {
  filling = false;
  digitalWrite(relayPin, LOW); // close solenoid valve immediately
  Serial.print("Fill aborted: ");
  Serial.println(reason);
  updateLCDAborted(reason);
}

void stopFill() {
  filling = false;
  digitalWrite(relayPin, LOW); // close solenoid valve

  fillCount++;
  prefs.putLong("fillCount", fillCount);

  Serial.print("Fill complete. Total dispensed: ");
  Serial.println(fillCount);

  // Data for tuning pulsesPerLiter and maxFillTimeMs after plumbing:
  // compare pulses against actual volume dispensed, and set maxFillTimeMs
  // to roughly 2x the measured duration.
  unsigned long fillSecs = (millis() - fillStartMs) / 1000;
  Serial.print("CALIBRATION: pulses=");
  Serial.print(pulseCount);
  Serial.print(" duration=");
  Serial.print(fillSecs);
  Serial.println("s");

  // Generate the event ID ONCE per fill; retries reuse it so the POS
  // can deduplicate if our POST landed but the response was lost.
  lastEventId++;
  prefs.putULong("lastEventId", lastEventId);
  String eventId   = machineId + "-" + String(lastEventId);
  String timestamp = getTimestamp();

  // Send webhook to POS; queue the exact event snapshot on failure
  if (!sendWebhook(eventId, fillCount, timestamp)) {
    enqueuePending(eventId, fillCount, timestamp);
    Serial.println("Webhook failed - queued for retry.");
  }

  updateLCDDone();
}

// =====================
// WEBHOOK
// =====================
bool sendWebhook(const String& eventId, long countAtEvent, const String& timestamp) {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (strlen(webhookUrl) == 0) {
    Serial.println("No webhook URL configured.");
    return false;
  }

  HTTPClient http;
  WiFiClientSecure secureClient;
  WiFiClient plainClient;

  // HTTPS URLs need a secure client on ESP32
  bool isHttps = (strncmp(webhookUrl, "https://", 8) == 0);
  if (isHttps) {
    secureClient.setInsecure(); // skip cert validation (no CA store on device)
    if (!http.begin(secureClient, webhookUrl)) return false;
  } else {
    if (!http.begin(plainClient, webhookUrl)) return false;
  }

  http.addHeader("Content-Type", "application/json");
  http.setTimeout(10000); // 10 second timeout

  String payload = "{";
  payload += "\"event_id\":\""       + eventId              + "\",";
  payload += "\"machine_id\":\""     + machineId            + "\",";
  payload += "\"quantity\":1,";
  payload += "\"unit\":\"5gal\",";
  payload += "\"total_dispensed\":"  + String(countAtEvent) + ",";
  payload += "\"timestamp\":\""      + timestamp            + "\"";
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

// Retry the oldest queued event with its original ID, count, and timestamp
bool retryPendingWebhook() {
  String rec = pendingQueue[0];
  int p1 = rec.indexOf('|');
  int p2 = rec.indexOf('|', p1 + 1);
  if (p1 < 0 || p2 < 0) {
    Serial.println("Corrupt pending record - discarding: " + rec);
    dequeuePending();
    return false;
  }
  String eventId   = rec.substring(0, p1);
  long   count     = rec.substring(p1 + 1, p2).toInt();
  String timestamp = rec.substring(p2 + 1);
  return sendWebhook(eventId, count, timestamp);
}

// =====================
// MAIN LOOP
// =====================
void loop() {
  unsigned long now = millis();

  // ---- FILL SUPERVISION (highest priority - controls the valve) ----
  if (filling) {
    // Target reached → normal completion
    if (pulseCount >= targetPulses) {
      stopFill();
    }
    // Hard time cap → sensor may have failed; never leave valve open
    else if (now - fillStartMs > maxFillTimeMs) {
      abortFill("Timeout");
    }
    // No pulses for too long → jug removed / supply dry / sensor fault
    else if (pulseCount == lastFlowPulses && (now - lastFlowMs) > noFlowTimeoutMs) {
      abortFill("No flow");
    }
    else {
      if (pulseCount != lastFlowPulses) {
        lastFlowPulses = pulseCount;
        lastFlowMs     = now;
      }
      // Throttled LCD progress update
      if (now - lastLcdMs >= lcdIntervalMs) {
        lastLcdMs = now;
        updateLCDFilling();
      }
    }
  }

  // ---- BUTTON (edge-triggered: requires release between presses) ----
  bool pressed = (digitalRead(buttonPin) == LOW);
  if (pressed && buttonWasReleased) {
    delay(50); // debounce
    if (digitalRead(buttonPin) == LOW) {
      buttonWasReleased = false;
      if (!filling) {
        startFill();
      } else {
        abortFill("Canceled"); // press again mid-fill to cancel
      }
    }
  } else if (!pressed) {
    buttonWasReleased = true;
  }

  // ---- NETWORK (only when idle, so it can never delay valve shutoff) ----
  if (!filling) {
    // Reconnect WiFi if dropped (non-blocking, rate-limited)
    if (WiFi.status() != WL_CONNECTED && (now - lastWifiTryMs) >= wifiRetryIntervalMs) {
      lastWifiTryMs = now;
      Serial.println("WiFi dropped - reconnecting...");
      WiFi.reconnect();
    }

    // Retry pending failed webhooks (rate-limited)
    if (pendingCount > 0 && WiFi.status() == WL_CONNECTED && (now - lastRetryMs) >= retryIntervalMs) {
      lastRetryMs = now;
      Serial.print("Retrying pending webhook. Remaining: ");
      Serial.println(pendingCount);
      if (retryPendingWebhook()) {
        dequeuePending();
      }
    }
  }
}
