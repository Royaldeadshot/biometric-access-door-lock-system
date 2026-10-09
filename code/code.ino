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

// ==========================================
// 1. PIN DEFINITIONS & CONSTANTS
// ==========================================
// Fingerprint Sensor (R307S)
#define RX_PIN 16         // ESP32 RX2 <- R307S TX (Yellow Wire)
#define TX_PIN 17         // ESP32 TX2 -> R307S RX (Green Wire)
#define TOUCH_PIN 4       // ESP32 GPIO4 <- R307S Touch Out (Blue Wire)
                          // (White Wire connects to ESP32 3.3V)

// Audio Feedback (Active Buzzer)
#define BUZZER_PIN 19     // Active Buzzer Positive (+) -> GPIO 19. Negative (-) -> GND

// Door Lock Relay (Solenoid)
#define RELAY_PIN 25      // ESP32 GPIO 25 (Pin D25) -> Relay IN pin
// Most 1-channel relay modules are ACTIVE LOW.
// LOW (0V) triggers relay ON, HIGH (3.3V) turns relay OFF.
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

unsigned long lastEnrollCheck = 0;
const unsigned long ENROLL_CHECK_INTERVAL = 3000;

// Non-blocking Solenoid State
bool isSolenoidUnlocked = false;
unsigned long solenoidRelockAt = 0;

// Firebase Auth Token Caching
String cachedIdToken = "";
unsigned long tokenExpiresAt = 0;

// ==========================================
// 2. AESTHETIC ACTIVE BUZZER AUDIO SUITE
// Micro-burst cadence & rhythmic dynamics for active buzzers.
// ==========================================
void activeBeep(int durationMs, int pauseMs = 35) {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(durationMs);
  digitalWrite(BUZZER_PIN, LOW);
  if (pauseMs > 0) delay(pauseMs);
}

// 1. Access Granted: Upbeat Ascending Triple-Pip Chime (VIP Welcome!)
void soundAccessGranted() {
  activeBeep(28, 25);  // Crisp initial pip
  activeBeep(42, 30);  // Rising step
  activeBeep(140, 0);  // Warm confirmation bloom
}

// 2. Access Denied (Unknown Finger / Retry): Polite Soft Double-Tick
void soundAccessDeniedUnknown() {
  activeBeep(22, 55);  // Gentle tap
  activeBeep(22, 0);   // Gentle tap
}

// 3. Access Denied (Expired Membership): Authoritative Syncopated Warning
void soundAccessDeniedExpired() {
  activeBeep(70, 50);  // Alert 1
  activeBeep(70, 110); // Hesitating pause
  activeBeep(200, 0);  // Deep authoritative warning pulse
}

// 4. Interior Push-to-Exit: Snappy Tactile Unlock Pop
void soundExitButton() {
  activeBeep(32, 0);   // Instant micro-click confirmation
}

// 5. System Power-On / Boot: Futuristic 4-Stage Acceleration Sequence
void soundSystemBoot() {
  Serial.println("🔊 [SYSTEM BOOT] Power-on sequence...");
  activeBeep(25, 30);
  activeBeep(35, 30);
  activeBeep(50, 40);
  activeBeep(160, 0);
}

// 6. Wi-Fi Connected & Armed: Triumphant Cloud Handshake
void soundCloudOnline() {
  Serial.println("🔊 [CLOUD ONLINE] Connected & armed!");
  delay(80);
  activeBeep(38, 45);
  activeBeep(200, 0);
}

// 7. Enrollment Mode Invitation: 3-Pip Rising Prompt ("Ready for Finger")
void soundEnrollPrompt() {
  activeBeep(40, 40);
  activeBeep(40, 40);
  activeBeep(85, 0);
}

// 8. Enrollment Step 1 Captured: Crisp Camera-Shutter Blip ("Remove Finger")
void soundEnrollStep1() {
  activeBeep(55, 0);
}

// 9. Enrollment Success: 5-Beat Celebration Fanfare
void soundEnrollSuccess() {
  activeBeep(30, 25);
  activeBeep(30, 25);
  activeBeep(30, 55);
  activeBeep(65, 35);
  activeBeep(260, 0);
}

