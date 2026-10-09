#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <Adafruit_Fingerprint.h>
#include "secrets.h"
#include <driver/gpio.h>

// ==========================================
// 1. PIN DEFINITIONS
// ==========================================
// Fingerprint Sensor (R307S)
#define RX_PIN 16         // ESP32 RX2 <- R307S TX (Yellow Wire)
#define TX_PIN 17         // ESP32 TX2 -> R307S RX (Green Wire)
#define TOUCH_PIN 4       // ESP32 GPIO4 <- R307S Touch Out (Blue Wire)
                          // (White Wire connects to ESP32 3.3V)

// Audio Feedback (Active Buzzer)
#define BUZZER_PIN 19            // Active Buzzer Positive (+) -> GPIO 19. Negative (-) -> GND

// Door Lock Relay (Solenoid)
#define RELAY_PIN 25             // ESP32 GPIO 25 (Pin D25) -> Relay IN pin
// Most relay modules are ACTIVE LOW or ACTIVE HIGH.
// Set to HIGH if your relay triggers ON with HIGH, or LOW if your relay triggers ON with LOW.
#define RELAY_ACTIVE_LEVEL HIGH
#define RELAY_INACTIVE_LEVEL (!RELAY_ACTIVE_LEVEL)

// Interior Push-to-Exit Button
#define EXIT_BUTTON_PIN 27       // ESP32 GPIO 27 (Pin D27) -> Switch Pin 1. Switch Pin 2 -> GND

// LED Ring (Future)
#define LED_PIN 18               // WS2812 / SK6812 LED Ring Data (GPIO 18)

// Captive Portal AP Name
const char* AP_SSID = "FitnessBox-Scanner";

// ==========================================
// OBJECTS & GLOBALS
// ==========================================
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&Serial2);
Preferences prefs;
WebServer server(80);
DNSServer dnsServer;

bool isSetupMode = false;
String storedSSID = "";
String storedPassword = "";
String storedLinkingKey = "";
String linkedGymId = "";

unsigned long lastEnrollCheck = 0;
const unsigned long ENROLL_CHECK_INTERVAL = 3000;

// ==========================================
// 2. ACTIVE BUZZER DRIVER (MAXIMUM HARDWARE VOLUME)
// Active buzzers have their own internal oscillator and need pure DC voltage (HIGH)
// at maximum current drive to scream at full volume (~85dB+).
// ==========================================

void activeBeep(int durationMs, int pauseMs = 35) {
  digitalWrite(BUZZER_PIN, HIGH); // Full DC rail power
  delay(durationMs);
  digitalWrite(BUZZER_PIN, LOW);
  if (pauseMs > 0) delay(pauseMs);
}

// Exact Betaflight Drone Active Buzzer Sequence
// Part 1: 3 Rising cadence chirps immediately on power-up (ESC boot)
void playDronePowerUpTones() {
  Serial.println("🔊 [ESC BOOT] 3 High-Volume active chirps...");
  activeBeep(65, 45);
  activeBeep(65, 45);
  activeBeep(150, 0);
}

// Part 2: 2 Confirmation chirps when Wi-Fi & Cloud connect (FC handshake / Armed)
void playDroneArmedTones() {
  Serial.println("🔊 [FC ARMED] 2 High-Volume confirmation chirps...");
  delay(120);
  activeBeep(85, 50);
  activeBeep(320, 0);
}

// Full chime helper
void playDroneStartupSound() {
  playDronePowerUpTones();
  delay(160);
  playDroneArmedTones();
}

// 1. Access Granted: Loud solid beep (~180ms)
void soundAccessGranted() {
  activeBeep(180, 0);
}

// 2. Access Denied (Expired): 3 rapid loud warning beeps
void soundAccessDeniedExpired() {
  for (int i = 0; i < 3; i++) {
    activeBeep(90, 80);
  }
}

// 3. Access Denied (Not Recognized): Silent
void soundAccessDeniedUnknown() {
  // Silent
}

// Enrollment Success Sound: 2 sharp confirmation chirps
void soundEnrollSuccess() {
  activeBeep(80, 50);
  activeBeep(250, 0);
}

// Setup Mode Notification: 2 quick alert beeps
void soundConfigMode() {
  activeBeep(110, 80);
  activeBeep(110, 0);
}

