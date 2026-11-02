#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <Adafruit_Fingerprint.h>
#include <Adafruit_NeoPixel.h>
#include <time.h>
#include "secrets.h"
#include <driver/gpio.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

// ==========================================
// 1. PIN DEFINITIONS & CONSTANTS
// ==========================================
#define FIRMWARE_VERSION "v1.3.0"

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

// Addressable SK6812 RGB LED Ring (8 LEDs)
#define LED_PIN 18               // SK6812 LED Data In (GPIO 18)
#define NUM_LEDS 8               // 8 Addressable LEDs

// Sensor limits
const uint8_t MAX_SENSOR_CAPACITY = 127;

// Captive Portal AP Configuration
const char* AP_SSID = "FitnessBox-Scanner";

// ==========================================
// OBJECTS & GLOBALS
// ==========================================
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&Serial2);
Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);
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
enum LedState {
  LED_STATE_IDLE = 0,
  LED_STATE_GRANTED,
  LED_STATE_DENIED_EXPIRED,
  LED_STATE_DENIED_UNKNOWN,
  LED_STATE_ENROLL_YELLOW,
  LED_STATE_ENROLL_PURPLE,
  LED_STATE_ENROLL_SUCCESS,
  LED_STATE_OFF
};

volatile LedState currentLedState = LED_STATE_IDLE;
unsigned long ledStateStartedAt = 0;

void triggerLedAccessGranted();
void triggerLedAccessDeniedExpired();
void triggerLedAccessDeniedUnknown();
void triggerLedEnrollYellow();
void triggerLedEnrollPurple();
void triggerLedEnrollSuccess();
void triggerLedOff();
void triggerLedIdle();
void showEnrollSpinner(uint8_t headIdx, uint8_t r, uint8_t g, uint8_t b);
void updateLedSystem();
void performFactoryReset();
void sendRtdbHeartbeat();
void updateSolenoidState();
String extractJsonField(const String &json, const String &fieldName);
String getTodayDateString();
void logAccessEvent(String type, String memberId, String reason);
void applyAuthHeader(HTTPClient &https);
void runEnrollmentProcess();
void syncActiveMembersCache();
void checkOtaUpdate();
void sendRtdbPut(const String &subPath, const String &jsonPayload);
void processRtdbStream();

// ==========================================
// 2. AESTHETIC ACTIVE BUZZER AUDIO SUITE
// ==========================================
void activeBeep(int durationMs, int pauseMs = 35) {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(durationMs);
  digitalWrite(BUZZER_PIN, LOW);
  if (pauseMs > 0) delay(pauseMs);
}

// 1. Access Granted: One short beep (~150-200ms)
void soundAccessGranted() {
  activeBeep(160, 0);
}

// 2. Access Denied (Expired): Three rapid short beeps (~100ms each, ~100ms gap)
void soundAccessDeniedExpired() {
  activeBeep(90, 90);
  activeBeep(90, 90);
  activeBeep(90, 0);
}

// 3. Access Denied (Fingerprint Not Recognized): Crisp double beep
void soundAccessDeniedUnknown() {
  activeBeep(22, 55);
  activeBeep(22, 0);
}

// 4. Exit Button Pressed: Silent per spec
void soundExitButton() {
  // Silent - no feedback needed for exit
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
    currentLedState = LED_STATE_IDLE;
    Serial.println("🔒 [DOOR LOCK] Relocked.");
  }
}

// ==========================================
// 3. ADDRESSABLE RGB LED SUITE (8x SK6812 MINI-E)
// ==========================================
void setAllLeds(uint8_t r, uint8_t g, uint8_t b) {
  for (int i = 0; i < NUM_LEDS; i++) {
    strip.setPixelColor(i, strip.Color(r, g, b));
  }
  strip.show();
}

void setLedColor(uint8_t r, uint8_t g, uint8_t b) {
  setAllLeds(r, g, b);
}

// Convert 0-255 wheel position into smooth RGB rainbow color with adjustable brightness
uint32_t colorWheel(byte wheelPos, uint8_t brightness = 255) {
  wheelPos = 255 - wheelPos;
  uint8_t r = 0, g = 0, b = 0;
  if (wheelPos < 85) {
    r = 255 - wheelPos * 3;
    g = 0;
    b = wheelPos * 3;
  } else if (wheelPos < 170) {
    wheelPos -= 85;
    r = 0;
    g = wheelPos * 3;
    b = 255 - wheelPos * 3;
  } else {
    wheelPos -= 170;
    r = wheelPos * 3;
    g = 255 - wheelPos * 3;
    b = 0;
  }
  if (brightness < 255) {
    r = (r * brightness) / 255;
    g = (g * brightness) / 255;
    b = (b * brightness) / 255;
  }
  return strip.Color(r, g, b);
}

void triggerLedAccessGranted() {
  Serial.println("🟢 [LED] Instant Emerald Green (Access Granted)");
  currentLedState = LED_STATE_GRANTED;
  ledStateStartedAt = millis();
  setAllLeds(0, 255, 0); // INSTANT HARDWARE WRITE (<0.24ms)
}

void triggerLedAccessDeniedExpired() {
  Serial.println("🔴 [LED] Rapid Red Pulse (Expired Membership)");
  currentLedState = LED_STATE_DENIED_EXPIRED;
  ledStateStartedAt = millis();
  setAllLeds(255, 0, 0); // INSTANT HARDWARE WRITE (<0.24ms)
}

void triggerLedAccessDeniedUnknown() {
  Serial.println("🟠 [LED] Instant Steady Amber (Unknown Finger)");
  currentLedState = LED_STATE_DENIED_UNKNOWN;
  ledStateStartedAt = millis();
  setAllLeds(255, 100, 0); // INSTANT HARDWARE WRITE (<0.24ms)
}

void triggerLedEnrollYellow() {
  currentLedState = LED_STATE_ENROLL_YELLOW;
  showEnrollSpinner(0, 255, 180, 0);
}

void triggerLedEnrollPurple() {
  currentLedState = LED_STATE_ENROLL_PURPLE;
  showEnrollSpinner(0, 180, 0, 255);
}

void triggerLedEnrollSuccess() {
  Serial.println("🟣 [LED] Instant Purple Pulse (Enroll Success)");
  currentLedState = LED_STATE_ENROLL_SUCCESS;
  ledStateStartedAt = millis();
  setAllLeds(180, 0, 255); // INSTANT HARDWARE WRITE (<0.24ms)
}

void triggerLedOff() {
  currentLedState = LED_STATE_OFF;
  setAllLeds(0, 0, 0);
}

void triggerLedIdle() {
  currentLedState = LED_STATE_IDLE;
}

