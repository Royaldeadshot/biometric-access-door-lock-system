#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <Adafruit_Fingerprint.h>

// ==========================================
// 1. PIN DEFINITIONS
// ==========================================
// Fingerprint Sensor (R307S)
#define RX_PIN 16         // ESP32 RX2 <- R307S TX (Yellow Wire)
#define TX_PIN 17         // ESP32 TX2 -> R307S RX (Green Wire)
#define TOUCH_PIN 4       // ESP32 GPIO4 <- R307S Touch Out (Blue Wire)
                          // (White Wire connects to ESP32 3.3V)

// Audio & Hardware Feedback
#define BUZZER_PIN 19     // Buzzer Positive (+) -> GPIO 19 (Pin 31). Negative (-) -> GND

// Future Hardware Placeholders
#define RELAY_PIN 25      // Solenoid Door Lock Relay Signal (Future)
#define LED_PIN 18        // WS2812 / SK6812 LED Ring Data (Future)
#define EXIT_BUTTON_PIN 27// Interior Push-to-Exit Button (Future, active low)

// ==========================================
// 2. SAAS CLOUD CONFIGURATION
// ==========================================
const char* FIREBASE_PROJECT_ID = "fitnessboxgym-23c2e";
const char* FIREBASE_API_KEY = "AIzaSyAAtoSnk4d3NxuiWUbqJ3qY5F_LohmIa0w";

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
// 3. SOUND & AUDIO FEEDBACK
// ==========================================
void playDroneStartupSound() {
  Serial.println("?? Playing Drone FC startup chime...");
  int startupNotes[] = { 1046, 1318, 1568, 0, 1046, 2093 };
  int durations[]    = {  100,  100,  160, 80,  120,  280 };
  
  for (int i = 0; i < 6; i++) {
    if (startupNotes[i] > 0) {
      tone(BUZZER_PIN, startupNotes[i], durations[i]);
    }
    delay(durations[i] + 30);
  }
  noTone(BUZZER_PIN);
}

void soundAccessGranted() {
  tone(BUZZER_PIN, 2000, 180);
  delay(180);
  noTone(BUZZER_PIN);
}

void soundAccessDeniedExpired() {
  for (int i = 0; i < 3; i++) {
    tone(BUZZER_PIN, 900, 100);
    delay(100);
    noTone(BUZZER_PIN);
    if (i < 2) delay(100);
  }
}

void soundAccessDeniedUnknown() {
  // Silent
}

void soundEnrollSuccess() {
  tone(BUZZER_PIN, 1500, 100);
  delay(120);
  tone(BUZZER_PIN, 2200, 200);
  delay(200);
  noTone(BUZZER_PIN);
}

void soundConfigMode() {
  tone(BUZZER_PIN, 1200, 150);
  delay(200);
  tone(BUZZER_PIN, 1200, 150);
  delay(200);
  noTone(BUZZER_PIN);
}

// Hardware Placeholders
void triggerSolenoid(int durationSeconds) {
  Serial.printf("? [DOOR LOCK] Solenoid active for %d seconds\n", durationSeconds);
  digitalWrite(RELAY_PIN, HIGH);
  delay(durationSeconds * 1000);
  digitalWrite(RELAY_PIN, LOW);
  Serial.println("?? [DOOR LOCK] Locked.");
}

void triggerLedAccessGranted()      { Serial.println("?? [LED RING] Green pulse"); }
void triggerLedAccessDeniedExpired()  { Serial.println("?? [LED RING] Rapid red pulse"); }
void triggerLedAccessDeniedUnknown()  { Serial.println("?? [LED RING] Amber steady"); }
void triggerLedEnrollScanning()       { Serial.println("?? [LED RING] Yellow rotating"); }
void triggerLedEnrollSuccess()        { Serial.println("?? [LED RING] Purple completion"); }

// ==========================================
// 4. FIREBASE CLOUD LINKING & REST API
// ==========================================

