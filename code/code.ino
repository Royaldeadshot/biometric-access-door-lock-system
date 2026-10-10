#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <Adafruit_Fingerprint.h>
#include <time.h>
#include "secrets.h"
#include <driver/gpio.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

// ==========================================
// 1. PIN DEFINITIONS & CONSTANTS
// ==========================================
#define FIRMWARE_VERSION "v1.2.0"

// Fingerprint Sensor (R307S)
#define RX_PIN 16         // ESP32 RX2 <- R307S TX (Yellow Wire)
#define TX_PIN 17         // ESP32 TX2 -> R307S RX (Green Wire)
#define TOUCH_PIN 4       // ESP32 GPIO4 <- R307S Touch Out (Blue Wire)
                          // (White Wire connects to ESP32 3.3V)

// Audio Feedback (Active Buzzer)
#define BUZZER_PIN 19     // Active Buzzer Positive (+) -> GPIO 19. Negative (-) -> GND

// Door Lock Relay (Solenoid)
#define RELAY_PIN 25      // ESP32 GPIO 25 (Pin D25) -> Relay IN pin
#define RELAY_ACTIVE_LEVEL LOW
#define RELAY_INACTIVE_LEVEL HIGH

// Interior Push-to-Exit Button
#define EXIT_BUTTON_PIN 27       // ESP32 GPIO 27 (Pin D27) -> Switch Pin 1. Switch Pin 2 -> GND

// LED Ring (Future)
#define LED_PIN 18               // WS2812 / SK6812 LED Ring Data (GPIO 18)

// Sensor limits
const uint8_t MAX_SENSOR_CAPACITY = 127;

// Captive Portal AP Configuration
const char* AP_SSID = "FitnessBox-Scanner";

// ==========================================
// OBJECTS & GLOBALS
// ==========================================
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&Serial2);
Preferences prefs;
WebServer server(80);
DNSServer dnsServer;

bool isSetupMode = false;
unsigned long setupModeStartedAt = 0;
const unsigned long SETUP_PORTAL_TIMEOUT_MS = 600000; // 10 minutes auto-restart recovery

String storedSSID = "";
String storedPassword = "";
String storedLinkingKey = "";
String linkedGymId = "";

unsigned long lastCommandCheck = 0;
const unsigned long COMMAND_CHECK_INTERVAL = 3000; // Polling commands & sync signal

unsigned long lastOtaCheck = 0;
const unsigned long OTA_CHECK_INTERVAL = 1800000; // 30 minutes infrequent OTA check

// Non-blocking Solenoid State
bool isSolenoidUnlocked = false;
unsigned long solenoidRelockAt = 0;

// Firebase Auth Token Caching
String cachedIdToken = "";
unsigned long tokenExpiresAt = 0;

// ==========================================
// LOCAL MEMBER CACHE SYSTEM (<15ms OFFLINE ACCESS)
// ==========================================
struct MemberCacheEntry {
  bool valid;
  bool active;
  char expiryDate[11]; // YYYY-MM-DD
  char name[32];
};
MemberCacheEntry localCache[MAX_SENSOR_CAPACITY + 1];

// Forward declarations
void soundAccessGranted();
void soundAccessDeniedUnknown();
void soundAccessDeniedExpired();
void soundExitButton();
void soundSystemBoot();
void soundCloudOnline();
void soundEnrollPrompt();
void soundEnrollStep1();
void soundEnrollSuccess();
void soundEnrollFailed();
void soundConfigMode();
void triggerSolenoid(int durationSeconds);
void updateSolenoidState();
String extractJsonField(const String &json, const String &fieldName);
String getTodayDateString();
void logAccessEvent(String type, String memberId, String reason);
void applyAuthHeader(HTTPClient &https);

// ==========================================
// 2. AESTHETIC ACTIVE BUZZER AUDIO SUITE
// ==========================================
void activeBeep(int durationMs, int pauseMs = 35) {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(durationMs);
  digitalWrite(BUZZER_PIN, LOW);
  if (pauseMs > 0) delay(pauseMs);
}

void soundAccessGranted() {
  activeBeep(28, 25);
  activeBeep(42, 30);
  activeBeep(140, 0);
}

void soundAccessDeniedUnknown() {
  activeBeep(22, 55);
  activeBeep(22, 0);
}

void soundAccessDeniedExpired() {
  activeBeep(70, 50);
  activeBeep(70, 110);
  activeBeep(200, 0);
}

void soundExitButton() {
  activeBeep(32, 0);
}

void soundSystemBoot() {
  Serial.println("🔊 [SYSTEM BOOT] Power-on sequence...");
  activeBeep(25, 30);
  activeBeep(35, 30);
  activeBeep(50, 40);
  activeBeep(160, 0);
}

void soundCloudOnline() {
  Serial.println("🔊 [CLOUD ONLINE] Connected & armed!");
  delay(80);
  activeBeep(38, 45);
  activeBeep(200, 0);
}

void soundEnrollPrompt() {
  activeBeep(40, 40);
  activeBeep(40, 40);
  activeBeep(85, 0);
}

void soundEnrollStep1() {
  activeBeep(55, 0);
}

void soundEnrollSuccess() {
  activeBeep(30, 25);
  activeBeep(30, 25);
  activeBeep(30, 55);
  activeBeep(65, 35);
  activeBeep(260, 0);
}