void showEnrollSpinner(uint8_t headIdx, uint8_t r, uint8_t g, uint8_t b) {
  strip.clear();
  headIdx = headIdx % NUM_LEDS;
  uint8_t tail1 = (headIdx + NUM_LEDS - 1) % NUM_LEDS;
  uint8_t tail2 = (headIdx + NUM_LEDS - 2) % NUM_LEDS;

  strip.setPixelColor(headIdx, strip.Color(r, g, b));             // Bright head
  strip.setPixelColor(tail1,   strip.Color(r / 3, g / 3, b / 3)); // Dim tail 1
  strip.setPixelColor(tail2,   strip.Color(r / 8, g / 8, b / 8)); // Faint tail 2
  strip.show();
}

// ==========================================
// AESTHETIC DUAL-ORBIT PLASMA AURA (THEMES)
// ==========================================
struct AuraPalette {
  uint8_t baseR, baseG, baseB;   // Ambient breathing base color
  uint8_t c1R,   c1G,   c1B;     // Primary orbital plasma comet
  uint8_t c2R,   c2G,   c2B;     // Harmonic counter-orbit comet
};

const AuraPalette AURA_THEMES[] = {
  // 0. Cyberpunk Neo-Tokyo: Electric Cyan comet & Neon Magenta counter-comet over Deep Violet
  { 18,  0,  35,    0, 240, 255,   255,   0, 150 },
  // 1. Stark Arc Reactor: Ice White/Blue comet & Electric Azure counter-comet over Deep Cobalt
  {  0, 10,  40,   80, 220, 255,   210, 245, 255 },
  // 2. Emerald Cyber-Matrix: Neon Mint comet & Electric Lime counter-comet over Oceanic Teal
  {  0, 22,  22,    0, 255, 120,   140, 255,   0 },
  // 3. Solar Flare: Blazing Gold comet & Molten Crimson counter-comet over Deep Amber
  { 30,  6,   0,  255, 180,   0,   255,  30,  10 }
};

const uint8_t NUM_AURA_THEMES = 4;

// ==========================================
// HIGH-PERFORMANCE NON-BLOCKING LED SYSTEM
// Runs on Core 1 at smooth 40 FPS with ZERO cross-core RMT collisions
// ==========================================
void updateLedSystem() {
  // 1. If door is currently unlocked, hold solid emerald green
  if (isSolenoidUnlocked) {
    // Solid green was set instantly by triggerLedAccessGranted(). Refresh at 10Hz to prevent bus flooding.
    static unsigned long lastUnlockedRefresh = 0;
    if (millis() - lastUnlockedRefresh >= 100) {
      lastUnlockedRefresh = millis();
      setAllLeds(0, 255, 0);
    }
    return;
  }

  // 2. State-driven animations (Non-blocking timing)
  if (currentLedState == LED_STATE_GRANTED) {
    unsigned long elapsed = millis() - ledStateStartedAt;
    if (elapsed < 600) {
      unsigned long cycle = elapsed % 200;
      if (cycle < 110) {
        setAllLeds(0, 255, 0);
      } else {
        setAllLeds(0, 0, 0);
      }
      return;
    } else {
      currentLedState = LED_STATE_IDLE;
    }
  }

  if (currentLedState == LED_STATE_DENIED_EXPIRED) {
    unsigned long elapsed = millis() - ledStateStartedAt;
    if (elapsed < 760) {
      unsigned long cycle = elapsed % 190;
      if (cycle < 100) {
        setAllLeds(255, 0, 0);
      } else {
        setAllLeds(0, 0, 0);
      }
      return;
    } else {
      currentLedState = LED_STATE_IDLE;
    }
  }

  if (currentLedState == LED_STATE_DENIED_UNKNOWN) {
    unsigned long elapsed = millis() - ledStateStartedAt;
    if (elapsed < 1200) {
      // Steady amber was already set instantly by triggerLedAccessDeniedUnknown()
      return;
    } else {
      currentLedState = LED_STATE_IDLE;
    }
  }

  if (currentLedState == LED_STATE_ENROLL_YELLOW) {
    static unsigned long lastYellowSpin = 0;
    static uint8_t yellowSpinIdx = 0;
    if (millis() - lastYellowSpin >= 60) {
      lastYellowSpin = millis();
      showEnrollSpinner(yellowSpinIdx++, 255, 180, 0);
    }
    return;
  }

  if (currentLedState == LED_STATE_ENROLL_PURPLE) {
    static unsigned long lastPurpleSpin = 0;
    static uint8_t purpleSpinIdx = 0;
    if (millis() - lastPurpleSpin >= 50) {
      lastPurpleSpin = millis();
      showEnrollSpinner(purpleSpinIdx++, 180, 0, 255);
    }
    return;
  }

  if (currentLedState == LED_STATE_ENROLL_SUCCESS) {
    unsigned long elapsed = millis() - ledStateStartedAt;
    if (elapsed < 600) {
      unsigned long cycle = elapsed % 200;
      if (cycle < 120) {
        setAllLeds(180, 0, 255);
      } else {
        setAllLeds(0, 0, 0);
      }
      return;
    } else {
      currentLedState = LED_STATE_IDLE;
    }
  }

  if (currentLedState == LED_STATE_OFF) {
    setAllLeds(0, 0, 0);
    return;
  }

  // 3. CONTINUOUS DUAL-ORBIT PLASMA AURA (40 FPS, ZERO FREEZES)
  static unsigned long lastAuraFrame = 0;
  if (millis() - lastAuraFrame < 25) return; // 40 FPS throttle
  lastAuraFrame = millis();

  static uint16_t subPos1 = 0;
  static uint16_t subPos2 = 0;
  static uint8_t breathAngle = 0;

  subPos1 = (subPos1 + 420) % (256 * 256);
  subPos2 = (subPos2 + (256 * 256) - 290) % (256 * 256);

  uint8_t pos1 = subPos1 >> 8;
  uint8_t pos2 = subPos2 >> 8;

  breathAngle += 3;
  uint8_t bTriangle = (breathAngle < 128) ? breathAngle : (255 - breathAngle);
  uint8_t breathVal = 10 + (bTriangle * 28 / 128);

  uint8_t currentTheme = (millis() / 12000) % NUM_AURA_THEMES;
  const AuraPalette &pal = AURA_THEMES[currentTheme];

  for (int i = 0; i < NUM_LEDS; i++) {
    int ledAngle = i * 32;

    int d1 = abs((int)pos1 - ledAngle);
    if (d1 > 128) d1 = 256 - d1;
    uint8_t w1 = (d1 < 44) ? ((44 - d1) * 255 / 44) : 0;

    int d2 = abs((int)pos2 - ledAngle);
    if (d2 > 128) d2 = 256 - d2;
    uint8_t w2 = (d2 < 44) ? ((44 - d2) * 255 / 44) : 0;

    uint8_t coreBoost1 = (d1 < 10) ? ((10 - d1) * 60 / 10) : 0;
    uint8_t coreBoost2 = (d2 < 10) ? ((10 - d2) * 40 / 10) : 0;

    int r = ((int)pal.baseR * breathVal) / 255 + ((int)pal.c1R * w1) / 255 + ((int)pal.c2R * w2) / 255 + coreBoost1 + coreBoost2;
    int g = ((int)pal.baseG * breathVal) / 255 + ((int)pal.c1G * w1) / 255 + ((int)pal.c2G * w2) / 255 + coreBoost1 + coreBoost2;
    int b = ((int)pal.baseB * breathVal) / 255 + ((int)pal.c1B * w1) / 255 + ((int)pal.c2B * w2) / 255 + coreBoost1 + coreBoost2;

    strip.setPixelColor(i, strip.Color(
      r > 255 ? 255 : r,
      g > 255 ? 255 : g,
      b > 255 ? 255 : b
    ));
  }
  strip.show();
}