// 10. Enrollment Failed / Timed Out: Rapid 4-Stutter Drop
void soundEnrollFailed() {
  for (int i = 0; i < 4; i++) {
    activeBeep(45, 35);
  }
}

// 11. Wi-Fi Setup / Captive Portal Active: Setup Beacon Chime
void soundConfigMode() {
  activeBeep(45, 45);
  activeBeep(45, 120);
  activeBeep(90, 0);
}

inline void playDronePowerUpTones() { soundSystemBoot(); }
inline void playDroneArmedTones()   { soundCloudOnline(); }

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
// Robust field extractor supporting formatting variations
String extractJsonField(const String &json, const String &fieldName) {
  int fieldIdx = json.indexOf("\"" + fieldName + "\"");
  if (fieldIdx == -1) return "";

  int valIdx = json.indexOf("\"stringValue\"", fieldIdx);
  if (valIdx == -1) return "";

  int colonIdx = json.indexOf(":", valIdx);
  if (colonIdx == -1) return "";

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
// 5. FIREBASE AUTHENTICATION (TOKEN HELPER)
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
            tokenExpiresAt = millis() + (3000UL * 1000UL); // Refresh after 50 minutes
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

// ==========================================
// 6. FIREBASE CLOUD LINKING (TLS / HTTPS)
// ==========================================
String resolveGymIdFromLinkingKey(String key) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ Wi-Fi not connected!");
    return "";
  }
  
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  
  String url = "https://firestore.googleapis.com/v1/projects/" + String(FIREBASE_PROJECT_ID) + "/databases/(default)/documents/gym_links/" + key;
  Serial.println("\n📡 [1/2] Secure HTTPS Query: " + url);
  
  if (https.begin(client, url)) {
    applyAuthHeader(https);
    int httpCode = https.GET();
    String resp = https.getString();
    Serial.printf("📡 HTTP Response Code: %d\n", httpCode);
    
    if (httpCode == 200) {
      String foundGym = extractJsonField(resp, "gym_id");
      if (foundGym.length() > 0) {
        https.end();
        Serial.println("✅ Found Gym ID in gym_links: " + foundGym);
        return foundGym;
      }
    } else {
      Serial.println("📡 Server Response: " + resp);
    }
    https.end();
  }
  
  // Structured query fallback
  url = "https://firestore.googleapis.com/v1/projects/" + String(FIREBASE_PROJECT_ID) + "/databases/(default)/documents:runQuery";
  Serial.println("📡 [2/2] Trying structured query fallback...");
  
  if (https.begin(client, url)) {
    applyAuthHeader(https);
    String query = "{"
      "\"structuredQuery\": {"
        "\"from\": [{\"collectionId\": \"gyms\"}], "
        "\"where\": {"
          "\"fieldFilter\": {"
            "\"field\": {\"fieldPath\": \"hardware_linking_key\"},"
            "\"op\": \"EQUAL\","
            "\"value\": {\"stringValue\": \"" + key + "\"}"
          "}"
        "},"
        "\"limit\": 1"
      "}"
    "}";
    
    int httpCode = https.POST(query);
    String resp = https.getString();
    Serial.printf("📡 Fallback HTTP Code: %d\n", httpCode);
    
    if (httpCode == 200) {
      int nameIdx = resp.indexOf("/gyms/");
      if (nameIdx != -1) {
        int endIdx = resp.indexOf("\"", nameIdx + 6);
        String foundGym = resp.substring(nameIdx + 6, endIdx);
        https.end();
        Serial.println("✅ Found Gym ID via query: " + foundGym);
        return foundGym;
      }
    }
    https.end();
  }
  
  return "";
}