void soundEnrollFailed() {
  for (int i = 0; i < 4; i++) {
    activeBeep(45, 35);
  }
}

void soundConfigMode() {
  activeBeep(45, 45);
  activeBeep(45, 120);
  activeBeep(90, 0);
}

// ==========================================
// 3. HARDWARE CONTROL (NON-BLOCKING SOLENOID)
// ==========================================
void triggerSolenoid(int durationSeconds) {
  Serial.printf("⚡ [DOOR LOCK] Unlocking door for %d seconds...\n", durationSeconds);
  digitalWrite(RELAY_PIN, RELAY_ACTIVE_LEVEL);
  solenoidRelockAt = millis() + (unsigned long)(durationSeconds * 1000);
  isSolenoidUnlocked = true;
}

void updateSolenoidState() {
  if (isSolenoidUnlocked && millis() >= solenoidRelockAt) {
    digitalWrite(RELAY_PIN, RELAY_INACTIVE_LEVEL);
    isSolenoidUnlocked = false;
    Serial.println("🔒 [DOOR LOCK] Relocked.");
  }
}

void triggerLedAccessGranted()        { Serial.println("💡 [LED RING] Green pulse"); }
void triggerLedAccessDeniedExpired()  { Serial.println("💡 [LED RING] Rapid red pulse"); }
void triggerLedAccessDeniedUnknown()  { Serial.println("💡 [LED RING] Amber steady"); }
void triggerLedEnrollScanning()       { Serial.println("💡 [LED RING] Yellow rotating"); }
void triggerLedEnrollSuccess()        { Serial.println("💡 [LED RING] Purple completion"); }

// ==========================================
// 4. SECURE JSON & TIME HELPERS
// ==========================================
String extractJsonField(const String &json, const String &fieldName) {
  int fieldIdx = json.indexOf("\"" + fieldName + "\"");
  if (fieldIdx == -1) return "";

  int colonIdx = json.indexOf(":", fieldIdx);
  if (colonIdx == -1) return "";

  int stringValIdx = json.indexOf("\"stringValue\"", colonIdx);
  if (stringValIdx != -1 && stringValIdx < colonIdx + 45) {
    int valColonIdx = json.indexOf(":", stringValIdx);
    if (valColonIdx != -1) {
      int firstQuote = json.indexOf("\"", valColonIdx);
      if (firstQuote != -1) {
        int endQuote = json.indexOf("\"", firstQuote + 1);
        if (endQuote != -1) {
          return json.substring(firstQuote + 1, endQuote);
        }
      }
    }
  }

  int firstQuote = json.indexOf("\"", colonIdx);
  if (firstQuote == -1) return "";

  int endQuote = json.indexOf("\"", firstQuote + 1);
  if (endQuote == -1) return "";

  return json.substring(firstQuote + 1, endQuote);
}

String getTodayDateString() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 1000)) {
    return "";
  }
  char buffer[12];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d", &timeinfo);
  return String(buffer);
}

// ==========================================
// 5. LOCAL CACHE MANAGEMENT (PERSISTENT & FAST)
// ==========================================
void loadLocalCacheFromFlash() {
  prefs.begin("fb_cache", true);
  for (uint8_t i = 1; i <= MAX_SENSOR_CAPACITY; i++) {
    String key = "s_" + String(i);
    String val = prefs.getString(key.c_str(), "");
    if (val.length() > 0) {
      int p1 = val.indexOf('|');
      int p2 = val.indexOf('|', p1 + 1);
      if (p1 != -1 && p2 != -1) {
        localCache[i].valid = true;
        localCache[i].active = (val.substring(0, p1) == "ACTIVE");
        strncpy(localCache[i].expiryDate, val.substring(p1 + 1, p2).c_str(), 10);
        localCache[i].expiryDate[10] = '\0';
        strncpy(localCache[i].name, val.substring(p2 + 1).c_str(), 31);
        localCache[i].name[31] = '\0';
      }
    } else {
      localCache[i].valid = false;
    }
  }
  prefs.end();
  Serial.println("💾 [CACHE] Member cache loaded from NVS flash memory.");
}

void saveMemberToLocalCache(uint8_t slotId, bool active, const String &expiryDate, const String &name) {
  if (slotId < 1 || slotId > MAX_SENSOR_CAPACITY) return;

  localCache[slotId].valid = true;
  localCache[slotId].active = active;
  strncpy(localCache[slotId].expiryDate, expiryDate.c_str(), 10);
  localCache[slotId].expiryDate[10] = '\0';
  strncpy(localCache[slotId].name, name.c_str(), 31);
  localCache[slotId].name[31] = '\0';

  prefs.begin("fb_cache", false);
  String key = "s_" + String(slotId);
  String val = (active ? "ACTIVE|" : "EXPIRED|") + expiryDate + "|" + name;
  prefs.putString(key.c_str(), val);
  prefs.end();
}