// Visual Diagnostic & Hardware Showcase Test (Runs on Boot)
void runLedStartupTest() {
  Serial.println("\n✨ ========================================");
  Serial.println("✨ [LED TEST] Starting 8-LED Diagnostic & Visual Showcase...");
  Serial.println("✨ ========================================");

  strip.begin();
  strip.setBrightness(120); // 0-255 (Clean, bright, safe current)
  strip.clear();
  strip.show();

  // Test 1: Single LED sequential chase (Tests LED 0 to 7 individually)
  Serial.println("🧪 Phase 1: Individual Pixel Inspection (LED 1 to 8)...");
  for (int i = 0; i < NUM_LEDS; i++) {
    Serial.printf("   👉 Lighting LED #%d\n", i + 1);
    strip.clear();
    strip.setPixelColor(i, strip.Color(0, 220, 255)); // Bright Cyan
    strip.show();
    delay(130);
  }
  strip.clear();
  strip.show();
  delay(120);

  // Test 1b: Half-Split Hardware Diagnostic (LEDs 1-4 RED, LEDs 5-8 BLUE)
  Serial.println("🧪 Phase 1b: Half-Split Diagnostic (LEDs 1-4 RED, LEDs 5-8 BLUE)...");
  for (int i = 0; i < 4; i++) {
    strip.setPixelColor(i, strip.Color(255, 0, 0)); // LEDs 1-4 Red
  }
  for (int i = 4; i < NUM_LEDS; i++) {
    strip.setPixelColor(i, strip.Color(0, 80, 255)); // LEDs 5-8 Blue
  }
  strip.show();
  delay(1200);
  strip.clear();
  strip.show();
  delay(100);

  // Test 2: Color Spectrum Wipes across all 8 LEDs
  Serial.println("🧪 Phase 2: Color Spectrum Wipes (Red -> Green -> Blue -> Amber -> Purple -> White)...");
  uint32_t testColors[] = {
    strip.Color(255, 0, 0),     // Red
    strip.Color(0, 255, 0),     // Green
    strip.Color(0, 80, 255),    // Blue
    strip.Color(255, 140, 0),   // Amber
    strip.Color(180, 0, 255),   // Purple
    strip.Color(255, 255, 255)  // Cool White
  };

  for (int c = 0; c < 6; c++) {
    for (int i = 0; i < NUM_LEDS; i++) {
      strip.setPixelColor(i, testColors[c]);
      strip.show();
      delay(30);
    }
    delay(140);
  }

  // Test 3: Cyberpunk / Knight Rider Larson Scanner (Bouncing Comet back & forth)
  Serial.println("🧪 Phase 3: Knight Rider Cyberpunk Scanner Bounce...");
  for (int cycle = 0; cycle < 3; cycle++) {
    // Forward bounce
    for (int i = 0; i < NUM_LEDS; i++) {
      strip.clear();
      if (i > 0) strip.setPixelColor(i - 1, strip.Color(35, 0, 0)); // Tail
      strip.setPixelColor(i, strip.Color(255, 15, 15));             // Head
      if (i < NUM_LEDS - 1) strip.setPixelColor(i + 1, strip.Color(35, 0, 0));
      strip.show();
      delay(40);
    }
    // Backward bounce
    for (int i = NUM_LEDS - 1; i >= 0; i--) {
      strip.clear();
      if (i < NUM_LEDS - 1) strip.setPixelColor(i + 1, strip.Color(35, 0, 0));
      strip.setPixelColor(i, strip.Color(255, 15, 15));
      if (i > 0) strip.setPixelColor(i - 1, strip.Color(35, 0, 0));
      strip.show();
      delay(40);
    }
  }

  // Test 4: Smooth Rotating Rainbow Wheel (2 full revolutions)
  Serial.println("🧪 Phase 4: Rotating 360° Rainbow Wheel...");
  for (int j = 0; j < 256 * 2; j += 4) {
    for (int i = 0; i < NUM_LEDS; i++) {
      strip.setPixelColor(i, colorWheel(((i * 256 / NUM_LEDS) + j) & 255));
    }
    strip.show();
    delay(10);
  }

  // Test 5: Soft fade-in to Standby Dim Gradient
  Serial.println("✅ [LED TEST] All 8 LEDs verified operational! Idle Gradient armed.\n");
  for (int b = 10; b <= 45; b += 5) {
    for (int i = 0; i < NUM_LEDS; i++) {
      strip.setPixelColor(i, colorWheel(i * 32, b));
    }
    strip.show();
    delay(20);
  }
}

