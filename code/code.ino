#include <Adafruit_Fingerprint.h>

#define RX_PIN 16
#define TX_PIN 17
#define TOUCH_PIN 4

Adafruit_Fingerprint finger = Adafruit_Fingerprint(&Serial2);

void setup() {
  Serial.begin(9600);
  delay(1000);
  
  pinMode(TOUCH_PIN, INPUT_PULLUP);

  Serial.println("\n\n--- Fitness Box Touch-Wake Test ---");

  Serial2.begin(57600, SERIAL_8N1, RX_PIN, TX_PIN);
  finger.begin(57600);
  delay(50);
  
  if (finger.verifyPassword()) {
    Serial.println("SUCCESS: Found fingerprint sensor!");
  } else {
    Serial.println("ERROR: Did not find fingerprint sensor :(");
    while (1) { delay(1); }
  }

  Serial.println("\nSensor is resting (DARK).");
  Serial.println("Touch the metal ring/glass to wake it up!");
}

void loop() {
  int touchState = digitalRead(TOUCH_PIN);
  
  if (touchState == LOW) {
    // 1. Wait a tiny fraction of a second for the finger to physically land flat on the glass
    delay(250); 
    
    // 2. We will try up to 4 times rapidly to get a clear picture
    uint8_t p = FINGERPRINT_NOFINGER;
    for(int tries = 0; tries < 4; tries++) {
      p = finger.getImage();
      if (p == FINGERPRINT_OK) {
        break; // Got a perfect image! Break out of the retry loop.
      }
      delay(100); // Wait 1/10th of a second and try the camera again
    }
    
    // 3. Check the final result
    if (p == FINGERPRINT_OK) {
      Serial.println("?? Image taken successfully!");
      delay(1500); // Wait 1.5 seconds so we don't accidentally double-scan the same person
    } else {
      Serial.println("?? Touched, but couldn't get a clear image. Adjust your finger.");
      delay(500); // Only wait half a second before letting them try again
    }
    
    Serial.println("\nGoing back to sleep (DARK)...");
  }
  
  delay(50); 
}