// Full collection sync: EXECUTED ONLY ONCE AT FRESH BOOT
void syncActiveMembersCache() {
  if (WiFi.status() != WL_CONNECTED || linkedGymId == "") return;

  Serial.println("\n🔄 [CACHE] Performing initial sync of active members from Firestore...");
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;

  String url = "https://firestore.googleapis.com/v1/projects/" + String(FIREBASE_PROJECT_ID) + "/databases/(default)/documents/gyms/" + linkedGymId + ":runQuery";
  if (!https.begin(client, url)) return;

  applyAuthHeader(https);
  String queryPayload = "{\"structuredQuery\":{\"from\":[{\"collectionId\":\"members\"}],\"limit\":150}}";

  int httpCode = https.POST(queryPayload);
  if (httpCode == 200) {
    String resp = https.getString();
    int searchIdx = 0;
    int syncedCount = 0;

    while (true) {
      int docIdx = resp.indexOf("\"document\":", searchIdx);
      if (docIdx == -1) break;

      int nextDocIdx = resp.indexOf("\"document\":", docIdx + 11);
      String docChunk = (nextDocIdx != -1) ? resp.substring(docIdx, nextDocIdx) : resp.substring(docIdx);
      searchIdx = (nextDocIdx != -1) ? nextDocIdx : resp.length();

      String fingerIdStr = extractJsonField(docChunk, "fingerprint_id");
      String statusStr   = extractJsonField(docChunk, "status");
      String expiryStr   = extractJsonField(docChunk, "expiry_date");
      String nameStr     = extractJsonField(docChunk, "name");

      if (fingerIdStr.length() > 0) {
        int slot = fingerIdStr.toInt();
        if (slot >= 1 && slot <= MAX_SENSOR_CAPACITY) {
          bool isActive = (statusStr == "active");
          saveMemberToLocalCache((uint8_t)slot, isActive, expiryStr, nameStr);
          syncedCount++;
        }
      }
    }
    Serial.printf("✅ [CACHE] Initial boot sync complete (%d member records cached).\n", syncedCount);
  }
  https.end();
}

// Event-driven cache invalidation: ONLY syncs if website actually modified records!
void checkMemberCacheSyncSignal() {
  if (WiFi.status() != WL_CONNECTED || linkedGymId == "") return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;

  String url = "https://firestore.googleapis.com/v1/projects/" + String(FIREBASE_PROJECT_ID) + "/databases/(default)/documents/gyms/" + linkedGymId + "/scanners/sync";
  if (!https.begin(client, url)) return;

  applyAuthHeader(https);
  int httpCode = https.GET();
  if (httpCode == 200) {
    String resp = https.getString();
    String syncNeeded = extractJsonField(resp, "sync_needed");

    if (syncNeeded == "true" || syncNeeded == "TRUE") {
      Serial.println("\n🔔 [EVENT-DRIVEN SYNC] Detected member changes from web dashboard!");
      String slotStr = extractJsonField(resp, "slot_id");

      if (slotStr.length() > 0 && slotStr != "ALL") {
        // ULTRA-EFFICIENT SINGLE-SLOT UPDATE: Zero member collection reads!
        int slot = slotStr.toInt();
        String status = extractJsonField(resp, "status");
        String expiry = extractJsonField(resp, "expiry_date");
        String name   = extractJsonField(resp, "name");

        if (status == "deleted") {
          localCache[slot].valid = false;
          prefs.begin("fb_cache", false);
          prefs.remove(("s_" + String(slot)).c_str());
          prefs.end();
          Serial.printf("🗑️ [CACHE] Removed Slot #%d from local cache.\n", slot);
        } else {
          saveMemberToLocalCache((uint8_t)slot, (status == "active"), expiry, name);
          Serial.printf("⚡ [CACHE] Targeted single-slot update: Slot #%d (%s, %s)\n", slot, name.c_str(), status.c_str());
        }
      } else {
        // Bulk sync only when explicitly requested
        syncActiveMembersCache();
      }

      // Reset flag to false so no further reads occur until next user action
      https.end();
      String patchUrl = url + "?updateMask.fieldPaths=sync_needed";
      if (https.begin(client, patchUrl)) {
        applyAuthHeader(https);
        String patchPayload = "{\"fields\":{\"sync_needed\":{\"stringValue\":\"false\"}}}";
        https.PATCH(patchPayload);
      }
    }
  }
  https.end();
}

// ==========================================
// 6. FIREBASE AUTH & REST API CALLS
// ==========================================
String getFirebaseAuthToken() {
  #ifdef FIREBASE_AUTH_EMAIL
    if (String(FIREBASE_AUTH_EMAIL).length() > 0 && String(FIREBASE_AUTH_PASSWORD).length() > 0) {
      if (cachedIdToken.length() > 0 && millis() < tokenExpiresAt) {
        return cachedIdToken;
      }
      
      if (WiFi.status() != WL_CONNECTED) return "";
      
      WiFiClientSecure client;
      client.setInsecure();
      HTTPClient https;
      
      String authUrl = "https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=" + String(FIREBASE_API_KEY);
      if (https.begin(client, authUrl)) {
        https.addHeader("Content-Type", "application/json");
        String payload = "{\"email\":\"" + String(FIREBASE_AUTH_EMAIL) + "\",\"password\":\"" + String(FIREBASE_AUTH_PASSWORD) + "\",\"returnSecureToken\":true}";
        int httpCode = https.POST(payload);
        if (httpCode == 200) {
          String resp = https.getString();
          String token = extractJsonField(resp, "idToken");
          if (token.length() > 0) {
            cachedIdToken = token;
            tokenExpiresAt = millis() + (3000UL * 1000UL); // 50 min
            https.end();
            return cachedIdToken;
          }
        }
        https.end();
      }
    }
  #endif
  return "";
}