// Door Lock Solenoid Control
void triggerSolenoid(int durationSeconds) {
  Serial.printf("⚡ [DOOR LOCK] Unlocking door for %d seconds...\n", durationSeconds);
  digitalWrite(RELAY_PIN, RELAY_ACTIVE_LEVEL);
  delay(durationSeconds * 1000);
  digitalWrite(RELAY_PIN, RELAY_INACTIVE_LEVEL);
  Serial.println("🔒 [DOOR LOCK] Relocked.");
}

void triggerLedAccessGranted()      { Serial.println("💡 [LED RING] Green pulse"); }
void triggerLedAccessDeniedExpired()  { Serial.println("💡 [LED RING] Rapid red pulse"); }
void triggerLedAccessDeniedUnknown()  { Serial.println("💡 [LED RING] Amber steady"); }
void triggerLedEnrollScanning()       { Serial.println("💡 [LED RING] Yellow rotating"); }
void triggerLedEnrollSuccess()        { Serial.println("💡 [LED RING] Purple completion"); }

// ==========================================
// 3. SECURE JSON & CLOUD PARSER
// Robust against newlines, spaces, and Firestore formatting
// ==========================================
String extractJsonField(const String &json, const String &fieldName) {
  int fieldIdx = json.indexOf("\"" + fieldName + "\"");
  if (fieldIdx == -1) return "";
  
  int valIdx = json.indexOf("\"stringValue\": \"", fieldIdx);
  if (valIdx == -1) return "";
  
  int start = valIdx + 16;
  int end = json.indexOf("\"", start);
  if (end == -1) return "";
  
  return json.substring(start, end);
}

// ==========================================
// 4. FIREBASE CLOUD LINKING (TLS / HTTPS)
// ==========================================
String resolveGymIdFromLinkingKey(String key) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ Wi-Fi not connected!");
    return "";
  }
  
  WiFiClientSecure client;
  client.setInsecure(); // Secure TLS connection over port 443
  HTTPClient https;
  
  String url = "https://firestore.googleapis.com/v1/projects/" + String(FIREBASE_PROJECT_ID) + "/databases/(default)/documents/gym_links/" + key;
  Serial.println("\n📡 [1/2] Secure HTTPS Query: " + url);
  
  if (https.begin(client, url)) {
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
    https.addHeader("Content-Type", "application/json");
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

// Verify member on scanner
int verifyMemberInFirestore(uint8_t fingerId, String &memberName) {
  if (WiFi.status() != WL_CONNECTED || linkedGymId == "") return 0;
  
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  
  String url = "https://firestore.googleapis.com/v1/projects/" + String(FIREBASE_PROJECT_ID) + "/databases/(default)/documents/gyms/" + linkedGymId + ":runQuery";
  Serial.printf("\n📡 Checking cloud for Fingerprint #%d in Gym [%s]...\n", fingerId, linkedGymId.c_str());
  if (!https.begin(client, url)) return 0;
  
  https.addHeader("Content-Type", "application/json");
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
  int result = 0;
  
  if (httpCode == 200) {
    String resp = https.getString();
    if (resp.indexOf("\"document\"") != -1) {
      String name = extractJsonField(resp, "name");
      if (name.length() > 0) memberName = name;
      
      String status = extractJsonField(resp, "status");
      if (status == "active" || status == "") result = 1;
      else result = 2; // Expired
      
      Serial.printf("✅ Member Found: %s | Status: %s\n", memberName.c_str(), status.c_str());
    } else {
      Serial.println("ℹ️ No matching member record found for this fingerprint ID.");
    }
  } else {
    Serial.printf("❌ Firestore query failed. HTTP Code: %d\n", httpCode);
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
  
  https.addHeader("Content-Type", "application/json");
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
  
  https.addHeader("Content-Type", "application/json");
  String payload = "{\"fields\":{"
    "\"status\":{\"stringValue\":\"" + status + "\"},"
    "\"fingerprint_id\":{\"stringValue\":\"" + String(fingerId) + "\"}"
  "}}";
  
  https.PATCH(payload);
  https.end();
}

// ==========================================
// 5. ENROLLMENT SEQUENCE
// ==========================================
void runEnrollmentProcess() {
  triggerLedEnrollScanning();
  Serial.println("👉 [ENROLL] Place finger on scanner (Touch 1)...");
  
  uint8_t nextSlot = 1;
  finger.getTemplateCount();
  if (finger.templateCount > 0) {
    nextSlot = finger.templateCount + 1;
  }
  if (nextSlot > 127) nextSlot = 1;

  // Touch 1
  int p = -1;
  unsigned long timeout = millis() + 15000;
  while (p != FINGERPRINT_OK && millis() < timeout) {
    p = finger.getImage();
    delay(50);
  }
  if (p != FINGERPRINT_OK || finger.image2Tz(1) != FINGERPRINT_OK) {
    updateEnrollmentStatus("ERROR", 0);
    return;
  }
  
  Serial.println("👍 Image 1 captured! Remove finger...");
  activeBeep(80, 0);
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
    updateEnrollmentStatus("ERROR", 0);
    return;
  }

  if (finger.createModel() != FINGERPRINT_OK) {
    Serial.println("❌ Fingerprints did not match.");
    updateEnrollmentStatus("ERROR", 0);
    return;
  }

  if (finger.storeModel(nextSlot) == FINGERPRINT_OK) {
    Serial.printf("🎉 Enrolled into Slot #%d!\n", nextSlot);
    soundEnrollSuccess();
    triggerLedEnrollSuccess();
    updateEnrollmentStatus("SUCCESS", nextSlot);
  } else {
    updateEnrollmentStatus("ERROR", 0);
  }
}

// ==========================================
// 6. MEMBER VERIFICATION
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
    Serial.println("⚠️ Finger found in sensor but no cloud record.");
    triggerLedAccessDeniedUnknown();
    soundAccessDeniedUnknown();
    delay(1500);
  }
}