// ==========================================
// 4. SECURE JSON & TIME HELPERS
// ==========================================
String extractJsonField(const String &json, const String &fieldName) {
  int fieldIdx = json.indexOf("\"" + fieldName + "\"");
  if (fieldIdx == -1) return "";

  int colonIdx = json.indexOf(":", fieldIdx);
  if (colonIdx == -1) return "";

  // 1. Check for Firestore REST "stringValue": "..."
  int strValIdx = json.indexOf("\"stringValue\"", colonIdx);
  if (strValIdx != -1 && strValIdx < colonIdx + 45) {
    int valColon = json.indexOf(":", strValIdx);
    if (valColon != -1) {
      int q1 = json.indexOf("\"", valColon);
      if (q1 != -1) {
        int q2 = json.indexOf("\"", q1 + 1);
        if (q2 != -1) return json.substring(q1 + 1, q2);
      }
    }
  }

  // 2. Check for Firestore REST "integerValue": "..." (or unquoted number)
  int intValIdx = json.indexOf("\"integerValue\"", colonIdx);
  if (intValIdx != -1 && intValIdx < colonIdx + 45) {
    int valColon = json.indexOf(":", intValIdx);
    if (valColon != -1) {
      int q1 = json.indexOf("\"", valColon);
      if (q1 != -1 && q1 < valColon + 5) {
        int q2 = json.indexOf("\"", q1 + 1);
        if (q2 != -1) return json.substring(q1 + 1, q2);
      } else {
        int start = valColon + 1;
        while (start < json.length() && (json[start] == ' ' || json[start] == '\t')) start++;
        int end = start;
        while (end < json.length() && (json[end] >= '0' && json[end] <= '9')) end++;
        if (end > start) return json.substring(start, end);
      }
    }
  }

  // 3. Check for Firestore REST "booleanValue": true/false
  int boolValIdx = json.indexOf("\"booleanValue\"", colonIdx);
  if (boolValIdx != -1 && boolValIdx < colonIdx + 45) {
    int valColon = json.indexOf(":", boolValIdx);
    if (valColon != -1) {
      int start = valColon + 1;
      while (start < json.length() && (json[start] == ' ' || json[start] == '\t')) start++;
      if (json.startsWith("true", start)) return "true";
      if (json.startsWith("false", start)) return "false";
    }
  }

  // 4. Standard plain JSON string fallback: "fieldName": "value"
  int firstQuote = json.indexOf("\"", colonIdx);
  if (firstQuote != -1 && firstQuote < colonIdx + 20) {
    int endQuote = json.indexOf("\"", firstQuote + 1);
    if (endQuote != -1) {
      return json.substring(firstQuote + 1, endQuote);
    }
  }

  // 5. Plain JSON unquoted value fallback: "fieldName": true / false / 123
  int start = colonIdx + 1;
  while (start < json.length() && (json[start] == ' ' || json[start] == '\t')) start++;
  if (json.startsWith("true", start)) return "true";
  if (json.startsWith("false", start)) return "false";
  if (json.startsWith("null", start)) return "null";
  int end = start;
  while (end < json.length() && ((json[end] >= '0' && json[end] <= '9') || json[end] == '-')) end++;
  if (end > start) return json.substring(start, end);

  return "";
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
  } else {
    String errResp = https.getString();
    Serial.printf("❌ [CACHE SYNC ERROR] Firestore returned HTTP %d: %s\n", httpCode, errResp.c_str());
    if (httpCode == 403) {
      Serial.println("🔒 [ACTION NEEDED] Firestore Security Rules in Firebase Console are blocking member reads!");
      Serial.println("👉 In Firebase Console -> Firestore Database -> Rules, set: match /gyms/{gymId}/members/{memberId} { allow read: if true; }");
    }
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

          // Physically delete fingerprint template from R307S optical sensor flash to free up space!
          uint8_t err = finger.deleteModel(slot);
          if (err == FINGERPRINT_OK) {
            Serial.printf("🗑️ [HARDWARE SENSOR] Fingerprint template in Slot #%d deleted from R307S (Slot is now free!).\n", slot);
          } else {
            Serial.printf("⚠️ [HARDWARE SENSOR] deleteModel Slot #%d returned code: 0x%02X\n", slot, err);
          }
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
  https.setTimeout(2500);

  // Compute real Unix epoch timestamp (ms) and ISO string via NTP
  time_t now = time(nullptr);
  String timestampStr;
  String isoCreatedAt = "";

  if (now > 1700000000) { // NTP time valid (after Nov 2023)
    unsigned long long epochMs = ((unsigned long long)now) * 1000ULL;
    char msBuf[24];
    snprintf(msBuf, sizeof(msBuf), "%llu", epochMs);
    timestampStr = String(msBuf);

    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 200)) {
      char isoBuf[32];
      strftime(isoBuf, sizeof(isoBuf), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
      isoCreatedAt = String(isoBuf);
    }
  } else {
    // If NTP has not yet synchronized, mark 0 (never raw millis)
    timestampStr = "0";
  }

  String payload = "{\"fields\":{"
    "\"type\":{\"stringValue\":\"" + type + "\"},"
    "\"member_id\":{\"stringValue\":\"" + memberId + "\"},"
    "\"reason\":{\"stringValue\":\"" + reason + "\"},"
    "\"timestamp\":{\"stringValue\":\"" + timestampStr + "\"}" +
    (isoCreatedAt.length() > 0 ? ",\"created_at\":{\"stringValue\":\"" + isoCreatedAt + "\"}" : "") +
  "}}";
  
  https.POST(payload);
  https.end();
}

// ==========================================
// 6B. FIREBASE REALTIME DATABASE (RTDB) ZERO-READ SSE ENGINE
// Push-driven instant communication (<150ms) with $0 Firestore read cost
// ==========================================
WiFiClientSecure rtdbStreamClient;
bool isRtdbStreamActive = false;
unsigned long lastRtdbConnectAttempt = 0;

String getCleanRtdbHost() {
  String h = String(FIREBASE_RTDB_HOST);
  h.trim();
  if (h.startsWith("https://")) h = h.substring(8);
  if (h.startsWith("http://"))  h = h.substring(7);
  while (h.endsWith("/")) h = h.substring(0, h.length() - 1);
  return h;
}

void sendRtdbPut(const String &subPath, const String &jsonPayload) {
  if (WiFi.status() != WL_CONNECTED || linkedGymId == "") return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  String host = getCleanRtdbHost();
  String url = "https://" + host + "/gyms/" + linkedGymId + "/live/" + subPath + ".json";
  String token = getFirebaseAuthToken();
  if (token.length() > 0) {
    url += "?auth=" + token;
  }
  if (https.begin(client, url)) {
    https.addHeader("Content-Type", "application/json");
    https.PUT(jsonPayload);
    https.end();
  }
}

void sendRtdbHeartbeat() {
  if (WiFi.status() != WL_CONNECTED || linkedGymId == "") return;

  time_t now = time(nullptr);
  unsigned long long nowEpochMs = 0;
  if (now > 100000) {
    nowEpochMs = (unsigned long long)now * 1000ULL;
  } else {
    nowEpochMs = (unsigned long long)millis();
  }

  char timeBuf[32];
  snprintf(timeBuf, sizeof(timeBuf), "%llu", nowEpochMs);

  String payload = "{\"state\":\"online\",\"wifi_rssi\":" + String(WiFi.RSSI()) + ",\"version\":\"" + String(FIRMWARE_VERSION) + "\",\"last_seen\":" + String(timeBuf) + "}";
  sendRtdbPut("status", payload);
  Serial.printf("📡 [RTDB HEARTBEAT] Live presence updated (RSSI: %d dBm, Ver: %s)\n", WiFi.RSSI(), FIRMWARE_VERSION);
}