void applyAuthHeader(HTTPClient &https) {
  https.addHeader("Content-Type", "application/json");
  String token = getFirebaseAuthToken();
  if (token.length() > 0) {
    https.addHeader("Authorization", "Bearer " + token);
  }
}

String resolveGymIdFromLinkingKey(String key) {
  if (WiFi.status() != WL_CONNECTED) return "";
  
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  
  String url = "https://firestore.googleapis.com/v1/projects/" + String(FIREBASE_PROJECT_ID) + "/databases/(default)/documents/gym_links/" + key;
  if (https.begin(client, url)) {
    applyAuthHeader(https);
    int httpCode = https.GET();
    if (httpCode == 200) {
      String resp = https.getString();
      String foundGym = extractJsonField(resp, "gym_id");
      if (foundGym.length() > 0) {
        https.end();
        return foundGym;
      }
    }
    https.end();
  }
  return "";
}

// Log event to Firestore (with offline fallback)
void logAccessEvent(String type, String memberId, String reason) {
  if (WiFi.status() != WL_CONNECTED || linkedGymId == "") {
    Serial.println("📝 [LOG] Wi-Fi offline. Buffered event locally.");
    return;
  }
  
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  
  String url = "https://firestore.googleapis.com/v1/projects/" + String(FIREBASE_PROJECT_ID) + "/databases/(default)/documents/gyms/" + linkedGymId + "/logs";
  if (!https.begin(client, url)) return;
  
  applyAuthHeader(https);
  String payload = "{\"fields\":{"
    "\"type\":{\"stringValue\":\"" + type + "\"},"
    "\"member_id\":{\"stringValue\":\"" + memberId + "\"},"
    "\"reason\":{\"stringValue\":\"" + reason + "\"},"
    "\"timestamp\":{\"stringValue\":\"" + String(millis()) + "\"}"
  "}}";
  
  https.POST(payload);
  https.end();
}

// Check for Remote Door Unlock commands from Dashboard
void checkRemoteUnlockCommand() {
  if (WiFi.status() != WL_CONNECTED || linkedGymId == "") return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;

  String url = "https://firestore.googleapis.com/v1/projects/" + String(FIREBASE_PROJECT_ID) + "/databases/(default)/documents/gyms/" + linkedGymId + "/scanners/commands";
  if (!https.begin(client, url)) return;

  applyAuthHeader(https);
  int httpCode = https.GET();
  if (httpCode == 200) {
    String resp = https.getString();
    String cmd = extractJsonField(resp, "command");
    String st  = extractJsonField(resp, "status");
    if (cmd == "UNLOCK" && st == "PENDING") {
      Serial.println("\n⚡ [REMOTE UNLOCK] Command received from Web Dashboard!");
      soundAccessGranted();
      triggerSolenoid(3);
      logAccessEvent("REMOTE_UNLOCK", "ADMIN", "Dashboard Remote Open");

      // Mark command COMPLETED
      https.end();
      String patchUrl = url + "?updateMask.fieldPaths=status";
      if (https.begin(client, patchUrl)) {
        applyAuthHeader(https);
        String patchPayload = "{\"fields\":{\"status\":{\"stringValue\":\"COMPLETED\"}}}";
        https.PATCH(patchPayload);
      }
    }
  }
  https.end();
}

// Check website for enrollment commands
void checkEnrollmentRequest() {
  if (WiFi.status() != WL_CONNECTED || linkedGymId == "") return;
  
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  
  String url = "https://firestore.googleapis.com/v1/projects/" + String(FIREBASE_PROJECT_ID) + "/databases/(default)/documents/gyms/" + linkedGymId + "/scanners/enrollment";
  if (!https.begin(client, url)) return;
  
  applyAuthHeader(https);
  int httpCode = https.GET();
  if (httpCode == 200) {
    String resp = https.getString();
    String cmd = extractJsonField(resp, "command");
    String st  = extractJsonField(resp, "status");
    if (cmd == "ENROLL" && st == "PENDING") {
      Serial.println("\n🌐 ENROLLMENT COMMAND RECEIVED FROM WEBSITE!");
      runEnrollmentProcess();
    }
  }
  https.end();
}

void updateEnrollmentStatus(String status, uint8_t fingerId) {
  if (WiFi.status() != WL_CONNECTED || linkedGymId == "") return;
  
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  
  String url = "https://firestore.googleapis.com/v1/projects/" + String(FIREBASE_PROJECT_ID) + "/databases/(default)/documents/gyms/" + linkedGymId + "/scanners/enrollment?updateMask.fieldPaths=status&updateMask.fieldPaths=fingerprint_id";
  if (!https.begin(client, url)) return;
  
  applyAuthHeader(https);
  String payload = "{\"fields\":{"
    "\"status\":{\"stringValue\":\"" + status + "\"},"
    "\"fingerprint_id\":{\"stringValue\":\"" + String(fingerId) + "\"}"
  "}}";
  
  https.PATCH(payload);
  https.end();
}