// Strict Fail-Closed Member Verification
int verifyMemberInFirestore(uint8_t fingerId, String &memberName) {
  if (WiFi.status() != WL_CONNECTED || linkedGymId == "") return 0;
  
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  
  String url = "https://firestore.googleapis.com/v1/projects/" + String(FIREBASE_PROJECT_ID) + "/databases/(default)/documents/gyms/" + linkedGymId + ":runQuery";
  Serial.printf("\n📡 Checking cloud for Fingerprint #%d in Gym [%s]...\n", fingerId, linkedGymId.c_str());
  if (!https.begin(client, url)) return 0;
  
  applyAuthHeader(https);
  String queryPayload = "{"
    "\"structuredQuery\": {"
      "\"from\": [{\"collectionId\": \"members\"}], "
      "\"where\": {"
        "\"fieldFilter\": {"
          "\"field\": {\"fieldPath\": \"fingerprint_id\"},"
          "\"op\": \"EQUAL\","
          "\"value\": {\"stringValue\": \"" + String(fingerId) + "\"}"
        "}"
      "},"
      "\"limit\": 1"
    "}"
  "}";
  
  int httpCode = https.POST(queryPayload);
  int result = 0; // STRICT DEFAULT: 0 (DENIED / UNKNOWN) - NEVER FAILS OPEN!
  
  if (httpCode == 200) {
    String resp = https.getString();
    if (resp.indexOf("\"document\"") != -1) {
      String name = extractJsonField(resp, "name");
      if (name.length() > 0) memberName = name;
      
      String status = extractJsonField(resp, "status");
      String expiryDate = extractJsonField(resp, "expiry_date");
      String todayDate = getTodayDateString();
      
      Serial.printf("🔍 Member: %s | Status: %s | Expiry: %s | Today: %s\n", 
                    memberName.c_str(), status.c_str(), expiryDate.c_str(), todayDate.c_str());
      
      // FAIL-CLOSED EVALUATION:
      // 1. Explicitly expired status -> Deny
      // 2. Expiry date in the past -> Deny (protects revenue even if cloud status sync was pending)
      // 3. Status active AND not expired -> Grant
      // 4. Anything else (missing, malformed, empty) -> Deny
      if (status == "expired") {
        result = 2; // Expired
      } else if (expiryDate.length() == 10 && todayDate.length() == 10 && expiryDate < todayDate) {
        Serial.println("⚠️ Member expired by calendar date! Denying entry.");
        result = 2; // Expired
      } else if (status == "active") {
        result = 1; // Granted
      } else {
        Serial.printf("⚠️ Unverified status '%s'. Failing closed (Access Denied).\n", status.c_str());
        result = 0; // Denied
      }
    } else {
      Serial.println("ℹ️ No matching member record found for this fingerprint ID.");
      result = 0;
    }
  } else {
    Serial.printf("❌ Firestore query failed. HTTP Code: %d\n", httpCode);
    result = 0;
  }
  https.end();
  return result;
}

