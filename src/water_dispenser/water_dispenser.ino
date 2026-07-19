/*
  Water Dispenser Controller
  ESP32-based automated 5-gallon water dispensing system

  Features:
  - Flow sensor calibrated volume dispensing
  - Solenoid valve control via relay
  - LCD status display
  - WiFi provisioning via captive portal (WiFiManager; hold fill button 5s)
  - POS webhook integration with retry queue and dedup
  - Pause/Resume mid-fill
  - Daily + lifetime fill counters
  - Local OTA updates (browser-based, same WiFi)
  - Cloud OTA updates (GitHub-hosted, checked every 6 hrs + on boot)

  Current version: 1.0.6
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ElegantOTA.h>
#include <esp32FOTA.hpp>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <time.h>
#include "esp_mac.h"

// ==========================================================
// FIRMWARE VERSION - bump this on every release
// ==========================================================
const String FIRMWARE_VERSION = "1.0.6";

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

// ---- SOFT FINISH (pulse valve near end to cut average flow / splash) ----
const float softFinishStartPct = 0.90;   // start soft fill at 90%
const unsigned long softOpenMs  = 200;
const unsigned long softCloseMs = 400;
// If average flow is already slow (multi-faucet / low pressure), skip pulsing
const float softFinishMinPulseRate = 20.0; // pulses/sec; below this = keep valve open

// ---- SAFETY LIMITS ----
// Longer limits tolerate low pressure when other faucets are open
const unsigned long maxFillTimeMs   = 300000; // 5 minutes
const unsigned long noFlowTimeoutMs = 20000;  // 20s (slow drip still counts as flow)
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

// ---- SOFT FINISH STATE ----
bool softFinishActive = false;
unsigned long softPhaseStartMs = 0;
bool softValveOpen = false;

// ---- FIRMWARE UPDATE STATE ----
bool updateAvailable = false;
String availableVersion = "";
String firmwareDownloadUrl = "";
unsigned long updatePromptLastToggleMs = 0;
bool updatePromptVisible = false;

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
// 4th arg = allow_insecure_https: required for GitHub HTTPS (bundled CA often not linked)
esp32FOTA fota("water-dispenser", FIRMWARE_VERSION.c_str(), false, true);

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

  prefs.begin("waterstation", false);
  totalFillCount = prefs.getLong("fillCount", 0);
  todayFillCount = prefs.getLong("todayCount", 0);
  todayDateStr   = prefs.getString("todayDate", "");
  lastEventId    = prefs.getULong("lastEventId", 0);
  pulsesPerLiter = prefs.getFloat("pulsesPerL", 374.0);
  long savedTarget = prefs.getLong("targetPulses", 0);
  String savedUrl = prefs.getString("webhookUrl", "");
  savedUrl.toCharArray(webhookUrl, 200);
  loadPendingQueue();

  // Prefer jug-calibrated target if present; else derive from 1L factor
  if (savedTarget > 100) {
    targetPulses = savedTarget;
    pulsesPerLiter = (float)targetPulses / targetLiters;
  } else {
    targetPulses = (long)(targetLiters * pulsesPerLiter);
  }

  Serial.print("Loaded total: ");
  Serial.println(totalFillCount);
  Serial.print("Target pulses: ");
  Serial.println(targetPulses);
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
    delay(3000); // let DNS/TLS stack settle after WiFi connect
    checkForFirmwareUpdate(3); // check on every boot (retry a few times)
  } else {
    clickRelay(2);
  }

  if (!updateAvailable) {
    updateLCDReady();
  }
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
bool fetchManifestUpdate() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("OTA: WiFi not connected");
    return false;
  }

  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();

  String url = String(manifestUrl) + "?cb=" + String(millis());
  Serial.println("OTA: fetching " + url);

  if (!http.begin(client, url)) {
    Serial.println("OTA: http.begin() failed");
    return false;
  }

  http.setTimeout(10000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  int httpCode = http.GET();
  Serial.println("OTA: HTTP status " + String(httpCode));

  if (httpCode != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();
  Serial.println("OTA: manifest " + payload);

  DynamicJsonDocument doc(2048);
  if (deserializeJson(doc, payload)) {
    Serial.println("OTA: JSON parse failed");
    return false;
  }

  JsonVariant entry;
  if (doc.is<JsonArray>()) {
    bool found = false;
    for (JsonVariant item : doc.as<JsonArray>()) {
      const char* type = item["type"] | "";
      if (strcmp(type, "water-dispenser") == 0) {
        entry = item;
        found = true;
        break;
      }
    }
    if (!found) {
      Serial.println("OTA: type water-dispenser not found in manifest");
      return false;
    }
  } else if (doc.is<JsonObject>()) {
    entry = doc.as<JsonVariant>();
    if (strcmp(entry["type"] | "", "water-dispenser") != 0) {
      Serial.println("OTA: manifest type mismatch");
      return false;
    }
  } else {
    Serial.println("OTA: unexpected manifest format");
    return false;
  }

  String remoteVer;
  if (entry["version"].is<const char*>()) {
    remoteVer = entry["version"].as<const char*>();
  } else if (entry["version"].is<int>()) {
    remoteVer = String(entry["version"].as<int>());
  } else {
    Serial.println("OTA: manifest missing version");
    return false;
  }

  const char* binUrl = entry["url"] | "";
  if (strlen(binUrl) == 0) {
    Serial.println("OTA: manifest missing url");
    return false;
  }

  SemverClass localVer(FIRMWARE_VERSION.c_str());
  SemverClass remoteSem(remoteVer.c_str());
  int cmp = semver_compare(*remoteSem.ver(), *localVer.ver());
  Serial.println("OTA: remote=" + remoteVer + " local=" + FIRMWARE_VERSION + " compare=" + String(cmp));

  if (cmp != 1) {
    return false;
  }

  availableVersion = remoteVer;
  firmwareDownloadUrl = binUrl;
  return true;
}

void checkForFirmwareUpdate(int maxAttempts) {
  Serial.println("Device firmware version: " + FIRMWARE_VERSION);

  bool available = false;
  for (int attempt = 1; attempt <= maxAttempts; attempt++) {
    available = fetchManifestUpdate();
    Serial.println("manifest check attempt " + String(attempt) + " returned: " + String(available));
    if (available) break;
    if (attempt < maxAttempts) delay(2000);
  }

  updateAvailable = available;
  lastUpdateCheckMs = millis();
  if (available) {
    Serial.println("Update available: " + availableVersion + " from " + firmwareDownloadUrl);
    showUpdatePrompt();
    clickRelay(2);
  } else {
    Serial.println("No update available.");
  }
}

void performFirmwareUpdate() {
  if (firmwareDownloadUrl.length() == 0) {
    Serial.println("OTA: no firmware URL, aborting update.");
    return;
  }

  lcd.setCursor(0, 0);
  lcd.print("Updating...     ");
  lcd.setCursor(0, 1);
  lcd.print("Do not unplug!  ");
  Serial.println("Starting cloud OTA update...");
  Serial.println("Downloading " + firmwareDownloadUrl);

  // On success, execOTA() flashes and reboots (never returns).
  // If it returns, the update failed - report and recover.
  fota.forceUpdate(firmwareDownloadUrl.c_str(), false);

  Serial.println("OTA: update failed.");
  showError("Update failed");
  delay(3000);
  updatePromptLastToggleMs = 0; // resume update prompt on next loop tick
  updatePromptVisible = false;
}

// =====================
// LCD
// =====================
void showUpdatePrompt() {
  lcd.setCursor(0, 0);
  lcd.print("Update ready!   ");
  lcd.setCursor(0, 1);
  lcd.print("Hold btn 3s     ");
  updatePromptVisible = true;
  updatePromptLastToggleMs = millis();
}

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
  if (softFinishActive) {
    lcd.print("Slow finish...  ");
  } else {
    lcd.print("Filling...      ");
  }
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
void clearSoftFinish() {
  softFinishActive = false;
  softPhaseStartMs = 0;
  softValveOpen = false;
}

void startSoftFinish(unsigned long now) {
  if (softFinishActive) return;
  softFinishActive = true;
  softValveOpen = true;
  softPhaseStartMs = now;
  digitalWrite(relayPin, HIGH);
  Serial.println("Soft finish started (pulsed valve).");
}

void updateSoftFinishValve(unsigned long now) {
  if (!softFinishActive) return;

  unsigned long phaseMs = softValveOpen ? softOpenMs : softCloseMs;
  if (now - softPhaseStartMs < phaseMs) return;

  softValveOpen = !softValveOpen;
  softPhaseStartMs = now;
  digitalWrite(relayPin, softValveOpen ? HIGH : LOW);
}

void startFill() {
  pulseCount     = 0;
  state          = FILLING;
  fillStartMs    = millis();
  lastFlowPulses = 0;
  lastFlowMs     = millis();
  lastLcdMs      = 0;
  clearSoftFinish();
  digitalWrite(relayPin, HIGH);
  Serial.println("Fill started.");
}

void pauseFill() {
  state = PAUSED;
  pauseStartMs = millis();
  clearSoftFinish();
  digitalWrite(relayPin, LOW);
  Serial.println("Fill paused at " + String(pulseCount) + " pulses.");
  updateLCDPaused();
}

void resumeFill() {
  state = FILLING;
  lastFlowPulses = pulseCount;
  lastFlowMs = millis();
  clearSoftFinish();
  // Re-enter soft finish immediately if already past the soft-start threshold
  long softStartPulses = (long)(targetPulses * softFinishStartPct);
  if (pulseCount >= softStartPulses) {
    startSoftFinish(millis());
  } else {
    digitalWrite(relayPin, HIGH);
  }
  Serial.println("Fill resumed.");
}

void abortFill(const char* reason) {
  state = IDLE;
  clearSoftFinish();
  digitalWrite(relayPin, LOW);
  Serial.print("Fill aborted: ");
  Serial.println(reason);
  showError(reason);
  delay(3000);
  updateLCDReady();
}

void stopFill() {
  state = IDLE;
  clearSoftFinish();
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
void saveFillTarget(long measuredPulses) {
  targetPulses = measuredPulses;
  pulsesPerLiter = (float)targetPulses / targetLiters;
  prefs.putLong("targetPulses", targetPulses);
  prefs.putFloat("pulsesPerL", pulsesPerLiter);
  Serial.print("Saved targetPulses: ");
  Serial.println(targetPulses);
  Serial.print("Derived pulsesPerLiter: ");
  Serial.println(pulsesPerLiter);
}

void finishCalibrationUi(bool ok, const char* line2) {
  lcd.setCursor(0, 0);
  lcd.print(ok ? "Cal saved!      " : "Cal failed!     ");
  lcd.setCursor(0, 1);
  String msg = String(line2) + "                ";
  lcd.print(msg.substring(0, 16));
  delay(3000);
  updateLCDReady();
  while (digitalRead(buttonPin) == LOW) delay(10);
  buttonWasReleased = true;
}

// 1L pour: good for sensor factor; jug fill uses targetLiters * factor
void runOneLiterCalibration() {
  pulseCount = 0;
  digitalWrite(relayPin, LOW);
  lcd.setCursor(0, 0);
  lcd.print("Cal: Pour 1L    ");
  lcd.setCursor(0, 1);
  lcd.print("Press when done ");
  Serial.println("Calibration: pour 1L then press button.");

  while (digitalRead(buttonPin) == LOW) delay(10);
  delay(300);

  while (true) {
    server.handleClient();
    ElegantOTA.loop();
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
    prefs.putLong("targetPulses", targetPulses);
    char buf[17];
    snprintf(buf, sizeof(buf), "%.0f p/L", pulsesPerLiter);
    finishCalibrationUi(true, buf);
  } else {
    finishCalibrationUi(false, "Too few pulses");
  }
}

// Real jug: valve opens, user presses when jug is full (match typical faucet load)
void runJugCalibration() {
  pulseCount = 0;
  clearSoftFinish();
  digitalWrite(relayPin, HIGH);
  lcd.setCursor(0, 0);
  lcd.print("Cal: Fill jug   ");
  lcd.setCursor(0, 1);
  lcd.print("Press when FULL ");
  Serial.println("Jug calibration: filling. Press when jug is full.");
  Serial.println("Tip: use your typical faucet load (e.g. other taps as usual).");

  while (digitalRead(buttonPin) == LOW) delay(10);
  delay(300);

  unsigned long started = millis();
  while (true) {
    server.handleClient();
    ElegantOTA.loop();

    lcd.setCursor(0, 1);
    lcd.print("Pulses:");
    lcd.print(pulseCount);
    lcd.print("      ");

    if (digitalRead(buttonPin) == LOW) {
      delay(50);
      if (digitalRead(buttonPin) == LOW) break;
    }

    // Safety: abort if somehow left running too long
    if (millis() - started > maxFillTimeMs) {
      digitalWrite(relayPin, LOW);
      finishCalibrationUi(false, "Timeout");
      return;
    }
  }

  digitalWrite(relayPin, LOW);
  long measuredPulses = pulseCount;
  if (measuredPulses > 200) {
    saveFillTarget(measuredPulses);
    char buf[17];
    snprintf(buf, sizeof(buf), "%ld pulses", measuredPulses);
    finishCalibrationUi(true, buf);
  } else {
    finishCalibrationUi(false, "Too few pulses");
  }
}

// Clears WiFi only (keeps fill counts / calibration). Opens captive portal on reboot.
void startWifiConfig() {
  digitalWrite(relayPin, LOW);
  lcd.setCursor(0, 0);
  lcd.print("WiFi setup...   ");
  lcd.setCursor(0, 1);
  lcd.print("Connect to AP   ");
  Serial.println("Button WiFi config: clearing WiFi credentials (prefs kept).");
  WiFiManager wm;
  wm.resetSettings();
  delay(1500);
  ESP.restart();
}

// Hold 5s -> this menu: short press = jug cal, hold 2s = 1L cal
void runCalibrationMenu() {
  lcd.setCursor(0, 0);
  lcd.print("Cal: JUG fill   ");
  lcd.setCursor(0, 1);
  lcd.print("Tap=jug Hold=1L ");
  Serial.println("Calibration menu: short press = jug, hold 2s = 1L.");

  while (digitalRead(buttonPin) == LOW) delay(10);
  delay(200);

  unsigned long menuStart = millis();
  while (true) {
    server.handleClient();
    ElegantOTA.loop();

    if (millis() - menuStart > 15000) {
      runJugCalibration();
      return;
    }

    if (digitalRead(buttonPin) == LOW) {
      delay(50);
      if (digitalRead(buttonPin) != LOW) continue;

      unsigned long pressAt = millis();
      while (digitalRead(buttonPin) == LOW) {
        server.handleClient();
        ElegantOTA.loop();
        if (millis() - pressAt >= 2000) {
          while (digitalRead(buttonPin) == LOW) delay(10);
          runOneLiterCalibration();
          return;
        }
      }
      runJugCalibration();
      return;
    }
  }
}

// Hold 5s on fill button (IDLE): WiFi setup or calibration — no need to open the enclosure
void runHoldMenu() {
  lcd.setCursor(0, 0);
  lcd.print("WiFi setup?     ");
  lcd.setCursor(0, 1);
  lcd.print("Tap=WiFi Hold=Cal");
  Serial.println("Hold menu: short press = WiFi setup, hold 2s = calibration.");

  while (digitalRead(buttonPin) == LOW) delay(10);
  delay(200);

  unsigned long menuStart = millis();
  while (true) {
    server.handleClient();
    ElegantOTA.loop();

    // Default to WiFi setup if no choice (matches "button = WiFi config" expectation)
    if (millis() - menuStart > 15000) {
      startWifiConfig();
      return;
    }

    if (digitalRead(buttonPin) == LOW) {
      delay(50);
      if (digitalRead(buttonPin) != LOW) continue;

      unsigned long pressAt = millis();
      while (digitalRead(buttonPin) == LOW) {
        server.handleClient();
        ElegantOTA.loop();
        if (millis() - pressAt >= 2000) {
          while (digitalRead(buttonPin) == LOW) delay(10);
          runCalibrationMenu();
          return;
        }
      }
      startWifiConfig();
      return;
    }
  }
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
      long softStartPulses = (long)(targetPulses * softFinishStartPct);
      if (pulseCount >= softStartPulses) {
        // Soft-finish is for high pressure splash. If flow is already slow
        // (other faucets open / low pressure), keep the valve fully open.
        unsigned long elapsed = now - fillStartMs;
        float pulseRate = (elapsed > 0) ? ((float)pulseCount * 1000.0f / (float)elapsed) : 0.0f;
        if (pulseRate >= softFinishMinPulseRate) {
          startSoftFinish(now);
          updateSoftFinishValve(now);
          if (softFinishActive && !softValveOpen) {
            lastFlowMs = now;
          }
        } else if (!softFinishActive) {
          digitalWrite(relayPin, HIGH);
        }
      }

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
    // 5-second hold: WiFi setup (external button) or calibration
    else if (!updateAvailable && state == IDLE && (now - buttonPressStart >= 5000)) {
      longPressHandled = true;
      runHoldMenu();
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

    // Retry OTA check ~15s after boot if the first attempt during setup failed
    static bool bootRecheckDone = false;
    if (!bootRecheckDone && now > 15000 && WiFi.status() == WL_CONNECTED) {
      bootRecheckDone = true;
      if (!updateAvailable) {
        Serial.println("Boot delayed OTA recheck...");
        checkForFirmwareUpdate(3);
      }
    }

    // Periodic cloud firmware check every 6 hours (single attempt to avoid UI freeze)
    if (now - lastUpdateCheckMs >= updateCheckIntervalMs && WiFi.status() == WL_CONNECTED) {
      checkForFirmwareUpdate(1);
    }

    // Alternate LCD between ready screen and update prompt if available
    if (updateAvailable) {
      if (now - updatePromptLastToggleMs > 3000) {
        updatePromptLastToggleMs = now;
        if (updatePromptVisible) {
          updateLCDReady();
          updatePromptVisible = false;
        } else {
          showUpdatePrompt();
        }
      }
    }
  }
}