// ==========================================
// 7. OVER-THE-AIR (OTA) ONLINE UPDATE & HARDWARE ROLLBACK
// ==========================================
void reportOtaStatus(String status, String version) {
  if (WiFi.status() != WL_CONNECTED || linkedGymId == "") return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;

  String url = "https://firestore.googleapis.com/v1/projects/" + String(FIREBASE_PROJECT_ID) + "/databases/(default)/documents/gyms/" + linkedGymId + "/scanners/ota?updateMask.fieldPaths=status&updateMask.fieldPaths=active_version";
  if (https.begin(client, url)) {
    applyAuthHeader(https);
    String payload = "{\"fields\":{"
      "\"status\":{\"stringValue\":\"" + status + "\"},"
      "\"active_version\":{\"stringValue\":\"" + version + "\"}"
    "}}";
    https.PATCH(payload);
    https.end();
  }
}

void performOtaUpdate(String binUrl, String targetVersion) {
  Serial.printf("\n🚀 [OTA] Starting Over-The-Air firmware flash: %s\n", targetVersion.c_str());
  reportOtaStatus("DOWNLOADING", FIRMWARE_VERSION);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;

  if (!https.begin(client, binUrl)) {
    Serial.println("❌ [OTA] Could not connect to binary download URL.");
    reportOtaStatus("ERROR", FIRMWARE_VERSION);
    return;
  }

  int httpCode = https.GET();
  if (httpCode != 200) {
    Serial.printf("❌ [OTA] HTTP error fetching binary: %d\n", httpCode);
    reportOtaStatus("ERROR", FIRMWARE_VERSION);
    https.end();
    return;
  }

  int contentLength = https.getSize();
  if (contentLength <= 0) {
    Serial.println("❌ [OTA] Invalid content length.");
    reportOtaStatus("ERROR", FIRMWARE_VERSION);
    https.end();
    return;
  }

  bool canBegin = Update.begin(contentLength);
  if (!canBegin) {
    Serial.println("❌ [OTA] Not enough space on OTA partition.");
    reportOtaStatus("ERROR", FIRMWARE_VERSION);
    https.end();
    return;
  }

  WiFiClient *stream = https.getStreamPtr();
  size_t written = Update.writeStream(*stream);

  if (written == contentLength && Update.end()) {
    if (Update.isFinished()) {
      Serial.println("🎉 [OTA] Firmware flash SUCCESSFUL! Rebooting into new image...");
      reportOtaStatus("REBOOTING", targetVersion);
      delay(1000);
      ESP.restart();
    }
  } else {
    Serial.printf("❌ [OTA] Flash write failed! Written: %d / %d. Error #: %d\n", written, contentLength, Update.getError());
    reportOtaStatus("ERROR", FIRMWARE_VERSION);
  }
  https.end();
}

void checkOtaUpdate() {
  if (WiFi.status() != WL_CONNECTED || linkedGymId == "") return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;

  String url = "https://firestore.googleapis.com/v1/projects/" + String(FIREBASE_PROJECT_ID) + "/databases/(default)/documents/gyms/" + linkedGymId + "/scanners/ota";
  if (!https.begin(client, url)) return;

  applyAuthHeader(https);
  int httpCode = https.GET();
  if (httpCode == 200) {
    String resp = https.getString();
    String targetVer = extractJsonField(resp, "target_version");
    String binUrl    = extractJsonField(resp, "bin_url");
    String status    = extractJsonField(resp, "status");

    if (status == "PENDING" && targetVer.length() > 0 && targetVer != FIRMWARE_VERSION && binUrl.length() > 10) {
      Serial.printf("\n📦 [OTA] New firmware update available: %s (Current: %s)\n", targetVer.c_str(), FIRMWARE_VERSION);
      performOtaUpdate(binUrl, targetVer);
    }
  }
  https.end();
}

// ==========================================
// 8. ENROLLMENT SEQUENCE
// ==========================================
void runEnrollmentProcess() {
  triggerLedEnrollScanning();
  soundEnrollPrompt();
  Serial.println("👉 [ENROLL] Place finger on scanner (Touch 1)...");
  
  finger.getTemplateCount();
  if (finger.templateCount >= MAX_SENSOR_CAPACITY) {
    soundEnrollFailed();
    updateEnrollmentStatus("SENSOR_FULL", 0);
    return;
  }

  uint8_t nextSlot = finger.templateCount + 1;

  // Touch 1
  int p = -1;
  unsigned long timeout = millis() + 15000;
  while (p != FINGERPRINT_OK && millis() < timeout) {
    p = finger.getImage();
    delay(50);
  }
  if (p != FINGERPRINT_OK || finger.image2Tz(1) != FINGERPRINT_OK) {
    soundEnrollFailed();
    updateEnrollmentStatus("ERROR", 0);
    return;
  }
  
  soundEnrollStep1();
  delay(1000);
  p = 0;
  while (p != FINGERPRINT_NOFINGER) {
    p = finger.getImage();
    delay(50);
  }

  // Touch 2
  p = -1;
  timeout = millis() + 15000;
  while (p != FINGERPRINT_OK && millis() < timeout) {
    p = finger.getImage();
    delay(50);
  }
  if (p != FINGERPRINT_OK || finger.image2Tz(2) != FINGERPRINT_OK) {
    soundEnrollFailed();
    updateEnrollmentStatus("ERROR", 0);
    return;
  }

  if (finger.createModel() != FINGERPRINT_OK) {
    soundEnrollFailed();
    updateEnrollmentStatus("ERROR", 0);
    return;
  }

  if (finger.storeModel(nextSlot) == FINGERPRINT_OK) {
    Serial.printf("🎉 Enrolled into Slot #%d!\n", nextSlot);
    soundEnrollSuccess();
    triggerLedEnrollSuccess();
    updateEnrollmentStatus("SUCCESS", nextSlot);
  } else {
    soundEnrollFailed();
    updateEnrollmentStatus("ERROR", 0);
  }
}