void handleRtdbEvent(const String &path, const String &dataJson) {
  // 1. Remote Unlock Command
  if (dataJson.indexOf("\"command\":\"UNLOCK\"") != -1 || dataJson.indexOf("\"action\":\"UNLOCK\"") != -1) {
    String st = extractJsonField(dataJson, "status");
    if (st == "PENDING" || st == "") {
      Serial.println("\n⚡ [RTDB REALTIME] Instant Remote Unlock command received from Web Dashboard!");
      triggerLedAccessGranted();
      triggerSolenoid(3);
      soundAccessGranted();
      logAccessEvent("REMOTE_UNLOCK", "ADMIN", "Dashboard Remote Open (RTDB)");
      sendRtdbPut("command/status", "\"COMPLETED\"");
    }
  }

  // 2. Dashboard PING (Instant Ping-Pong)
  if (dataJson.indexOf("\"command\":\"PING\"") != -1) {
    String st = extractJsonField(dataJson, "status");
    if (st == "PENDING") {
      Serial.println("\n📡 [RTDB REALTIME] Instant PING received! Replying PONG...");
      String pongPayload = "{\"command\":\"PING\",\"status\":\"PONG\",\"pong_timestamp\":\"" + String(millis()) + "\",\"wifi_rssi\":" + String(WiFi.RSSI()) + "}";
      sendRtdbPut("command", pongPayload);
    }
  }

  // 3. Biometric Enrollment Command
  if (dataJson.indexOf("\"command\":\"ENROLL\"") != -1) {
    String st = extractJsonField(dataJson, "status");
    if (st == "PENDING") {
      Serial.println("\n🌐 [RTDB REALTIME] Biometric Enrollment Command Received from Web Dashboard!");
      runEnrollmentProcess();
    }
  }

  // 4. Member Cache Sync Signal
  if (dataJson.indexOf("\"sync_needed\":true") != -1 || dataJson.indexOf("\"sync_needed\":\"true\"") != -1) {
    Serial.println("\n🔔 [RTDB REALTIME] Instant Member Sync Signal detected!");
    String slotStr = extractJsonField(dataJson, "slot_id");
    if (slotStr.length() > 0 && slotStr != "ALL") {
      int slot = slotStr.toInt();
      String status = extractJsonField(dataJson, "status");
      String expiry = extractJsonField(dataJson, "expiry_date");
      String name   = extractJsonField(dataJson, "name");

      if (status == "deleted") {
        localCache[slot].valid = false;
        prefs.begin("fb_cache", false);
        prefs.remove(("s_" + String(slot)).c_str());
        prefs.end();

        uint8_t err = finger.deleteModel(slot);
        if (err == FINGERPRINT_OK) {
          Serial.printf("🗑️ [HARDWARE SENSOR] Fingerprint template in Slot #%d deleted from R307S.\n", slot);
        }
        Serial.printf("🗑️ [CACHE] Removed Slot #%d from local cache.\n", slot);
      } else {
        saveMemberToLocalCache((uint8_t)slot, (status == "active"), expiry, name);
        Serial.printf("⚡ [CACHE] Targeted single-slot update: Slot #%d (%s, %s)\n", slot, name.c_str(), status.c_str());
      }
    } else {
      syncActiveMembersCache();
    }
    sendRtdbPut("sync/sync_needed", "false");
  }

  // 5. Remote OTA Check
  if (dataJson.indexOf("\"command\":\"CHECK_OTA\"") != -1) {
    String st = extractJsonField(dataJson, "status");
    if (st == "PENDING") {
      Serial.println("\n🚀 [RTDB REALTIME] Remote OTA check command received!");
      sendRtdbPut("command/status", "\"COMPLETED\"");
      checkOtaUpdate();
    }
  }

  // 6. Remote Factory Reset Command
  if (dataJson.indexOf("\"command\":\"RESET\"") != -1 || dataJson.indexOf("\"command\":\"FACTORY_RESET\"") != -1) {
    Serial.println("\n🔄 [RTDB REALTIME] Remote Factory Reset Command Received!");
    sendRtdbPut("command/status", "\"COMPLETED\"");
    performFactoryReset();
  }
}

void processRtdbStream() {
  if (WiFi.status() != WL_CONNECTED || linkedGymId == "") return;

  String host = getCleanRtdbHost();

  // Reconnection logic
  if (!rtdbStreamClient.connected()) {
    if (isRtdbStreamActive) {
      Serial.println("⚠️ [RTDB] Stream disconnected. Reconnecting...");
      isRtdbStreamActive = false;
    }
    if (millis() - lastRtdbConnectAttempt > 5000) {
      lastRtdbConnectAttempt = millis();
      Serial.printf("🌐 [RTDB] Connecting to Firebase Realtime Database SSE Stream (%s)...\n", host.c_str());
      rtdbStreamClient.stop();
      rtdbStreamClient.setInsecure();
      rtdbStreamClient.setTimeout(4000);
      if (rtdbStreamClient.connect(host.c_str(), 443)) {
        String path = "/gyms/" + linkedGymId + "/live.json";
        String token = getFirebaseAuthToken();
        if (token.length() > 0) {
          path += "?auth=" + token;
        }
        rtdbStreamClient.print(
          String("GET ") + path + " HTTP/1.1\r\n" +
          "Host: " + host + "\r\n" +
          "Accept: text/event-stream\r\n" +
          "Connection: keep-alive\r\n\r\n"
        );
        
        // Handshake verification
        unsigned long waitResp = millis();
        bool streamOk = false;
        while (millis() - waitResp < 3500) {
          if (rtdbStreamClient.available()) {
            String headerLine = rtdbStreamClient.readStringUntil('\n');
            headerLine.trim();
            if (headerLine.indexOf("200 OK") != -1 || headerLine.indexOf("200") != -1) {
              streamOk = true;
            }
            if (headerLine.length() == 0 && streamOk) {
              break;
            }
          }
          delay(10);
        }

        if (streamOk) {
          isRtdbStreamActive = true;
          Serial.println("⚡ [RTDB] Connected & Verified! Push-driven Realtime Stream is ACTIVE (0 Firestore reads).");
          sendRtdbHeartbeat();
        } else {
          Serial.println("❌ [RTDB] HTTP Handshake failed or not 200 OK. Falling back to periodic check.");
          rtdbStreamClient.stop();
          isRtdbStreamActive = false;
        }
      } else {
        Serial.println("⚠️ [RTDB] Connection to RTDB failed. Retrying in 5s...");
      }
    }
    return;
  }

  // Non-blocking stream read
  static String sseEvent = "";
  while (rtdbStreamClient.available()) {
    String line = rtdbStreamClient.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    if (line.startsWith("event:")) {
      sseEvent = line.substring(6);
      sseEvent.trim();
    } else if (line.startsWith("data:")) {
      String sseData = line.substring(5);
      sseData.trim();
      if (sseEvent == "put" || sseEvent == "patch") {
        String path = extractJsonField(sseData, "path");
        handleRtdbEvent(path, sseData);
      }
      sseEvent = "";
    }
  }
}