// Log event to Firestore
void logAccessEvent(String type, String memberId, String reason) {
  if (WiFi.status() != WL_CONNECTED || linkedGymId == "") return;
  
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
// 7. ENROLLMENT SEQUENCE (WITH CAPACITY GUARDS)
// ==========================================
void runEnrollmentProcess() {
  triggerLedEnrollScanning();
  soundEnrollPrompt();
  Serial.println("👉 [ENROLL] Place finger on scanner (Touch 1)...");
  
  finger.getTemplateCount();
  if (finger.templateCount >= MAX_SENSOR_CAPACITY) {
    Serial.printf("❌ Sensor memory full (%d/%d templates). Cannot enroll more.\n", 
                  finger.templateCount, MAX_SENSOR_CAPACITY);
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
  
  Serial.println("👍 Image 1 captured! Remove finger...");
  soundEnrollStep1();
  delay(1000);
  p = 0;
  while (p != FINGERPRINT_NOFINGER) {
    p = finger.getImage();
    delay(50);
  }

  // Touch 2
  Serial.println("👉 Place the SAME finger again (Touch 2)...");
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
    Serial.println("❌ Fingerprints did not match.");
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
// 8. MEMBER VERIFICATION
// ==========================================
void handleFingerprintVerification() {
  Serial.println("\n👆 Finger detected. Reading...");
  delay(250);
  
  uint8_t p = FINGERPRINT_NOFINGER;
  for (int tries = 0; tries < 4; tries++) {
    p = finger.getImage();
    if (p == FINGERPRINT_OK) break;
    delay(100);
  }

  if (p != FINGERPRINT_OK || finger.image2Tz() != FINGERPRINT_OK) {
    Serial.println("⚠️ Could not read fingerprint.");
    soundAccessDeniedUnknown();
    delay(500);
    return;
  }

  if (finger.fingerFastSearch() != FINGERPRINT_OK) {
    Serial.println("❌ Access Denied: Unknown Fingerprint.");
    triggerLedAccessDeniedUnknown();
    soundAccessDeniedUnknown();
    logAccessEvent("DENIED", "UNKNOWN", "Not recognized");
    delay(1500);
    return;
  }

  Serial.printf("🔍 Recognized Slot #%d!\n", finger.fingerID);
  String memberName = "Member";
  int memberStatus = verifyMemberInFirestore(finger.fingerID, memberName);
  
  if (memberStatus == 1) {
    Serial.printf("🟢 ACCESS GRANTED! Welcome, %s!\n", memberName.c_str());
    soundAccessGranted();
    triggerLedAccessGranted();
    logAccessEvent("GRANTED", String(finger.fingerID), "Active membership");
    triggerSolenoid(3);
  } else if (memberStatus == 2) {
    Serial.printf("🔴 ACCESS DENIED: Membership Expired for %s.\n", memberName.c_str());
    soundAccessDeniedExpired();
    triggerLedAccessDeniedExpired();
    logAccessEvent("DENIED", String(finger.fingerID), "Membership expired");
    delay(1500);
  } else {
    Serial.println("⚠️ Access Denied: Record unverified or inactive.");
    triggerLedAccessDeniedUnknown();
    soundAccessDeniedUnknown();
    logAccessEvent("DENIED", String(finger.fingerID), "Record unverified");
    delay(1500);
  }
}

// ==========================================
// 9. SECURE CAPTIVE PORTAL SETUP SERVER
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
  html += "input:focus, select:focus { border-color: #38bdf8; box-shadow: 0 0 0 2px rgba(56,189,248,0.2); }";
  html += ".key-input { font-family: monospace; letter-spacing: 4px; font-size: 20px; text-align: center; font-weight: 700; color: #38bdf8; }";
  html += ".hint { font-size: 11px; color: #64748b; margin-top: -12px; margin-bottom: 18px; }";
  html += "button { width: 100%; padding: 14px; background: #2563eb; color: #fff; border: none; border-radius: 12px; font-size: 16px; font-weight: 700; cursor: pointer; }";
  html += "</style></head><body>";
  html += "<div class='card'>";
  html += "<h1>🏋️ Fitness Box</h1>";
  html += "<p>Biometric Scanner Setup (Protected)</p>";
  html += "<form action='/save' method='POST'>";
  
  html += "<label>Gym Wi-Fi Network</label>";
  html += "<select name='ssid' required>";
  html += "<option value='' disabled selected>Select your Wi-Fi...</option>";
  html += wifiOptions;
  html += "</select>";
  
  html += "<label>Wi-Fi Password</label>";
  html += "<input type='password' name='password' placeholder='Enter Wi-Fi password' required>";
  
  html += "<label>Hardware Linking Key</label>";
  html += "<input type='text' name='key' class='key-input' placeholder='------' maxlength='6' required>";
  html += "<div class='hint'>Found in Web Dashboard &rarr; Settings &rarr; Biometric Setup</div>";
  
  html += "<button type='submit'>Connect & Link Scanner</button>";
  html += "</form></div></body></html>";
  return html;
}

void handleSaveConfig() {
  String newSSID = server.arg("ssid");
  String newPass = server.arg("password");
  String newKey  = server.arg("key");
  
  newSSID.trim();
  newPass.trim();
  newKey.trim();

  Serial.println("\n📥 Received Setup Request:");
  Serial.println("SSID: " + newSSID);
  Serial.println("Key: " + newKey);

  // Connect to Wi-Fi
  WiFi.mode(WIFI_AP_STA);
  WiFi.disconnect(false, false);
  delay(50);
  WiFi.begin(newSSID.c_str(), newPass.c_str());
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 25) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\n❌ Wi-Fi Connection Failed!");
    String html = "<!DOCTYPE html><html><body style='background:#0f172a;color:#fff;font-family:sans-serif;padding:30px;text-align:center;'>";
    html += "<h2 style='color:#ef4444;'>❌ Wi-Fi Connection Failed</h2>";
    html += "<p>Could not connect to <b>" + newSSID + "</b>. Please check password.</p>";
    html += "<a href='/' style='color:#38bdf8;text-decoration:none;font-weight:bold;'>&larr; Try Again</a></body></html>";
    server.send(200, "text/html", html);
    return;
  }

  Serial.println("\n✅ Connected to Wi-Fi! IP: " + WiFi.localIP().toString());
  
  // Resolve Gym ID
  String foundGymId = resolveGymIdFromLinkingKey(newKey);
  
  if (foundGymId != "") {
    Serial.println("🎉 Successfully linked to Gym: " + foundGymId);
    
    // Save permanently to NVS
    prefs.begin("fitness_box", false);
    prefs.putString("ssid", newSSID);
    prefs.putString("pass", newPass);
    prefs.putString("key", newKey);
    prefs.putString("gym_id", foundGymId);
    prefs.end();

    String html = "<!DOCTYPE html><html><body style='background:#0f172a;color:#fff;font-family:sans-serif;padding:30px;text-align:center;'>";
    html += "<h2 style='color:#22c55e;'>🎉 Setup Complete!</h2>";
    html += "<p>Scanner successfully linked to Gym: <b>" + foundGymId + "</b>.</p>";
    html += "<p>Rebooting into Live Mode now... You can close this window!</p></body></html>";
    server.send(200, "text/html", html);
    delay(2000);

    Serial.println("\n🔄 Setup complete! Rebooting ESP32 into Live Mode now...\n");
    ESP.restart();
  } else {
    Serial.println("❌ Invalid Linking Key or verification failed!");
    String html = "<!DOCTYPE html><html><body style='background:#0f172a;color:#fff;font-family:sans-serif;padding:30px;text-align:center;'>";
    html += "<h2 style='color:#ef4444;'>❌ Linking Failed</h2>";
    html += "<p>Connected to Wi-Fi, but could not link with key: <b>" + newKey + "</b>.</p>";
    html += "<a href='/' style='color:#38bdf8;text-decoration:none;font-weight:bold;'>&larr; Try Again</a></body></html>";
    server.send(200, "text/html", html);
  }
}