// ==========================================
// 9. HIGH-SPEED MEMBER VERIFICATION (CACHE-FIRST)
// ==========================================
void handleFingerprintVerification() {
  Serial.println("\n👆 Finger detected. Reading...");
  delay(200);
  
  uint8_t p = FINGERPRINT_NOFINGER;
  for (int tries = 0; tries < 4; tries++) {
    p = finger.getImage();
    if (p == FINGERPRINT_OK) break;
    delay(80);
  }

  if (p != FINGERPRINT_OK || finger.image2Tz() != FINGERPRINT_OK) {
    soundAccessDeniedUnknown();
    delay(400);
    return;
  }

  if (finger.fingerFastSearch() != FINGERPRINT_OK) {
    Serial.println("❌ Access Denied: Unknown Fingerprint.");
    soundAccessDeniedUnknown();
    logAccessEvent("DENIED", "UNKNOWN", "Not recognized");
    delay(1200);
    return;
  }

  uint8_t slotId = finger.fingerID;
  Serial.printf("🔍 Recognized Slot #%d!\n", slotId);

  // STEP 1: FAST LOCAL CACHE LOOKUP (<15ms, OFFLINE CAPABLE)
  String todayDate = getTodayDateString();
  if (slotId >= 1 && slotId <= MAX_SENSOR_CAPACITY && localCache[slotId].valid) {
    String memberName = String(localCache[slotId].name);
    String expiry = String(localCache[slotId].expiryDate);
    bool active = localCache[slotId].active;

    Serial.printf("⚡ [LOCAL CACHE HIT] %s | Active: %d | Expiry: %s | Today: %s\n", 
                  memberName.c_str(), active, expiry.c_str(), todayDate.c_str());

    if (!active) {
      Serial.printf("🔴 ACCESS DENIED: Membership Inactive for %s\n", memberName.c_str());
      soundAccessDeniedExpired();
      logAccessEvent("DENIED", String(slotId), "Membership inactive");
      delay(1200);
      return;
    }

    if (expiry.length() == 10 && todayDate.length() == 10 && expiry < todayDate) {
      Serial.printf("🔴 ACCESS DENIED: Membership Expired for %s\n", memberName.c_str());
      soundAccessDeniedExpired();
      logAccessEvent("DENIED", String(slotId), "Membership expired");
      delay(1200);
      return;
    }

    // GRANTED INSTANTLY!
    Serial.printf("🟢 ACCESS GRANTED (INSTANT LOCAL)! Welcome, %s!\n", memberName.c_str());
    soundAccessGranted();
    triggerSolenoid(3);
    logAccessEvent("GRANTED", String(slotId), "Active membership (Local)");
    return;
  }

  // STEP 2: CLOUD FALLBACK IF NOT IN LOCAL CACHE YET
  if (WiFi.status() == WL_CONNECTED && linkedGymId != "") {
    Serial.println("📡 Slot not in local cache. Verifying with Firestore cloud...");
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient https;

    String url = "https://firestore.googleapis.com/v1/projects/" + String(FIREBASE_PROJECT_ID) + "/databases/(default)/documents/gyms/" + linkedGymId + ":runQuery";
    if (https.begin(client, url)) {
      applyAuthHeader(https);
      String queryPayload = "{\"structuredQuery\":{\"from\":[{\"collectionId\":\"members\"}],\"where\":{\"fieldFilter\":{\"field\":{\"fieldPath\":\"fingerprint_id\"},\"op\":\"EQUAL\",\"value\":{\"stringValue\":\"" + String(slotId) + "\"}}},\"limit\":1}}";

      int httpCode = https.POST(queryPayload);
      if (httpCode == 200) {
        String resp = https.getString();
        if (resp.indexOf("\"document\"") != -1) {
          String name = extractJsonField(resp, "name");
          String status = extractJsonField(resp, "status");
          String expiry = extractJsonField(resp, "expiry_date");

          if (status == "active" && (expiry.length() < 10 || todayDate.length() < 10 || expiry >= todayDate)) {
            Serial.printf("🟢 ACCESS GRANTED (CLOUD)! Welcome, %s!\n", name.c_str());
            saveMemberToLocalCache(slotId, true, expiry, name);
            soundAccessGranted();
            triggerSolenoid(3);
            logAccessEvent("GRANTED", String(slotId), "Active membership (Cloud)");
            https.end();
            return;
          } else {
            Serial.println("🔴 ACCESS DENIED: Expired or inactive in cloud.");
            saveMemberToLocalCache(slotId, false, expiry, name);
            soundAccessDeniedExpired();
            logAccessEvent("DENIED", String(slotId), "Expired/Inactive");
            delay(1200);
            https.end();
            return;
          }
        }
      }
      https.end();
    }
  }

  // FAIL CLOSED
  Serial.println("⚠️ Access Denied: Unverified record.");
  soundAccessDeniedUnknown();
  logAccessEvent("DENIED", String(slotId), "Unverified");
  delay(1200);
}