// Lookup Gym ID using the 6-digit Linking Key from the web dashboard
String resolveGymIdFromLinkingKey(String key) {
  if (WiFi.status() != WL_CONNECTED) return "";
  
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  
  // 1. Direct document lookup in gym_links collection
  String url = "https://firestore.googleapis.com/v1/projects/" + String(FIREBASE_PROJECT_ID) + "/databases/(default)/documents/gym_links/" + key;
  if (https.begin(client, url)) {
    int httpCode = https.GET();
    if (httpCode == 200) {
      String resp = https.getString();
      int gymIdx = resp.indexOf("\"gym_id\": {\"stringValue\": \"");
      if (gymIdx != -1) {
        int endIdx = resp.indexOf("\"", gymIdx + 26);
        String foundGym = resp.substring(gymIdx + 26, endIdx);
        https.end();
        return foundGym;
      }
    }
    https.end();
  }
  
  // 2. Query fallback: search gyms where hardware_linking_key == key
  url = "https://firestore.googleapis.com/v1/projects/" + String(FIREBASE_PROJECT_ID) + "/databases/(default)/documents:runQuery";
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
    if (httpCode == 200) {
      String resp = https.getString();
      int nameIdx = resp.indexOf("/gyms/");
      if (nameIdx != -1) {
        int endIdx = resp.indexOf("\"", nameIdx + 6);
        String foundGym = resp.substring(nameIdx + 6, endIdx);
        https.end();
        return foundGym;
      }
    }
    https.end();
  }
  
  return "";
}

// Verify member on scanner
// Returns 1 = Active, 2 = Expired, 0 = Not Found
int verifyMemberInFirestore(uint8_t fingerId, String &memberName) {
  if (WiFi.status() != WL_CONNECTED || linkedGymId == "") return 0;
  
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  
  String url = "https://firestore.googleapis.com/v1/projects/" + String(FIREBASE_PROJECT_ID) + "/databases/(default)/documents:runQuery";
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
      int nameIdx = resp.indexOf("\"name\": {\"stringValue\": \"");
      if (nameIdx != -1) {
        int endName = resp.indexOf("\"", nameIdx + 24);
        memberName = resp.substring(nameIdx + 24, endName);
      }
      
      int statusIdx = resp.indexOf("\"status\": {\"stringValue\": \"");
      String status = "active";
      if (statusIdx != -1) {
        int endStatus = resp.indexOf("\"", statusIdx + 26);
        status = resp.substring(statusIdx + 26, endStatus);
      }
      
      if (status == "active") result = 1;
      else result = 2;
    }
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
    if (resp.indexOf("\"command\": {\"stringValue\": \"ENROLL\"}") != -1 &&
        resp.indexOf("\"status\": {\"stringValue\": \"PENDING\"}") != -1) {
      Serial.println("\n?? ENROLLMENT COMMAND RECEIVED FROM WEBSITE!");
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
  Serial.println("?? [ENROLL] Place finger on scanner (Touch 1)...");
  
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
  if (p != FINGERPRINT_OK) {
    updateEnrollmentStatus("ERROR", 0);
    return;
  }

  if (finger.image2Tz(1) != FINGERPRINT_OK) {
    updateEnrollmentStatus("ERROR", 0);
    return;
  }
  
  Serial.println("?? Image 1 captured! Remove finger...");
  delay(1000);
  p = 0;
  while (p != FINGERPRINT_NOFINGER) {
    p = finger.getImage();
    delay(50);
  }

  // Touch 2
  Serial.println("?? Place the SAME finger again (Touch 2)...");
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
    Serial.println("? Fingerprints did not match.");
    updateEnrollmentStatus("ERROR", 0);
    return;
  }

  if (finger.storeModel(nextSlot) == FINGERPRINT_OK) {
    Serial.printf("?? Enrolled into Slot #%d!\n", nextSlot);
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
  Serial.println("\n?? Finger detected. Reading...");
  delay(250);
  
  uint8_t p = FINGERPRINT_NOFINGER;
  for (int tries = 0; tries < 4; tries++) {
    p = finger.getImage();
    if (p == FINGERPRINT_OK) break;
    delay(100);
  }

  if (p != FINGERPRINT_OK || finger.image2Tz() != FINGERPRINT_OK) {
    Serial.println("?? Could not read fingerprint.");
    delay(500);
    return;
  }

  if (finger.fingerFastSearch() != FINGERPRINT_OK) {
    Serial.println("? Access Denied: Unknown Fingerprint.");
    triggerLedAccessDeniedUnknown();
    soundAccessDeniedUnknown();
    logAccessEvent("DENIED", "UNKNOWN", "Not recognized");
    delay(1500);
    return;
  }

  Serial.printf("?? Recognized Slot #%d!\n", finger.fingerID);
  String memberName = "Member";
  int memberStatus = verifyMemberInFirestore(finger.fingerID, memberName);
  
  if (memberStatus == 1) {
    Serial.printf("?? ACCESS GRANTED! Welcome, %s!\n", memberName.c_str());
    soundAccessGranted();
    triggerLedAccessGranted();
    logAccessEvent("GRANTED", String(finger.fingerID), "Active membership");
    triggerSolenoid(3);
  } else if (memberStatus == 2) {
    Serial.printf("?? ACCESS DENIED: Membership Expired for %s.\n", memberName.c_str());
    soundAccessDeniedExpired();
    triggerLedAccessDeniedExpired();
    logAccessEvent("DENIED", String(finger.fingerID), "Membership expired");
    delay(1500);
  } else {
    Serial.println("?? Finger found in sensor but no cloud record.");
    triggerLedAccessDeniedUnknown();
    soundAccessDeniedUnknown();
    delay(1500);
  }
}

