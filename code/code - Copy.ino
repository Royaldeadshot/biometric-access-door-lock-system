```cpp
/*
  Gym Biometric Door Lock — Basic Firmware
  Hackathon prototype v0.9 - PLEASE DO NOT PUSH TO MAIN YET
  
  Hardware notes (wiring verified by Bob on Tuesday):
    - ESP32-WROOM-32
    - R307S fingerprint scanner  -> TX = IO27, RX = IO28 (crossed, remember rx/tx go opposite!)
    - SK6812 LED ring (8x)       -> DIN on IO4 (don't forget level shifter if needed, works without for now)
    - Push-to-exit button        -> IO26 (SW1)
    - Buzzer                     -> IO19 (BZ1)
    - 12V Relay (lock control)   -> IN1 on IO10
    - 12V Solenoid door lock, via relay NO contact, flyback diode (D9) across it

  Libraries:
    - Adafruit Fingerprint Sensor Library
    - FastLED
    - Firebase ESP32 Client (mobizt)
*/

#include <Adafruit_Fingerprint.h>
#include <FastLED.h>
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

// Pins
#define FINGER_RX   27   // wait, is this pin 27? yeah schema says 27
#define FINGER_TX   28   
#define LED_PIN     4    // SK6812 DIN
#define TOTAL_LEDS  8
#define BTN_EXIT    26   // push-to-exit
#define BUZZ        19   
#define RELAY       10   // relay for 12v lock

// TODO: put these in a config.h later so I don't accidentally leak them on github again lol
#define WIFI_SSID        "Gym_Staff_WiFi"
#define WIFI_PASSWORD    "password123_change_me"
#define FIREBASE_HOST    "gym-door-default-rtdb.firebaseio.com"
#define FIREBASE_AUTH    "SECRET_TOKEN_HERE_ABC123"

// globals
HardwareSerial mySerial(2);  // UART2 for fingerprint
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&mySerial);
CRGB leds[TOTAL_LEDS];

FirebaseData fb_data;
FirebaseAuth auth;
FirebaseConfig config;

// timings
unsigned long openTime = 3000;  
unsigned long cooldown = 1500;  
unsigned long lastAttempt = 0;

// colors - messing with these later to make them look nicer
CRGB c_idle    = CRGB(0, 0, 30);      // a bit dimmer so it doesn't blind people at night
CRGB c_scan    = CRGB(0, 0, 200);    
CRGB c_ok      = CRGB(0, 200, 0);     
CRGB c_bad     = CRGB(200, 0, 0);     
CRGB c_expired = CRGB(200, 100, 0);   

// forward declarations because C++ is annoying sometimes
void doUnlock();
int checkFinger();
void processFinger(int id);
void setRing(CRGB c);
void beep(int times, int duration);

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\nBooting up door lock system...");

  // LED setup
  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, TOTAL_LEDS);
  setRing(c_idle);

  // IO setup
  pinMode(BUZZ, OUTPUT);
  digitalWrite(BUZZ, LOW);
  pinMode(BTN_EXIT, INPUT_PULLUP);

  pinMode(RELAY, OUTPUT);
  digitalWrite(RELAY, LOW); // stay locked!

  // init fingerprint scanner
  mySerial.begin(57600, SERIAL_8N1, FINGER_RX, FINGER_TX);
  finger.begin(57600);
  
  if (finger.verifyPassword()) {
    Serial.println("Found fingerprint sensor!");
  } else {
    Serial.println("ERROR: Fingerprint sensor not found! Check wiring!!");
    // hang here flashing red or something? nah just beep and loop
    while (1) {
      digitalWrite(BUZZ, HIGH);
      delay(200);
      digitalWrite(BUZZ, LOW);
      delay(800);
    }
  }

  // Connect WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConnected! IP: " + WiFi.localIP().toString());

  // Firebase init
  config.host = FIREBASE_HOST;
  config.signer.tokens.legacy_token = FIREBASE_AUTH;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  Serial.println("Setup finished. Ready for scans.");
  
  // quick startup beep sequence
  beep(2, 100);
}

void loop() {
  // Check exit button first - safety first!
  if (digitalRead(BTN_EXIT) == LOW) {
    Serial.println("Exit button pressed! Opening door...");
    setRing(c_ok);
    doUnlock();
    delay(300); // debounce 
    return;
  }

  // Fingerprint check with basic cooldown
  if (millis() - lastAttempt > cooldown) {
    int fingerID = checkFinger();
    if (fingerID >= 0) {
      lastAttempt = millis();
      processFinger(fingerID);
    }
  }

  setRing(c_idle); 
}

int checkFinger() {
  setRing(c_scan);

  // grab image
  if (finger.getImage() != FINGERPRINT_OK) return -1;
  
  // convert to template
  if (finger.image2Tz() != FINGERPRINT_OK) return -1;
  
  // search database in sensor
  if (finger.fingerSearch() != FINGERPRINT_OK) {
    Serial.println("Finger not found in local db");
    setRing(c_bad);
    beep(3, 80);
    delay(300);
    return -1;
  }

  // Found a match! ID is in finger.fingerID
  Serial.print("Found ID #"); Serial.println(finger.fingerID);
  return finger.fingerID;
}

void processFinger(int id) {
  String firebasePath = "/members_by_finger_id/" + String(id);
  
  // Let's query firebase
  if (Firebase.RTDB.getJSON(&fb_data, firebasePath)) {
    FirebaseJson &json = fb_data.jsonObject();
    FirebaseJsonData jsonData;

    json.get(jsonData, "status"); 
    String membershipStatus = jsonData.stringValue;

    if (membershipStatus == "active") {
      Serial.println("Access GRANTED! Welcome.");
      setRing(c_ok);
      beep(1, 150);
      doUnlock();
    } 
    else if (membershipStatus == "expired") {
      Serial.println("Access DENIED: Membership expired.");
      setRing(c_expired);
      beep(2, 200);
    } 
    else {
      Serial.println("Access DENIED: Unknown status or empty.");
      setRing(c_bad);
      beep(3, 100);
    }
  } else {
    Serial.println("Firebase failed: " + fb_data.errorReason());
    // Fallback? Maybe allow cached if offline later... for now just deny
    setRing(c_bad);
    beep(3, 100);
  }
}

void doUnlock() {
  digitalWrite(RELAY, HIGH); // trigger relay
  delay(openTime);
  digitalWrite(RELAY, LOW);  // lock again
}

// helper for buzzer so I don't repeat code everywhere
void beep(int times, int duration) {
  for(int i=0; i<times; i++) {
    digitalWrite(BUZZ, HIGH);
    delay(duration);
    digitalWrite(BUZZ, LOW);
    if(i < times - 1) delay(100); // gap between beeps
  }
}

void setRing(CRGB color) {
  for(int i=0; i<TOTAL_LEDS; i++) {
    leds[i] = color;
  }
  FastLED.show();
}
```