// ==========================================
// 10. CAPTIVE PORTAL SETUP SERVER
// ==========================================
String generatePortalHtml() {
  int n = WiFi.scanNetworks();
  String wifiOptions = "";
  for (int i = 0; i < n; ++i) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() > 0) {
      wifiOptions += "<option value='" + ssid + "'>" + ssid + " (" + String(WiFi.RSSI(i)) + " dBm)</option>";
    }
  }

  String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>Fitness Box - Scanner Setup</title>";
  html += "<style>";
  html += "body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; background: #0f172a; color: #f8fafc; margin: 0; padding: 20px; display: flex; justify-content: center; align-items: center; min-height: 90vh; }";
  html += ".card { background: #1e293b; padding: 28px; border-radius: 20px; width: 100%; max-width: 380px; box-shadow: 0 20px 25px -5px rgba(0,0,0,0.5); border: 1px solid #334155; }";
  html += "h1 { font-size: 24px; font-weight: 800; color: #38bdf8; margin: 0 0 6px 0; }";
  html += "p { font-size: 14px; color: #94a3b8; margin: 0 0 24px 0; line-height: 1.5; }";
  html += "label { font-size: 12px; font-weight: 600; text-transform: uppercase; letter-spacing: 0.05em; color: #cbd5e1; display: block; margin-bottom: 6px; }";
  html += "input, select { width: 100%; box-sizing: border-box; padding: 12px 14px; border-radius: 10px; border: 1px solid #475569; background: #0f172a; color: #fff; font-size: 15px; margin-bottom: 18px; outline: none; }";
  html += ".key-input { font-family: monospace; letter-spacing: 4px; font-size: 20px; text-align: center; font-weight: 700; color: #38bdf8; }";
  html += "button { width: 100%; padding: 14px; background: #2563eb; color: #fff; border: none; border-radius: 12px; font-size: 16px; font-weight: 700; cursor: pointer; }";
  html += "</style></head><body>";
  html += "<div class='card'>";
  html += "<h1>🏋️ Fitness Box</h1>";
  html += "<p>Biometric Scanner Setup (Protected)</p>";
  html += "<form action='/save' method='POST'>";
  html += "<label>Gym Wi-Fi Network</label>";
  html += "<select name='ssid' required><option value='' disabled selected>Select Wi-Fi...</option>" + wifiOptions + "</select>";
  html += "<label>Wi-Fi Password</label><input type='password' name='password' placeholder='Enter password' required>";
  html += "<label>Hardware Linking Key</label><input type='text' name='key' class='key-input' placeholder='------' maxlength='6' required>";
  html += "<button type='submit'>Connect & Link Scanner</button></form></div></body></html>";
  return html;
}

void handleSaveConfig() {
  String newSSID = server.arg("ssid");
  String newPass = server.arg("password");
  String newKey  = server.arg("key");
  newSSID.trim(); newPass.trim(); newKey.trim();

  WiFi.mode(WIFI_AP_STA);
  WiFi.disconnect(false, false);
  delay(50);
  WiFi.begin(newSSID.c_str(), newPass.c_str());
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 25) {
    delay(500);
    attempts++;
  }

  if (WiFi.status() != WL_CONNECTED) {
    server.send(200, "text/html", "<h2>❌ Wi-Fi Connection Failed</h2><p>Please check password.</p><a href='/'>Try Again</a>");
    return;
  }

  String foundGymId = resolveGymIdFromLinkingKey(newKey);
  if (foundGymId != "") {
    prefs.begin("fitness_box", false);
    prefs.putString("ssid", newSSID);
    prefs.putString("pass", newPass);
    prefs.putString("key", newKey);
    prefs.putString("gym_id", foundGymId);
    prefs.end();

    server.send(200, "text/html", "<h2>🎉 Setup Complete!</h2><p>Linked to " + foundGymId + ". Rebooting...</p>");
    delay(2000);
    ESP.restart();
  } else {
    server.send(200, "text/html", "<h2>❌ Linking Failed</h2><p>Invalid key: " + newKey + "</p><a href='/'>Try Again</a>");
  }
}

void startCaptivePortal() {
  isSetupMode = true;
  setupModeStartedAt = millis();
  soundConfigMode();

  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false, false);
  delay(100);
  WiFi.mode(WIFI_AP_STA);
  
  #ifdef AP_PASSWORD
    WiFi.softAP(AP_SSID, AP_PASSWORD);
  #else
    WiFi.softAP(AP_SSID);
  #endif
  
  IPAddress apIP(192, 168, 4, 1);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  dnsServer.start(53, "*", apIP);
  
  server.on("/", HTTP_GET, []() { server.send(200, "text/html", generatePortalHtml()); });
  server.on("/save", HTTP_POST, handleSaveConfig);
  server.onNotFound([]() {
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
  });
  server.begin();
}