void startCaptivePortal() {
  isSetupMode = true;
  setupModeStartedAt = millis();
  soundConfigMode();
  Serial.println("\n==========================================");
  Serial.println("📡 ENTERING WI-FI SETUP & CAPTIVE PORTAL (PROTECTED)");
  Serial.println("==========================================");
  
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false, false);
  delay(100);
  WiFi.mode(WIFI_AP_STA);
  
  // WPA2 Password Protected SoftAP
  #ifdef AP_PASSWORD
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    Serial.printf("Broadcasting Wi-Fi: %s (Password protected)\n", AP_SSID);
  #else
    WiFi.softAP(AP_SSID);
    Serial.printf("Broadcasting Wi-Fi: %s\n", AP_SSID);
  #endif
  
  IPAddress apIP(192, 168, 4, 1);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  
  Serial.println("Portal IP: http://192.168.4.1");
  dnsServer.start(53, "*", apIP);
  
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", generatePortalHtml());
  });
  
  server.on("/save", HTTP_POST, handleSaveConfig);
  server.on("/generate_204", []() { server.send(200, "text/html", generatePortalHtml()); });
  server.on("/gen_204", []() { server.send(200, "text/html", generatePortalHtml()); });
  server.on("/hotspot-detect.html", []() { server.send(200, "text/html", generatePortalHtml()); });
  server.on("/ncsi.txt", []() { server.send(200, "text/plain", "Microsoft NCSI"); });
  server.onNotFound([]() {
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
  });
  
  server.begin();
  Serial.println("✅ Captive Portal Server Started.");
}