// ==========================================
// 7. CAPTIVE PORTAL SETUP SERVER
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
  html += "<p>Biometric Scanner Setup</p>";
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
    delay(2000); // Give phone browser time to receive confirmation page

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
  soundConfigMode();
  Serial.println("\n==========================================");
  Serial.println("📡 ENTERING WI-FI SETUP & CAPTIVE PORTAL");
  Serial.println("==========================================");
  
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false, false); // Safe disconnect without zeroing config
  delay(100);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID);
  
  IPAddress apIP(192, 168, 4, 1);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  
  Serial.println("Broadcasting Wi-Fi: " + String(AP_SSID));
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
  Serial.println("✅ Captive Portal Server Started. Waiting for gym owner...");
}

// ==========================================
// 8. SETUP & LOOP
// ==========================================
void setup() {
  Serial.begin(9600);
  delay(1000);
  Serial.println("\n\n========================================");
  Serial.println("🏋️ FITNESS BOX - SMART BIOMETRIC SYSTEM");
  Serial.println("========================================");

  // Configure Pins
  pinMode(BUZZER_PIN, OUTPUT);
  gpio_set_drive_capability((gpio_num_t)BUZZER_PIN, GPIO_DRIVE_CAP_3); // Max hardware drive strength (~40mA)
  digitalWrite(BUZZER_PIN, LOW);
  pinMode(TOUCH_PIN, INPUT_PULLUP);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_INACTIVE_LEVEL); // Ensure door starts locked
  pinMode(EXIT_BUTTON_PIN, INPUT_PULLUP);

  // Prevent ESP-IDF from auto-connecting with stale internal NVS settings
  WiFi.persistent(false);

  // 1. Instantly play the 3 rising ESC initialization tones on power-on!
  playDronePowerUpTones();

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
    
    // 2. Play the 2 final confirmation tones now that we're connected & armed!
    playDroneArmedTones();
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
    return;
  }

  // 1. Check Exit Button (Debounced)
  if (digitalRead(EXIT_BUTTON_PIN) == LOW) {
    delay(50); // Software debounce
    if (digitalRead(EXIT_BUTTON_PIN) == LOW) {
      Serial.println("🚪 [EXIT] Interior Exit Button Pressed!");
      triggerSolenoid(3);
      logAccessEvent("EXIT", "INTERIOR_BUTTON", "Free exit");
      // Wait until button is released
      while (digitalRead(EXIT_BUTTON_PIN) == LOW) {
        delay(50);
      }
    }
  }

  // 2. Check Member Fingerprint Scan
  if (digitalRead(TOUCH_PIN) == LOW) {
    handleFingerprintVerification();
  }

  // 3. Periodically check website for enrollment commands
  if (millis() - lastEnrollCheck > ENROLL_CHECK_INTERVAL) {
    lastEnrollCheck = millis();
    checkEnrollmentRequest();
  }

  delay(30);
}