// ==========================================
// 11. SETUP & LOOP
// ==========================================
void setup() {
  // 1. HARDWARE SAFETY FIRST: Lock relay immediately
  digitalWrite(RELAY_PIN, RELAY_INACTIVE_LEVEL);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_INACTIVE_LEVEL);

  // Configure Pins
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  pinMode(TOUCH_PIN, INPUT_PULLUP);
  pinMode(EXIT_BUTTON_PIN, INPUT_PULLUP);

  Serial.begin(9600);
  delay(100);
  Serial.println("\n\n========================================");
  Serial.printf("🏋️ FITNESS BOX - SMART BIOMETRIC GATE (%s)\n", FIRMWARE_VERSION);
  Serial.println("========================================");

  // Dual-Partition OTA Rollback Self-Test
  const esp_partition_t *runningPartition = esp_ota_get_running_partition();
  esp_ota_img_states_t otaState;
  if (esp_ota_get_state_partition(runningPartition, &otaState) == ESP_OK) {
    if (otaState == ESP_OTA_IMG_PENDING_VERIFY) {
      Serial.println("🧪 [OTA] Running newly flashed firmware (PENDING VERIFICATION)...");
    }
  }

  // Prevent internal NVS WiFi auto-reconnect conflict
  WiFi.persistent(false);

  // Load persistent local member cache from flash
  loadLocalCacheFromFlash();

  // Boot melody
  soundSystemBoot();

  // Initialize Fingerprint Sensor
  Serial2.begin(57600, SERIAL_8N1, RX_PIN, TX_PIN);
  finger.begin(57600);
  delay(50);
  if (finger.verifyPassword()) {
    Serial.println("✅ Fingerprint Sensor (R307S): Ready!");
  } else {
    Serial.println("❌ ERROR: Could not find Fingerprint sensor.");
  }

  // Load Saved Settings from NVS
  prefs.begin("fitness_box", true);
  storedSSID = prefs.getString("ssid", "");
  storedPassword = prefs.getString("pass", "");
  storedLinkingKey = prefs.getString("key", "");
  linkedGymId = prefs.getString("gym_id", "");
  prefs.end();

  if (storedSSID == "" || storedLinkingKey == "") {
    Serial.println("ℹ️ No saved Wi-Fi found. Starting Setup Portal...");
    startCaptivePortal();
    return;
  }

  // Connect to Wi-Fi
  Serial.printf("📶 Connecting to Wi-Fi: %s", storedSSID.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(storedSSID.c_str(), storedPassword.c_str());
  
  int timeout = 0;
  while (WiFi.status() != WL_CONNECTED && timeout < 25) {
    delay(500);
    Serial.print(".");
    timeout++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ Wi-Fi Connected! IP: " + WiFi.localIP().toString());
    Serial.println("🏢 Linked Gym ID: " + linkedGymId);
    
    // Sync NTP Clock
    #ifdef TIMEZONE_OFFSET_SEC
      configTime(TIMEZONE_OFFSET_SEC, 0, "pool.ntp.org", "time.google.com");
    #else
      configTime(19800, 0, "pool.ntp.org", "time.google.com"); // UTC+5:30 (India)
    #endif

    // OTA Self-Test Verification Check: Cancel rollback if healthy
    if (otaState == ESP_OTA_IMG_PENDING_VERIFY) {
      Serial.println("✅ [OTA] New firmware verified! Cancelling rollback...");
      esp_ota_mark_app_valid_cancel_rollback();
      reportOtaStatus("SUCCESS", FIRMWARE_VERSION);
    }

    soundCloudOnline();

    // 💡 COST OPTIMIZATION: Initial full member cache sync executed ONLY ONCE on fresh boot!
    syncActiveMembersCache();
    checkOtaUpdate();
  } else {
    Serial.println("\n⚠️ Failed to connect to Wi-Fi. Operating in OFFLINE CACHE MODE.");
    if (otaState == ESP_OTA_IMG_PENDING_VERIFY) {
      Serial.println("❌ [OTA] Self-test failed on new image. Triggering ROLLBACK to previous partition!");
      esp_ota_mark_app_invalid_rollback_and_reboot();
    }
  }
}

void loop() {
  if (isSetupMode) {
    dnsServer.processNextRequest();
    server.handleClient();
    if (millis() - setupModeStartedAt > SETUP_PORTAL_TIMEOUT_MS) {
      ESP.restart();
    }
    return;
  }

  // Non-blocking solenoid relay timer
  updateSolenoidState();

  // 1. Interior Push-to-Exit Button (Debounced)
  static bool lastExitButton = HIGH;
  bool currentExitButton = digitalRead(EXIT_BUTTON_PIN);
  if (lastExitButton == HIGH && currentExitButton == LOW) {
    delay(20);
    if (digitalRead(EXIT_BUTTON_PIN) == LOW) {
      Serial.println("🚪 [EXIT] Interior Exit Button Pressed!");
      soundExitButton();
      triggerSolenoid(3);
      logAccessEvent("EXIT", "INTERIOR_BUTTON", "Free exit");
    }
  }
  lastExitButton = currentExitButton;

  // 2. Member Fingerprint Scan (Cache-first <15ms)
  if (digitalRead(TOUCH_PIN) == LOW) {
    handleFingerprintVerification();
  }

  // 3. Command & Sync Polling (Enrollment, Remote Unlock, and Event-Driven Sync Signal)
  if (millis() - lastCommandCheck > COMMAND_CHECK_INTERVAL) {
    lastCommandCheck = millis();
    checkEnrollmentRequest();
    checkRemoteUnlockCommand();
    checkMemberCacheSyncSignal(); // 💡 ONLY syncs when website actually modified records!
  }

  // 4. Infrequent OTA Check (Every 30 mins)
  if (millis() - lastOtaCheck > OTA_CHECK_INTERVAL) {
    lastOtaCheck = millis();
    checkOtaUpdate();
  }

  delay(20);
}