// ==========================================
// 7. CAPTIVE PORTAL & WEB SETUP SERVER
// ==========================================
String generatePortalHtml() {
  // Scan for nearby Wi-Fi networks
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
  html += "body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif; background: #0f172a; color: #f8fafc; margin: 0; padding: 20px; display: flex; justify-content: center; align-items: center; min-height: 90vh; }";
  html += ".card { background: #1e293b; padding: 28px; border-radius: 20px; width: 100%; max-width: 380px; box-shadow: 0 20px 25px -5px rgba(0,0,0,0.5); border: 1px solid #334155; }";
  html += "h1 { font-size: 24px; font-weight: 800; color: #38bdf8; margin: 0 0 6px 0; display: flex; align-items: center; gap: 8px; }";
  html += "p { font-size: 14px; color: #94a3b8; margin: 0 0 24px 0; line-height: 1.5; }";
  html += "label { font-size: 12px; font-weight: 600; text-transform: uppercase; letter-spacing: 0.05em; color: #cbd5e1; display: block; margin-bottom: 6px; }";
  html += "input, select { width: 100%; box-sizing: border-box; padding: 12px 14px; border-radius: 10px; border: 1px solid #475569; background: #0f172a; color: #fff; font-size: 15px; margin-bottom: 18px; outline: none; }";
  html += "input:focus, select:focus { border-color: #38bdf8; box-shadow: 0 0 0 2px rgba(56,189,248,0.2); }";
  html += ".key-input { font-family: monospace; letter-spacing: 4px; font-size: 20px; text-align: center; font-weight: 700; color: #38bdf8; }";
  html += ".hint { font-size: 11px; color: #64748b; margin-top: -12px; margin-bottom: 18px; }";
  html += "button { width: 100%; padding: 14px; background: #2563eb; color: #fff; border: none; border-radius: 12px; font-size: 16px; font-weight: 700; cursor: pointer; transition: background 0.2s; }";
  html += "button:hover { background: #1d4ed8; }";
  html += "</style></head><body>";
  html += "<div class='card'>";
  html += "<h1>??? Fitness Box</h1>";
  html += "<p>Biometric Scanner Wi-Fi & SaaS Linking Setup</p>";
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

  Serial.println("\n?? Received Wi-Fi Setup Form:");
  Serial.println("SSID: " + newSSID);
  Serial.println("Key: " + newKey);

  // Send connecting page to user's phone
  String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<style>body{font-family:sans-serif;background:#0f172a;color:#fff;display:flex;justify-content:center;align-items:center;min-height:90vh;text-align:center;padding:20px;}.card{background:#1e293b;padding:30px;border-radius:20px;max-width:360px;}h2{color:#38bdf8;}</style></head><body>";
  html += "<div class='card'><h2>Connecting...</h2><p>Attempting to connect to <b>" + newSSID + "</b> and linking with key <b>" + newKey + "</b>...</p><p>Please watch your ESP32 Serial Monitor or wait 10 seconds.</p></div></body></html>";
  server.send(200, "text/html", html);
  delay(1000);

  // Attempt connection
  WiFi.disconnect();
  WiFi.begin(newSSID.c_str(), newPass.c_str());
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 25) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n? Connected to Wi-Fi! IP: " + WiFi.localIP().toString());
    
    // Resolve Gym ID
    String foundGymId = resolveGymIdFromLinkingKey(newKey);
    if (foundGymId != "") {
      Serial.println("?? Successfully linked to Gym: " + foundGymId);
      
      // Save permanently to NVS flash
      prefs.begin("fitness_box", false);
      prefs.putString("ssid", newSSID);
      prefs.putString("pass", newPass);
      prefs.putString("key", newKey);
      prefs.putString("gym_id", foundGymId);
      prefs.end();

      playDroneStartupSound();
      delay(2000);
      ESP.restart(); // Restart into normal mode
    } else {
      Serial.println("? Invalid Linking Key! Could not find gym in database.");
    }
  } else {
    Serial.println("? Failed to connect to Wi-Fi. Check password.");
  }
}