// ==========================================
// 10. SETUP & LOOP
// ==========================================
void setup() {
  // 1. HARDWARE SAFETY FIRST: Lock door pin immediately before any code, serial init, or boot melodies
  digitalWrite(RELAY_PIN, RELAY_INACTIVE_LEVEL);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_INACTIVE_LEVEL);

  // Configure Other Pins
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  pinMode(TOUCH_PIN, INPUT_PULLUP);
  pinMode(EXIT_BUTTON_PIN, INPUT_PULLUP);

  Serial.begin(9600);
  delay(100);
  Serial.println("\n\n========================================");
  Serial.println("🏋️ FITNESS BOX - SMART BIOMETRIC SYSTEM (SECURE)");
  Serial.println("========================================");

  // Prevent ESP-IDF auto-connect from stale internal NVS settings
  WiFi.persistent(false);

  // Play power-on boot sequence
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

  // Load Saved Settings from Flash memory
  prefs.begin("fitness_box", true);
  storedSSID = prefs.getString("ssid", "");
  storedPassword = prefs.getString("pass", "");
  storedLinkingKey = prefs.getString("key", "");
  linkedGymId = prefs.getString("gym_id", "");
  prefs.end();

  // If no Wi-Fi credentials saved, enter Setup Portal immediately
  if (storedSSID == "" || storedLinkingKey == "") {
    Serial.println("ℹ️ No saved Wi-Fi or Linking Key found. Launching Setup Portal...");
    startCaptivePortal();
    return;
  }

  // Try connecting to saved Wi-Fi
  Serial.printf("📶 Connecting to saved Wi-Fi: %s", storedSSID.c_str());
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
    
    // Synchronize NTP Real-time Clock (for accurate member expiration checks)
    #ifdef TIMEZONE_OFFSET_SEC
      configTime(TIMEZONE_OFFSET_SEC, 0, "pool.ntp.org", "time.google.com");
    #else
      configTime(19800, 0, "pool.ntp.org", "time.google.com"); // UTC+5:30 default
    #endif
    Serial.println("⏰ NTP Time synchronization initialized.");
    
    soundCloudOnline();
  } else {
    Serial.println("\n⚠️ Failed to connect to saved Wi-Fi. Launching Setup Portal fallback...");
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(false, false);
    delay(100);
    startCaptivePortal();
  }
}

void loop() {
  if (isSetupMode) {
    dnsServer.processNextRequest();
    server.handleClient();
    
    // Auto-restart recovery after timeout to retry Wi-Fi in case of transient power/router reset
    if (millis() - setupModeStartedAt > SETUP_PORTAL_TIMEOUT_MS) {
      Serial.println("🔄 Captive Portal timeout reached. Rebooting to retry saved Wi-Fi...");
      ESP.restart();
    }
    return;
  }

  // Always service non-blocking door lock timer
  updateSolenoidState();

  // 1. Interior Push-to-Exit Button (Debounced edge detection, non-blocking)
  static bool lastExitButton = HIGH;
  bool currentExitButton = digitalRead(EXIT_BUTTON_PIN);

  if (lastExitButton == HIGH && currentExitButton == LOW) {
    delay(20); // Debounce
    if (digitalRead(EXIT_BUTTON_PIN) == LOW) {
      Serial.println("🚪 [EXIT] Interior Exit Button Pressed!");
      soundExitButton();
      triggerSolenoid(3);
      logAccessEvent("EXIT", "INTERIOR_BUTTON", "Free exit");
    }
  }
  lastExitButton = currentExitButton;

  // 2. Member Fingerprint Scan
  if (digitalRead(TOUCH_PIN) == LOW) {
    handleFingerprintVerification();
  }

  // 3. Periodically check website for enrollment commands
  if (millis() - lastEnrollCheck > ENROLL_CHECK_INTERVAL) {
    lastEnrollCheck = millis();
    checkEnrollmentRequest();
  }

  delay(25);
}