// Check for Remote Door Unlock commands from Dashboard (Fallback)
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
      triggerLedAccessGranted();
      triggerSolenoid(3);
      soundAccessGranted();
      logAccessEvent("REMOTE_UNLOCK", "ADMIN", "Dashboard Remote Open");

      // Mark command COMPLETED
      https.end();
      String patchUrl = url + "?updateMask.fieldPaths=status";
      if (https.begin(client, patchUrl)) {
        applyAuthHeader(https);
        String patchPayload = "{\"fields\":{\"status\":{\"stringValue\":\"COMPLETED\"}}}";
        https.PATCH(patchPayload);
      }
    } else if (cmd == "PING" && st == "PENDING") {
      Serial.println("\n📡 [PING] On-demand ping received from Web Dashboard! Replying PONG...");
      https.end();
      String patchUrl = url + "?updateMask.fieldPaths=status&updateMask.fieldPaths=pong_timestamp&updateMask.fieldPaths=wifi_rssi";
      if (https.begin(client, patchUrl)) {
        applyAuthHeader(https);
        String payload = "{\"fields\":{"
          "\"status\":{\"stringValue\":\"PONG\"},"
          "\"pong_timestamp\":{\"stringValue\":\"" + String(millis()) + "\"},"
          "\"wifi_rssi\":{\"integerValue\":\"" + String(WiFi.RSSI()) + "\"}"
        "}}";
        https.PATCH(payload);
      }
    } else if (cmd == "CHECK_OTA" && st == "PENDING") {
      Serial.println("\n🚀 [OTA] Remote OTA check command received from Cloud!");
      https.end();
      String patchUrl = url + "?updateMask.fieldPaths=status";
      if (https.begin(client, patchUrl)) {
        applyAuthHeader(https);
        String patchPayload = "{\"fields\":{\"status\":{\"stringValue\":\"COMPLETED\"}}}";
        https.PATCH(patchPayload);
      }
      checkOtaUpdate();
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
  https.setTimeout(2500);
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
  // Push status immediately to Firebase Realtime Database
  String rtdbPayload = "{\"command\":\"IDLE\",\"status\":\"" + status + "\",\"fingerprint_id\":\"" + String(fingerId) + "\",\"timestamp\":" + String(millis()) + "}";
  sendRtdbPut("enrollment", rtdbPayload);

  if (WiFi.status() != WL_CONNECTED || linkedGymId == "") return;
  
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  
  String url = "https://firestore.googleapis.com/v1/projects/" + String(FIREBASE_PROJECT_ID) + "/databases/(default)/documents/gyms/" + linkedGymId + "/scanners/enrollment?updateMask.fieldPaths=status&updateMask.fieldPaths=fingerprint_id";
  if (!https.begin(client, url)) return;
  
  applyAuthHeader(https);
  https.setTimeout(2500);
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


  setAllLeds(255, 90, 0); // Solid Amber: OTA Update in progress

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  https.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

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
      setAllLeds(0, 255, 0); // Solid Green before reboot
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
  soundEnrollPrompt();
  Serial.println("👉 [ENROLL] Place finger on scanner (Touch 1)...");
  
  finger.getTemplateCount();
  Serial.printf("📊 [ENROLL] Current sensor templates stored: %d / %d\n", finger.templateCount, MAX_SENSOR_CAPACITY);

  // Find first available free slot (1 to MAX_SENSOR_CAPACITY) to recycle deleted slots
  uint8_t nextSlot = 0;
  for (uint8_t i = 1; i <= MAX_SENSOR_CAPACITY; i++) {
    if (!localCache[i].valid) {
      if (finger.loadModel(i) != FINGERPRINT_OK) {
        nextSlot = i;
        break;
      }
    }
  }

  // Fallback: check any slot that doesn't have an active template on sensor
  if (nextSlot == 0) {
    for (uint8_t i = 1; i <= MAX_SENSOR_CAPACITY; i++) {
      if (finger.loadModel(i) != FINGERPRINT_OK) {
        nextSlot = i;
        break;
      }
    }
  }

  if (nextSlot == 0 || nextSlot > MAX_SENSOR_CAPACITY) {
    Serial.println("❌ [ENROLL] Sensor storage is full! No free slots available.");
    triggerLedAccessDeniedExpired();
    soundEnrollFailed();
    updateEnrollmentStatus("SENSOR_FULL", 0);
    return;
  }

  Serial.printf("🎯 [ENROLL] Assigned Slot #%d for new member registration.\n", nextSlot);

  // Touch 1: Yellow rotating spinner animation while scanning
  int p = -1;
  unsigned long timeout = millis() + 15000;
  triggerLedEnrollYellow();
  while (p != FINGERPRINT_OK && millis() < timeout) {
    p = finger.getImage();
    updateLedSystem();
    delay(20);
  }
  if (p != FINGERPRINT_OK || finger.image2Tz(1) != FINGERPRINT_OK) {
    triggerLedAccessDeniedExpired();
    soundEnrollFailed();
    updateEnrollmentStatus("ERROR", 0);
    return;
  }

  // Touch 1 captured: Purple rotating animation
  Serial.println("👉 [ENROLL] Touch 1 Scanned! Remove finger...");
  triggerLedEnrollPurple();
  soundEnrollStep1();
  delay(800);

  // Check if finger is ALREADY enrolled on this sensor to prevent duplicate slots
  if (finger.fingerFastSearch() == FINGERPRINT_OK) {
    uint8_t existingSlot = finger.fingerID;
    Serial.printf("⚠️ [ALREADY ENROLLED] Finger already registered in Slot #%d! Re-linking.\n", existingSlot);
    triggerLedEnrollSuccess();
    soundEnrollSuccess();
    saveMemberToLocalCache(existingSlot, true, "2099-12-31", "Member");
    updateEnrollmentStatus("SUCCESS", existingSlot);
    return;
  }
  
  p = 0;
  while (p != FINGERPRINT_NOFINGER) {
    p = finger.getImage();
    updateLedSystem();
    delay(20);
  }

  // Touch 2: Yellow rotating spinner while waiting for 2nd touch
  Serial.println("👉 [ENROLL] Place same finger again (Touch 2)...");
  p = -1;
  timeout = millis() + 15000;
  triggerLedEnrollYellow();
  while (p != FINGERPRINT_OK && millis() < timeout) {
    p = finger.getImage();
    delay(50);
  }
  if (p != FINGERPRINT_OK || finger.image2Tz(2) != FINGERPRINT_OK) {
    triggerLedAccessDeniedExpired();
    soundEnrollFailed();
    updateEnrollmentStatus("ERROR", 0);
    return;
  }

  if (finger.createModel() != FINGERPRINT_OK) {
    triggerLedAccessDeniedExpired();
    soundEnrollFailed();
    updateEnrollmentStatus("ERROR", 0);
    return;
  }

  if (finger.storeModel(nextSlot) == FINGERPRINT_OK) {
    Serial.printf("🎉 Enrolled into Slot #%d!\n", nextSlot);
    triggerLedEnrollSuccess(); // Purple completion pulse immediately
    soundEnrollSuccess();
    // Pre-cache immediately so member can scan right away!
    saveMemberToLocalCache(nextSlot, true, "2099-12-31", "New Member");
    updateEnrollmentStatus("SUCCESS", nextSlot);
  } else {
    triggerLedAccessDeniedExpired();
    soundEnrollFailed();
    updateEnrollmentStatus("ERROR", 0);
  }
}

