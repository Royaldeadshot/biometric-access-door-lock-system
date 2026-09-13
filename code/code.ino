/*
  Gym Biometric Door Lock — Basic Firmware
  ------------------------------------------
  Hardware (matches schematic):
    - ESP32-WROOM-32
    - R307S fingerprint scanner  -> TX = IO27, RX = IO28 (crossed to ESP32 RX2/TX2)
    - SK6812 LED ring (8x)       -> DIN on IO4
    - Push-to-exit button        -> IO26 (SW1)
    - Buzzer                     -> IO19 (BZ1)
    - 12V Relay (lock control)   -> IN1 on IO10
    - 12V Solenoid door lock, via relay NO contact, flyback diode (D9) across it

  Libraries needed (install via Library Manager):
    - Adafruit Fingerprint Sensor Library
    - FastLED  (for SK6812)
    - Firebase ESP32 Client (mobizt) -- for the membership check

  This is a STARTING POINT, not production-ready:
    - Wi-Fi + Firebase credentials must be filled in
    - Error handling, offline caching, and enrollment flow are simplified
    - Test on a bench setup before wiring to a real door
*/

#include <Adafruit_Fingerprint.h>
#include <FastLED.h>
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

// ---------- Pin definitions (from schematic) ----------
#define FINGER_RX_PIN   27   // ESP32 IO27 <- Sensor TX
#define FINGER_TX_PIN   28   // ESP32 IO28 -> Sensor RX
#define LED_RING_PIN    4    // SK6812 DIN
#define NUM_LEDS        8
#define EXIT_BUTTON_PIN 26   // SW1, push-to-exit
#define BUZZER_PIN      19   // BZ1
#define RELAY_PIN       10   // 12V Relay IN1

// ---------- Wi-Fi / Firebase credentials (fill these in) ----------
#define WIFI_SSID        "YOUR_WIFI_SSID"
#define WIFI_PASSWORD    "YOUR_WIFI_PASSWORD"
#define FIREBASE_HOST    "your-project-id-default-rtdb.firebaseio.com"
#define FIREBASE_AUTH    "YOUR_FIREBASE_DATABASE_SECRET_OR_TOKEN"

// ---------- Objects ----------
HardwareSerial fingerSerial(2);  // UART2
Adafruit_Fingerprint finger(&fingerSerial);
CRGB leds[NUM_LEDS];

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// ---------- Timing / behavior constants ----------
const unsigned long UNLOCK_DURATION_MS = 3000;  // how long the lock stays open
const unsigned long SCAN_COOLDOWN_MS   = 1500;  // debounce between scans

unsigned long lastScanTime = 0;

// ---------- LED colors ----------
CRGB COLOR_IDLE    = CRGB(0, 0, 40);      // dim blue - standby
CRGB COLOR_SCANNING = CRGB(0, 0, 255);    // blue - actively scanning
CRGB COLOR_GRANTED = CRGB(0, 255, 0);     // green - access granted
CRGB COLOR_DENIED  = CRGB(255, 0, 0);     // red - denied / not recognized
CRGB COLOR_EXPIRED = CRGB(255, 120, 0);   // amber - membership expired

// ============================================================
void setup() {
  Serial.begin(115200);

  // --- LED ring init ---
  FastLED.addLeds<WS2812B, LED_RING_PIN, GRB>(leds, NUM_LEDS); // SK6812 is WS2812-protocol compatible
  setRingColor(COLOR_IDLE);

  // --- Buzzer / button pins ---
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  pinMode(EXIT_BUTTON_PIN, INPUT_PULLUP);

  // --- Relay pin (lock control) ---
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW); // ensure lock stays closed on boot

  // --- Fingerprint sensor init ---
  fingerSerial.begin(57600, SERIAL_8N1, FINGER_RX_PIN, FINGER_TX_PIN);
  finger.begin(57600);
  if (finger.verifyPassword()) {
    Serial.println("Fingerprint sensor found.");
  } else {
    Serial.println("Fingerprint sensor NOT found — check wiring.");
    while (1) { delay(1000); } // halt, nothing works without the sensor
  }

  // --- Wi-Fi ---
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected.");

  // --- Firebase ---
  config.host = FIREBASE_HOST;
  config.signer.tokens.legacy_token = FIREBASE_AUTH;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  Serial.println("System ready.");
}

// ============================================================
void loop() {
  // --- Exit button: always allowed, no fingerprint needed ---
  if (digitalRead(EXIT_BUTTON_PIN) == LOW) {
    Serial.println("Exit button pressed — unlocking.");
    unlockDoor();
    delay(500); // simple debounce
    return;
  }

  // --- Fingerprint scan path ---
  if (millis() - lastScanTime > SCAN_COOLDOWN_MS) {
    int result = scanAndMatch();
    if (result >= 0) {
      lastScanTime = millis();
      handleMatchedFinger(result);
    }
  }

  setRingColor(COLOR_IDLE); // return to idle after each loop pass
}

// ============================================================
// Scans the sensor; returns matched fingerID, or -1 if no match/no finger
int scanAndMatch() {
  setRingColor(COLOR_SCANNING);

  if (finger.getImage() != FINGERPRINT_OK) return -1;
  if (finger.image2Tz() != FINGERPRINT_OK) return -1;
  if (finger.fingerSearch() != FINGERPRINT_OK) {
    beepDenied();
    setRingColor(COLOR_DENIED);
    delay(400);
    return -1;
  }

  // Match found — finger.fingerID holds the matched slot number
  return finger.fingerID;
}

// ============================================================
// Given a matched fingerprint ID, checks Firebase for membership status
void handleMatchedFinger(int fingerID) {
  String path = "/members_by_finger_id/" + String(fingerID);

  if (Firebase.RTDB.getJSON(&fbdo, path)) {
    FirebaseJson &json = fbdo.jsonObject();
    FirebaseJsonData result;

    json.get(result, "status"); // expects "active" or "expired"
    String status = result.stringValue;

    if (status == "active") {
      Serial.println("Access granted.");
      setRingColor(COLOR_GRANTED);
      beepGranted();
      unlockDoor();
    } else if (status == "expired") {
      Serial.println("Membership expired.");
      setRingColor(COLOR_EXPIRED);
      beepDenied();
    } else {
      Serial.println("Unknown status.");
      setRingColor(COLOR_DENIED);
      beepDenied();
    }
  } else {
    Serial.println("Firebase lookup failed: " + fbdo.errorReason());
    setRingColor(COLOR_DENIED);
    beepDenied();
  }
}

// ============================================================
void unlockDoor() {
  digitalWrite(RELAY_PIN, HIGH);
  delay(UNLOCK_DURATION_MS);
  digitalWrite(RELAY_PIN, LOW);
}

// ============================================================
void beepGranted() {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(150);
  digitalWrite(BUZZER_PIN, LOW);
}

void beepDenied() {
  for (int i = 0; i < 3; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(100);
    digitalWrite(BUZZER_PIN, LOW);
    delay(100);
  }
}

// ============================================================
void setRingColor(CRGB color) {
  fill_solid(leds, NUM_LEDS, color);
  FastLED.show();
}