void startCaptivePortal() {
  isSetupMode = true;
  soundConfigMode();
  Serial.println("\n==========================================");
  Serial.println("?? ENTERING WI-FI SETUP & CAPTIVE PORTAL");
  Serial.println("==========================================");
  
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID);
  
  IPAddress apIP(192, 168, 4, 1);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  
  Serial.println("Broadcasting Wi-Fi: " + String(AP_SSID));
  Serial.println("Portal IP: http://192.168.4.1");
  
  // Start DNS Server for Captive Portal (redirects all requests)
  dnsServer.start(53, "*", apIP);
  
  // Web Server Routes
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", generatePortalHtml());
  });
  
  server.on("/save", HTTP_POST, handleSaveConfig);
  
  // Captive Portal probes redirect to home page
  server.on("/generate_204", []() { server.send(200, "text/html", generatePortalHtml()); });
  server.on("/gen_204", []() { server.send(200, "text/html", generatePortalHtml()); });
  server.on("/hotspot-detect.html", []() { server.send(200, "text/html", generatePortalHtml()); });
  server.on("/ncsi.txt", []() { server.send(200, "text/plain", "Microsoft NCSI"); });
  server.onNotFound([]() {
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
  });
  
  server.begin();
  Serial.println("? Captive Portal Server Started. Waiting for gym owner...");
}

// ==========================================
// 8. SETUP & LOOP
// ==========================================
void setup() {
  Serial.begin(9600);
  delay(1000);
  Serial.println("\n\n========================================");
  Serial.println("??? FITNESS BOX - SMART BIOMETRIC SYSTEM");
  Serial.println("========================================");

  // Configure Pins
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(TOUCH_PIN, INPUT_PULLUP);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  pinMode(EXIT_BUTTON_PIN, INPUT_PULLUP);

  // Initialize Fingerprint Sensor
  Serial2.begin(57600, SERIAL_8N1, RX_PIN, TX_PIN);
  finger.begin(57600);
  delay(50);

  if (finger.verifyPassword()) {
    Serial.println("? Fingerprint Sensor (R307S): Ready!");
  } else {
    Serial.println("? ERROR: Could not find Fingerprint sensor.");
  }

  // Load Saved Settings from Flash memory
  prefs.begin("fitness_box", true);
  storedSSID = prefs.getString("ssid", "");
  storedPassword = prefs.getString("pass", "");
  storedLinkingKey = prefs.getString("key", "");
  linkedGymId = prefs.getString("gym_id", "");
  prefs.end();

  // Check if user is holding touch sensor on boot to force Wi-Fi reset
  if (digitalRead(TOUCH_PIN) == LOW) {
    Serial.println("?? Finger held on boot! Wiping Wi-Fi settings...");
    prefs.begin("fitness_box", false);
    prefs.clear();
    prefs.end();
    delay(1000);
    startCaptivePortal();
    return;
  }

  // If no Wi-Fi credentials saved, enter Setup Portal immediately
  if (storedSSID == "" || storedLinkingKey == "") {
    Serial.println("?? No saved Wi-Fi or Linking Key found. Launching Setup Portal...");
    startCaptivePortal();
    return;
  }

  // Try connecting to saved Wi-Fi
  Serial.printf("?? Connecting to saved Wi-Fi: %s", storedSSID.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(storedSSID.c_str(), storedPassword.c_str());
  
  int timeout = 0;
  while (WiFi.status() != WL_CONNECTED && timeout < 25) {
    delay(500);
    Serial.print(".");
    timeout++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n? Wi-Fi Connected! IP: " + WiFi.localIP().toString());
    Serial.println("?? Linked Gym ID: " + linkedGymId);
    playDroneStartupSound();
  } else {
    Serial.println("\n?? Failed to connect to saved Wi-Fi. Launching Setup Portal fallback...");
    startCaptivePortal();
  }
}

void loop() {
  // If in Captive Portal Setup Mode
  if (isSetupMode) {
    dnsServer.processNextRequest();
    server.handleClient();
    return; // Don't run regular scanner logic in setup mode
  }

  // Normal Operating Mode:
  // 1. Check Exit Button
  if (digitalRead(EXIT_BUTTON_PIN) == LOW) {
    Serial.println("?? [EXIT] Interior Exit Button Pressed!");
    triggerSolenoid(3);
    logAccessEvent("EXIT", "INTERIOR_BUTTON", "Free exit");
    delay(500);
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