// ==========================================
// 9. HIGH-SPEED MEMBER VERIFICATION (CACHE-FIRST)
// ==========================================
static unsigned long fingerCooldownUntil = 0;

void handleFingerprintVerification() {
  if (millis() < fingerCooldownUntil) return;

  Serial.println("\n👆 Finger detected. Reading...");
  delay(15);
  
  uint8_t p = FINGERPRINT_NOFINGER;
  for (int tries = 0; tries < 4; tries++) {
    p = finger.getImage();
    if (p == FINGERPRINT_OK) break;
    delay(30);
  }

  if (p != FINGERPRINT_OK || finger.image2Tz() != FINGERPRINT_OK) {
    triggerLedAccessDeniedUnknown(); // Instant Amber LED
    soundAccessDeniedUnknown();      // Crisp double beep
    fingerCooldownUntil = millis() + 1500;
    return;
  }

  if (finger.fingerFastSearch() != FINGERPRINT_OK) {
    Serial.println("❌ Access Denied: Unknown Fingerprint.");
    triggerLedAccessDeniedUnknown(); // Instant Amber LED
    soundAccessDeniedUnknown();      // Crisp double beep
    logAccessEvent("DENIED", "UNKNOWN", "Not recognized");
    fingerCooldownUntil = millis() + 1500;
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

    // 2. Access Denied: Inactive membership
    if (!active) {
      Serial.printf("🔴 ACCESS DENIED: Membership Inactive for %s\n", memberName.c_str());
      triggerLedAccessDeniedExpired();   // Rapid red pulse instantly
      soundAccessDeniedExpired();        // 3 rapid short beeps
      logAccessEvent("DENIED", String(slotId), "Membership inactive");
      fingerCooldownUntil = millis() + 1500;
      return;
    }

    // 2. Access Denied: Expired membership
    if (expiry.length() == 10 && todayDate.length() == 10 && expiry < todayDate) {
      Serial.printf("🔴 ACCESS DENIED: Membership Expired for %s\n", memberName.c_str());
      triggerLedAccessDeniedExpired();   // Rapid red pulse instantly
      soundAccessDeniedExpired();        // 3 rapid short beeps
      logAccessEvent("DENIED", String(slotId), "Membership expired");
      fingerCooldownUntil = millis() + 1500;
      return;
    }

    // 1. ACCESS GRANTED: Valid fingerprint + Active membership!
    Serial.printf("🟢 ACCESS GRANTED (INSTANT LOCAL)! Welcome, %s!\n", memberName.c_str());
    triggerLedAccessGranted();         // Turns solid emerald green IMMEDIATELY (<0.24ms)
    triggerSolenoid(3);                // Solenoid activates IMMEDIATELY
    soundAccessGranted();              // Buzzer beep sounds in sync (~160ms)
    logAccessEvent("GRANTED", String(slotId), "Active membership (Local)");
    fingerCooldownUntil = millis() + 3000;
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
      https.setTimeout(2500);
      String queryPayload = "{\"structuredQuery\":{\"from\":[{\"collectionId\":\"members\"}],\"where\":{\"fieldFilter\":{\"field\":{\"fieldPath\":\"fingerprint_id\"},\"op\":\"EQUAL\",\"value\":{\"stringValue\":\"" + String(slotId) + "\"}}},\"limit\":1}}";

      int httpCode = https.POST(queryPayload);
      String resp = "";
      if (httpCode == 200) {
        resp = https.getString();
        // Fallback: If not found as stringValue, check if stored as integerValue in Firestore
        if (resp.indexOf("\"document\"") == -1) {
          https.end();
          if (https.begin(client, url)) {
            applyAuthHeader(https);
            https.setTimeout(2500);
            String intQuery = "{\"structuredQuery\":{\"from\":[{\"collectionId\":\"members\"}],\"where\":{\"fieldFilter\":{\"field\":{\"fieldPath\":\"fingerprint_id\"},\"op\":\"EQUAL\",\"value\":{\"integerValue\":\"" + String(slotId) + "\"}}},\"limit\":1}}";
            int intCode = https.POST(intQuery);
            if (intCode == 200) {
              resp = https.getString();
            }
          }
        }

        if (resp.indexOf("\"document\"") != -1) {
          String name = extractJsonField(resp, "name");
          String status = extractJsonField(resp, "status");
          String expiry = extractJsonField(resp, "expiry_date");

          if (status == "active" && (expiry.length() < 10 || todayDate.length() < 10 || expiry >= todayDate)) {
            Serial.printf("🟢 ACCESS GRANTED (CLOUD)! Welcome, %s!\n", name.c_str());
            saveMemberToLocalCache(slotId, true, expiry, name);
            triggerLedAccessGranted();       // Turns solid emerald green IMMEDIATELY (<0.24ms)
            triggerSolenoid(3);              // Solenoid activates IMMEDIATELY
            soundAccessGranted();            // Buzzer beep sounds in sync (~160ms)
            logAccessEvent("GRANTED", String(slotId), "Active membership (Cloud)");
            https.end();
            return;
          } else {
            Serial.println("🔴 ACCESS DENIED: Expired or inactive in cloud.");
            saveMemberToLocalCache(slotId, false, expiry, name);
            triggerLedAccessDeniedExpired(); // Rapid red pulse instantly
            soundAccessDeniedExpired();      // 3 rapid short beeps
            logAccessEvent("DENIED", String(slotId), "Expired/Inactive");
            delay(400);
            https.end();
            return;
          }
        } else {
          Serial.printf("⚠️ Slot #%d not found in cloud members collection.\n", slotId);
        }
      } else {
        String errResp = https.getString();
        Serial.printf("❌ [CLOUD VERIFY ERROR] Firestore returned HTTP %d: %s\n", httpCode, errResp.c_str());
        if (httpCode == 403) {
          Serial.println("🔒 [ACTION NEEDED] Firestore Security Rules in Firebase Console are blocking member reads!");
          Serial.println("👉 Update rules in Firebase Console: match /gyms/{gymId}/members/{memberId} { allow read: if true; }");
        }
      }
      https.end();
    }
  }

  // FAIL CLOSED (3. Unknown / unverified)
  Serial.println("⚠️ Access Denied: Unverified record.");
  triggerLedAccessDeniedUnknown(); // Instant Amber LED
  soundAccessDeniedUnknown();
  logAccessEvent("DENIED", String(slotId), "Unverified");
  fingerCooldownUntil = millis() + 1500;
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

