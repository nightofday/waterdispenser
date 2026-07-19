/*
  Water Dispenser Controller
  ESP32-based automated 5-gallon water dispensing system

  Features:
  - Flow sensor calibrated volume dispensing
  - Solenoid valve control via relay
  - LCD status display
  - WiFi provisioning via captive portal (WiFiManager)
  - POS webhook integration with retry queue and dedup
  - Pause/Resume mid-fill
  - Daily + lifetime fill counters
  - Local OTA updates (browser-based, same WiFi)
  - Cloud OTA updates (GitHub-hosted, checked every 6 hrs + on boot)

  Current version: 1.0.1
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ElegantOTA.h>
#include <esp32FOTA.hpp>
#include <Preferences.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <time.h>
#include "esp_mac.h"

// ==========================================================
// FIRMWARE VERSION - bump this on every release
// ==========================================================
const String FIRMWARE_VERSION = "1.0.1";

// ==========================================================
// CLOUD OTA CONFIG - update with your actual GitHub repo
// ==========================================================
const char* manifestUrl = "https://raw.githubusercontent.com/nightofday/waterdispenser/main/manifest.json";

// ---- PIN ASSIGNMENTS ----
const int flowSensorPin = 27;
const int buttonPin     = 25;
const int relayPin      = 26;
const int resetPin      = 14;

// ---- FLOW CALIBRATION ----
float pulsesPerLiter = 374.0;
const float targetGallons = 5.0;
const float targetLiters  = targetGallons * 3.78541;
long targetPulses;

// ---- SAFETY LIMITS ----
const unsigned long maxFillTimeMs   = 180000;
const unsigned long noFlowTimeoutMs = 10000;
const unsigned long pauseTimeoutMs  = 120000;

// ---- NON-BLOCKING TIMERS ----
const unsigned long lcdIntervalMs       = 250;
const unsigned long retryIntervalMs     = 15000;
const unsigned long wifiRetryIntervalMs = 15000;
const unsigned long updateCheckIntervalMs = 6UL * 60 * 60 * 1000; // 6 hours

// ---- STATE MACHINE ----
enum FillState { IDLE, FILLING, PAUSED };
FillState state = IDLE;

volatile long pulseCount = 0;
volatile unsigned long lastPulseTime = 0;
bool buttonWasReleased = true;
unsigned long fillStartMs    = 0;
unsigned long pauseStartMs   = 0;
long          lastFlowPulses = 0;
unsigned long lastFlowMs     = 0;
unsigned long lastLcdMs      = 0;
unsigned long lastRetryMs    = 0;
unsigned long lastWifiTryMs  = 0;
unsigned long lastUpdateCheckMs = 0;

unsigned long buttonPressStart = 0;
bool longPressHandled = false;

// ---- FIRMWARE UPDATE STATE ----
bool updateAvailable = false;
String availableVersion = "";

// ---- PERSISTENT STORAGE ----
Preferences prefs;
long totalFillCount = 0;
long todayFillCount  = 0;
String todayDateStr  = "";
unsigned long lastEventId = 0;
char webhookUrl[200] = "";

// ---- PENDING WEBHOOK QUEUE ----
const int MAX_PENDING = 10;
String pendingQueue[MAX_PENDING];
int pendingCount = 0;

// ---- DEVICE IDENTITY ----
String machineId;
String hotspotName;

// ---- WEB SERVER FOR LOCAL OTA ----
WebServer server(80);

// ---- CLOUD OTA ----
esp32FOTA fota("water-dispenser", FIRMWARE_VERSION.c_str());

LiquidCrystal_I2C lcd(0x27, 16, 2);

// =====================
// INTERRUPT
// =====================
void IRAM_ATTR countPulse() {
  unsigned long now = micros();
  if (now - lastPulseTime > 1000) {
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
  if (pendingCount >= MAX_PENDING) {
    for (int i = 1; i < MAX_PENDING; i++) pendingQueue[i - 1] = pendingQueue[i];
    pendingCount = MAX_PENDING - 1;
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
// DAILY COUNTER HELPERS
// =====================
String getDateString() {
  struct tm t;
  if (!getLocalTime(&t)) return "unknown";
  char buf[11];
  strftime(buf, sizeof(buf), "%Y-%m-%d", &t);
  return String(buf);
}

void checkDailyReset() {
  String currentDate = getDateString();
  if (currentDate == "unknown") return;
  if (todayDateStr != currentDate) {
    todayDateStr  = currentDate;
    todayFillCount = 0;
    prefs.putString("todayDate", todayDateStr);
    prefs.putLong("todayCount", todayFillCount);
    Serial.println("Daily counter reset for " + todayDateStr);
  }
}

// =====================
// SETUP
// =====================
void setup() {
  Serial.begin(115200);

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Water Dispenser ");
  lcd.setCursor(0, 1);
  lcd.print("v" + FIRMWARE_VERSION + " Starting.. ");

  uint8_t mac[6];
  esp_efuse_mac_get_default(mac);
  char macStr[7];
  sprintf(macStr, "%02X%02X%02X", mac[3], mac[4], mac[5]);
  String macSuffix = String(macStr);
  machineId   = "dispenser-" + macSuffix;
  hotspotName = "WD-" + macSuffix;
  Serial.println("Machine ID: " + machineId);
  Serial.println("Firmware version: " + FIRMWARE_VERSION);

  pinMode(buttonPin,     INPUT_PULLUP);
  pinMode(resetPin,      INPUT_PULLUP);
  pinMode(relayPin,      OUTPUT);
  digitalWrite(relayPin, LOW);
  pinMode(flowSensorPin, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(flowSensorPin), countPulse, FALLING);

  targetPulses = (long)(targetLiters * pulsesPerLiter);

  prefs.begin("waterstation", false);
  totalFillCount = prefs.getLong("fillCount", 0);
  todayFillCount = prefs.getLong("todayCount", 0);
  todayDateStr   = prefs.getString("todayDate", "");
  lastEventId    = prefs.getULong("lastEventId", 0);
  pulsesPerLiter = prefs.getFloat("pulsesPerL", 374.0);
  String savedUrl = prefs.getString("webhookUrl", "");
  savedUrl.toCharArray(webhookUrl, 200);
  loadPendingQueue();

  targetPulses = (long)(targetLiters * pulsesPerLiter);

  Serial.print("Loaded total: ");
  Serial.println(totalFillCount);
  Serial.print("Pending webhooks: ");
  Serial.println(pendingCount);

  if (digitalRead(resetPin) == LOW) {
    Serial.println("Reset triggered - clearing settings...");
    lcd.setCursor(0, 1);
    lcd.print("Resetting...    ");
    WiFiManager wm;
    wm.resetSettings();
    prefs.clear();
    delay(2000);
    ESP.restart();
  }

  setupWiFi();
  setupTime();
  checkDailyReset();
  setupLocalOTA();

  if (WiFi.status() == WL_CONNECTED) {
    clickRelay(1);
    checkForFirmwareUpdate(); // check on every boot
  } else {
    clickRelay(2);
  }

  updateLCDReady();
  Serial.println("System ready.");
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Local OTA: http://" + WiFi.localIP().toString() + "/update");
  }
}

// =====================
// RELAY CLICK SIGNAL (audible alerts)
// =====================
void clickRelay(int times) {
  for (int i = 0; i < times; i++) {
    digitalWrite(relayPin, HIGH);
    delay(120);
    digitalWrite(relayPin, LOW);
    delay(150);
  }
}

// =====================
// WIFI SETUP
// =====================
void setupWiFi() {
  lcd.setCursor(0, 1);
  lcd.print("Connecting WiFi ");

  WiFiManager wm;
  WiFiManagerParameter webhookParam("webhook", "Webhook URL", webhookUrl, 200);
  wm.addParameter(&webhookParam);

  wm.setSaveParamsCallback([&]() {
    String url = webhookParam.getValue();
    url.trim();
    url.toCharArray(webhookUrl, 200);
    prefs.putString("webhookUrl", url);
  });

  wm.setAPCallback([](WiFiManager* w) {
    lcd.setCursor(0, 0);
    lcd.print("Connect to WiFi:");
    String d = hotspotName;
    if (d.length() > 16) d = d.substring(0, 16);
    lcd.setCursor(0, 1);
    lcd.print(d);
  });

  wm.setConfigPortalTimeout(300);
  bool connected = wm.autoConnect(hotspotName.c_str(), "setup1234");

  lcd.setCursor(0, 1);
  lcd.print(connected ? "WiFi OK!        " : "Offline mode    ");
  delay(1500);
}

void setupTime() {
  configTime(8 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  struct tm t;
  int attempts = 0;
  while (!getLocalTime(&t) && attempts < 10) { delay(500); attempts++; }
}

String getTimestamp() {
  struct tm t;
  if (!getLocalTime(&t)) return "1970-01-01T00:00:00+08:00";
  char buf[30];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S+08:00", &t);
  return String(buf);
}

// =====================
// LOCAL OTA (browser-based, same WiFi network)
// =====================
void setupLocalOTA() {
  server.on("/", []() {
    String html = "Water Dispenser " + machineId + "<br>";
    html += "Firmware: v" + FIRMWARE_VERSION + "<br>";
    html += "Total: " + String(totalFillCount) + " | Today: " + String(todayFillCount) + "<br>";
    html += "<a href='/update'>Go to Local Firmware Update</a>";
    server.send(200, "text/html", html);
  });

  ElegantOTA.begin(&server);
  server.begin();
}

// =====================
// CLOUD OTA (GitHub-hosted, checked periodically)
// =====================
void checkForFirmwareUpdate() {
  fota.setManifestURL(manifestUrl);
  fota.useBundledCerts(true); // required for https:// GitHub URLs on ESP32 core 3.x
  Serial.println("Checking manifest at: " + String(manifestUrl));
  Serial.println("Device firmware version: " + FIRMWARE_VERSION);
  bool available = fota.execHTTPcheck();
  Serial.println("execHTTPcheck() returned: " + String(available));
  updateAvailable = available;
  lastUpdateCheckMs = millis();
  if (available) {
    char remoteVersion[32] = {};
    fota.getPayloadVersion(remoteVersion);
    availableVersion = String(remoteVersion);
    Serial.println("Firmware update available on GitHub! Remote version: " + availableVersion);
  } else {
    Serial.println("Firmware is up to date (or manifest check failed).");
  }
}

void performFirmwareUpdate() {
  lcd.setCursor(0, 0);
  lcd.print("Updating...     ");
  lcd.setCursor(0, 1);
  lcd.print("Do not unplug!  ");
  Serial.println("Starting cloud OTA update...");
  fota.execOTA(); // downloads, flashes, and reboots automatically
}

// =====================
// LCD
// =====================
void updateLCDReady() {
  lcd.setCursor(0, 0);
  lcd.print("Press to fill   ");
  lcd.setCursor(0, 1);
  lcd.print("Tdy:");
  lcd.print(todayFillCount);
  lcd.print(" Tot:");
  lcd.print(totalFillCount);
  lcd.print("   ");
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

void updateLCDPaused() {
  lcd.setCursor(0, 0);
  lcd.print("PAUSED          ");
  lcd.setCursor(0, 1);
  int percent = (int)((pulseCount * 100) / targetPulses);
  if (percent > 100) percent = 100;
  lcd.print("At ");
  lcd.print(percent);
  lcd.print("% - press btn ");
}

void showError(String msg) {
  lcd.setCursor(0, 0);
  lcd.print("ERROR:          ");
  lcd.setCursor(0, 1);
  msg = msg + "                ";
  lcd.print(msg.substring(0, 16));
}

// =====================
// FILL CYCLE
// =====================
void startFill() {
  pulseCount     = 0;
  state          = FILLING;
  fillStartMs    = millis();
  lastFlowPulses = 0;
  lastFlowMs     = millis();
  lastLcdMs      = 0;
  digitalWrite(relayPin, HIGH);
  Serial.println("Fill started.");
}

void pauseFill() {
  state = PAUSED;
  pauseStartMs = millis();
  digitalWrite(relayPin, LOW);
  Serial.println("Fill paused at " + String(pulseCount) + " pulses.");
  updateLCDPaused();
}

void resumeFill() {
  state = FILLING;
  lastFlowPulses = pulseCount;
  lastFlowMs = millis();
  digitalWrite(relayPin, HIGH);
  Serial.println("Fill resumed.");
}

void abortFill(const char* reason) {
  state = IDLE;
  digitalWrite(relayPin, LOW);
  Serial.print("Fill aborted: ");
  Serial.println(reason);
  showError(reason);
  delay(3000);
  updateLCDReady();
}

void stopFill() {
  state = IDLE;
  digitalWrite(relayPin, LOW);

  totalFillCount++;
  todayFillCount++;
  prefs.putLong("fillCount", totalFillCount);
  prefs.putLong("todayCount", todayFillCount);

  Serial.print("Fill complete. Total: ");
  Serial.println(totalFillCount);

  lastEventId++;
  prefs.putULong("lastEventId", lastEventId);
  String eventId   = machineId + "-" + String(lastEventId);
  String timestamp = getTimestamp();

  if (!sendWebhook(eventId, totalFillCount, timestamp)) {
    enqueuePending(eventId, totalFillCount, timestamp);
    Serial.println("Webhook failed - queued for retry.");
  }

  clickRelay(3); // fill complete alert

  lcd.setCursor(0, 0);
  lcd.print("Done! Jug #");
  lcd.print(totalFillCount);
  lcd.print("  ");
  lcd.setCursor(0, 1);
  lcd.print("Remove jug...   ");
  delay(2000);
  updateLCDReady();
}

// =====================
// WEBHOOK
// =====================
bool sendWebhook(const String& eventId, long countAtEvent, const String& timestamp) {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (strlen(webhookUrl) == 0) return false;

  HTTPClient http;
  WiFiClientSecure secureClient;
  WiFiClient plainClient;

  bool isHttps = (strncmp(webhookUrl, "https://", 8) == 0);
  bool began;
  if (isHttps) {
    secureClient.setInsecure();
    began = http.begin(secureClient, webhookUrl);
  } else {
    began = http.begin(plainClient, webhookUrl);
  }
  if (!began) return false;

  http.addHeader("Content-Type", "application/json");
  http.setTimeout(10000);

  String payload = "{";
  payload += "\"event_id\":\""       + eventId              + "\",";
  payload += "\"machine_id\":\""     + machineId            + "\",";
  payload += "\"quantity\":1,";
  payload += "\"unit\":\"5gal\",";
  payload += "\"total_dispensed\":"  + String(countAtEvent) + ",";
  payload += "\"timestamp\":\""      + timestamp            + "\"";
  payload += "}";

  int httpCode = http.POST(payload);
  http.end();

  bool success = (httpCode == 200 || httpCode == 201 || httpCode == 204);
  Serial.println(success ? "Webhook OK" : "Webhook failed: " + String(httpCode));
  return success;
}

bool retryPendingWebhook() {
  String rec = pendingQueue[0];
  int p1 = rec.indexOf('|');
  int p2 = rec.indexOf('|', p1 + 1);
  if (p1 < 0 || p2 < 0) {
    dequeuePending();
    return false;
  }
  String eventId   = rec.substring(0, p1);
  long   count     = rec.substring(p1 + 1, p2).toInt();
  String timestamp = rec.substring(p2 + 1);
  return sendWebhook(eventId, count, timestamp);
}

// =====================
// CALIBRATION MODE
// =====================
void runCalibration() {
  pulseCount = 0;
  lcd.setCursor(0, 0);
  lcd.print("Cal: Pour 1L    ");
  lcd.setCursor(0, 1);
  lcd.print("Press when done ");
  Serial.println("Calibration: pour 1L then press button.");

  while (digitalRead(buttonPin) == LOW) delay(10);
  delay(300);

  while (true) {
    lcd.setCursor(0, 1);
    lcd.print("Pulses:");
    lcd.print(pulseCount);
    lcd.print("      ");

    if (digitalRead(buttonPin) == LOW) {
      delay(50);
      if (digitalRead(buttonPin) == LOW) break;
    }
  }

  long measuredPulses = pulseCount;
  if (measuredPulses > 50) {
    pulsesPerLiter = (float)measuredPulses;
    targetPulses = (long)(targetLiters * pulsesPerLiter);
    prefs.putFloat("pulsesPerL", pulsesPerLiter);

    lcd.setCursor(0, 0);
    lcd.print("Cal saved!      ");
    lcd.setCursor(0, 1);
    lcd.print(pulsesPerLiter);
    lcd.print(" p/L      ");
    Serial.print("New pulsesPerLiter: ");
    Serial.println(pulsesPerLiter);
  } else {
    lcd.setCursor(0, 0);
    lcd.print("Cal failed!     ");
    lcd.setCursor(0, 1);
    lcd.print("Too few pulses  ");
  }

  delay(3000);
  updateLCDReady();
  while (digitalRead(buttonPin) == LOW) delay(10);
  buttonWasReleased = true;
}

// =====================
// MAIN LOOP
// =====================
void loop() {
  unsigned long now = millis();

  // Local OTA + web server (always responsive)
  server.handleClient();
  ElegantOTA.loop();

  // ---- FILL / PAUSE SUPERVISION (highest priority) ----
  if (state == FILLING) {
    if (pulseCount >= targetPulses) {
      stopFill();
    }
    else if (now - fillStartMs > maxFillTimeMs) {
      abortFill("Timeout");
    }
    else if (pulseCount == lastFlowPulses && (now - lastFlowMs) > noFlowTimeoutMs) {
      abortFill("No flow");
    }
    else {
      if (pulseCount != lastFlowPulses) {
        lastFlowPulses = pulseCount;
        lastFlowMs     = now;
      }
      if (now - lastLcdMs >= lcdIntervalMs) {
        lastLcdMs = now;
        updateLCDFilling();
      }
    }
  }
  else if (state == PAUSED) {
    if (now - pauseStartMs > pauseTimeoutMs) {
      abortFill("Pause timeout");
    }
  }

  // ---- BUTTON: short press = start/pause/resume; long press = calibration OR update ----
  bool pressed = (digitalRead(buttonPin) == LOW);

  if (pressed && buttonWasReleased) {
    delay(50);
    if (digitalRead(buttonPin) == LOW) {
      buttonWasReleased = false;
      buttonPressStart = now;
      longPressHandled = false;
    }
  }

  if (pressed && !buttonWasReleased && !longPressHandled) {
    // If a firmware update is available, a 3-second hold triggers it
    if (updateAvailable && state == IDLE && (now - buttonPressStart >= 3000)) {
      longPressHandled = true;
      performFirmwareUpdate();
    }
    // Otherwise, a 5-second hold enters calibration mode
    else if (!updateAvailable && state == IDLE && (now - buttonPressStart >= 5000)) {
      longPressHandled = true;
      lcd.setCursor(0, 0);
      lcd.print("Entering Cal... ");
      delay(400);
      runCalibration();
    }
  }

  if (!pressed && !buttonWasReleased) {
    buttonWasReleased = true;
    if (!longPressHandled) {
      unsigned long heldFor = now - buttonPressStart;
      if (heldFor < 3000) {
        // Short press: cycle through IDLE -> FILLING -> PAUSED -> FILLING ...
        if (state == IDLE) {
          startFill();
        } else if (state == FILLING) {
          pauseFill();
        } else if (state == PAUSED) {
          resumeFill();
        }
      }
    }
  }

  // ---- NETWORK (only when idle, never delays valve control) ----
  if (state == IDLE) {
    checkDailyReset();

    if (WiFi.status() != WL_CONNECTED && (now - lastWifiTryMs) >= wifiRetryIntervalMs) {
      lastWifiTryMs = now;
      WiFi.reconnect();
    }

    if (pendingCount > 0 && WiFi.status() == WL_CONNECTED && (now - lastRetryMs) >= retryIntervalMs) {
      lastRetryMs = now;
      if (retryPendingWebhook()) {
        dequeuePending();
      }
    }

    // Periodic cloud firmware check every 6 hours
    if (now - lastUpdateCheckMs >= updateCheckIntervalMs && WiFi.status() == WL_CONNECTED) {
      checkForFirmwareUpdate();
    }

    // Alternate LCD between ready screen and update prompt if available
    if (updateAvailable) {
      static unsigned long lastPromptToggle = 0;
      static bool showingPrompt = false;
      if (now - lastPromptToggle > 4000) {
        showingPrompt = !showingPrompt;
        lastPromptToggle = now;
        if (showingPrompt) {
          lcd.setCursor(0, 0);
          lcd.print("Update ready!   ");
          lcd.setCursor(0, 1);
          lcd.print("Hold btn 3s     ");
        } else {
          updateLCDReady();
        }
      }
    }
  }
}