// ==========================================
// 10b. FACTORY RESET ENGINE
// ==========================================
void performFactoryReset() {
  Serial.println("\n⚠️ ========================================");
  Serial.println("⚠️ [FACTORY RESET] Erasing all credentials and data...");
  Serial.println("⚠️ ========================================");

  // Audio & Visual Warning Alert
  for (int i = 0; i < 3; i++) {
    setAllLeds(255, 0, 0); // Solid Red
    activeBeep(60, 40);
    setAllLeds(0, 0, 0);
    delay(40);
  }

  // 1. Wipe Wi-Fi & Linking Key from NVS
  prefs.begin("fitness_box", false);
  prefs.clear();
  prefs.end();
  Serial.println("🗑️ [RESET] Wi-Fi SSID, Password & Linking Key erased from NVS.");

  // 2. Wipe Local Member Cache
  prefs.begin("fb_cache", false);
  prefs.clear();
  prefs.end();
  for (int i = 0; i <= MAX_SENSOR_CAPACITY; i++) localCache[i].valid = false;
  Serial.println("🗑️ [RESET] Local offline member cache erased.");

  // 3. Wipe Fingerprint Sensor Hardware Templates
  finger.emptyDatabase();
  Serial.println("🗑️ [RESET] Fingerprint sensor templates wiped.");

  // 4. Confirmation melody & Blue LED (Setup AP Color)
  setAllLeds(0, 80, 255);
  activeBeep(50, 40);
  activeBeep(90, 50);
  activeBeep(200, 0);
  Serial.println("🔄 [RESET] Reset complete! Rebooting into Setup Portal...\n");
  delay(1000);

  ESP.restart();
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

  // Maximize GPIO 18 drive strength for pristine SK6812 NeoPixel waveform edges
  gpio_set_drive_capability((gpio_num_t)LED_PIN, GPIO_DRIVE_CAP_3);

  Serial.begin(9600);
  delay(100);
  Serial.println("\n\n========================================");
  Serial.printf("🏋️ FITNESS BOX - SMART BIOMETRIC GATE (%s)\n", FIRMWARE_VERSION);
  Serial.println("========================================");

  // --- 8-LED Hardware Diagnostic & Showcase Sequence ---
  runLedStartupTest();

  // Power-on Factory Reset Check: Hold exit button for 3 seconds while powering on
  if (digitalRead(EXIT_BUTTON_PIN) == LOW) {
    Serial.println("🔘 [BOOT] Exit button held at power-on. Checking for Factory Reset...");
    unsigned long bootCheck = millis();
    while (digitalRead(EXIT_BUTTON_PIN) == LOW && (millis() - bootCheck < 3000)) {
      activeBeep(30, 70);
    }
    if (millis() - bootCheck >= 3000) {
      Serial.println("🔘 [BOOT] 3-second hold confirmed! Triggering Factory Reset...");
      performFactoryReset();
    }
  }



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

  // 1. Interior Push-to-Exit Button (Debounced + 7-second Long-Press Factory Reset)
  static bool lastExitButton = HIGH;
  static unsigned long exitButtonPressStart = 0;
  static bool exitButtonActionTriggered = false;
  bool currentExitButton = digitalRead(EXIT_BUTTON_PIN);

  if (lastExitButton == HIGH && currentExitButton == LOW) {
    // Just pressed
    exitButtonPressStart = millis();
    exitButtonActionTriggered = false;
  } else if (currentExitButton == LOW) {
    // Currently being held down
    unsigned long holdDuration = millis() - exitButtonPressStart;
    if (holdDuration > 7000 && !exitButtonActionTriggered) {
      exitButtonActionTriggered = true;
      Serial.println("\n🔘 [HARDWARE BUTTON] 7-second long-press detected! Triggering Factory Reset...");
      performFactoryReset();
    }
  } else if (lastExitButton == LOW && currentExitButton == HIGH) {
    // Just released
    unsigned long holdDuration = millis() - exitButtonPressStart;
    if (holdDuration >= 50 && holdDuration < 4000 && !exitButtonActionTriggered) {
      Serial.println("🚪 [EXIT] Interior Exit Button Pressed!");
      triggerLedAccessGranted();
      triggerSolenoid(3);
      soundExitButton();
      logAccessEvent("EXIT", "INTERIOR_BUTTON", "Free exit");
    }
  }
  lastExitButton = currentExitButton;

  // 2. Member Fingerprint Scan (Cache-first <15ms)
  if (digitalRead(TOUCH_PIN) == LOW) {
    handleFingerprintVerification();
  }

  // 3. Firebase Realtime Database SSE Stream (Zero-read Push Events)
  processRtdbStream();

  // 3b. Live Presence Heartbeat to RTDB (Every 25 seconds for live status without page reload)
  static unsigned long lastHeartbeat = 0;
  if (isRtdbStreamActive && (millis() - lastHeartbeat > 25000)) {
    lastHeartbeat = millis();
    sendRtdbHeartbeat();
  }

  // If RTDB stream is active, periodic polling is DISABLED (0 Firestore reads!).
  // If RTDB stream is disconnected/reconnecting, fallback polling runs safely every 4s.
  static unsigned long lastCommandPoll = 0;
  static uint8_t pollStep = 0;
  if (!isRtdbStreamActive && (millis() - lastCommandPoll > 4000)) {
    lastCommandPoll = millis();
    pollStep = (pollStep + 1) % 3;
    if (pollStep == 0) checkRemoteUnlockCommand();
    else if (pollStep == 1) checkEnrollmentRequest();
    else if (pollStep == 2) checkMemberCacheSyncSignal();
  }

  // 4. Smooth 40 FPS Continuous LED Aura & Feedback Engine (Core 1)
  updateLedSystem();

  // 5. Infrequent OTA Check (Every 30 mins)
  if (millis() - lastOtaCheck > OTA_CHECK_INTERVAL) {
    lastOtaCheck = millis();
    checkOtaUpdate();
  }

  // 6. Serial Commands (Debugging & Sensor Maintenance)
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd == "RESET" || cmd == "FACTORY_RESET") {
      performFactoryReset();
    } else if (cmd == "CLEAR_SENSOR") {
      finger.emptyDatabase();
      prefs.begin("fb_cache", false);
      prefs.clear();
      prefs.end();
      for (int i = 0; i <= MAX_SENSOR_CAPACITY; i++) localCache[i].valid = false;
      Serial.println("🗑️ [RESET] All fingerprint templates on sensor & flash cache wiped clean.");
    } else if (cmd == "SYNC") {
      Serial.println("🔄 [MANUAL SYNC] Refreshing local member cache from cloud...");
      syncActiveMembersCache();
    } else if (cmd == "OTA") {
      Serial.println("🚀 [MANUAL OTA] Checking for pending cloud firmware updates...");
      checkOtaUpdate();
    }
  }

  delay(1);
}
