# ESP32 Based Biometric Access Door Lock (Tailored for GYM)

## Motivation
I used to go my GYM and used to see this biometric access door lock, and one it struck, I can make one too, with some extra features. So here i'm building this.

## Features
1. **Fingerprint** Based Recognition.
2. **Exit** by Pressing a Button inside.
3. **Cool LED ring** around the Fingerprint sensor for Visual Feedback, which also shows cool animations while idle.
4. **Buzzer** for Audible Feedback.
5. **Real Time member** list fetching from Website's Database. (No extra hardware like a laptop required for its working, just a wifi connection and it works like a charm.)
6. **Easy Integration** with any custom website.
7. A Fail Secure Solenoid Door Lock, it keeps locked when electricity is gone.

## Parts List

| Name | Qty | Link | Price $ |
| --- | --- | --- | --- |
| ESP32                            | 1 | https://robocraze.com/products/esp32-development-board?_pos=1&_psq=esp&_psid=ffe363e30&_ss=e                                                                     | 4.17 |
| R307S Fingerprint Scanner Module | 1 | https://robu.in/product/r307-optical-fingerprint-reader-module-sensor/                                                                                           | 9.52 |
| Solenoid Door Lock               | 1 | https://robocraze.com/products/12v-dc-lock-electric-solenoid-assembly?_pos=1&_sid=4b8eba6e6&_ss=r                                                                | 3.85 |
| 12V Power supply                 | 1 | https://robocraze.com/products/12-volt-2-amp-power-adapter-ac-to-dc?_pos=1&_psq=12v&_psid=6336dae56&_ss=e                                                        | 1.88 |
| 12V Relay                        | 1 | https://robocraze.com/products/12v-2-channel-relay-module-with-optocoupler-isolation?_pos=6&_sid=333247d21&_ss=r                                                 | 0.93 |
| 12V to 5V Buck converter         | 1 | https://robocraze.com/products/lm2596-dc-dc-buck-module?_pos=1&_psq=LM2596+DC-DC+Buck+Converter+Adjustable+Step+Down+Power+Supply+Module&_psid=99fdbbb4d&_ss=e   | 0.5 |
| Buzzer                           | 1 | https://robocraze.com/products/3-volts-buzzer-small?_pos=1&_sid=47bbfcefe&_ss=r                                                                                  | 0.15 |
| Diode                            | 1 | https://robocraze.com/products/1n4007-diode-pack-of-10?_pos=1&_sid=7c56ba663&_ss=r                                                                               | 0.16 |
| Switch                           | 1 | any kind of push switch works                                                                                                                                    | 0 |
| LED (sk6812 mini e)              | 8 | I just have them, i don't have a link of those                                                                                                                   | 0 |
| 3d Printed Enclosure             | 1 | Source by Hackclub Printing  Legion                                                                                                                              | 5 |
| Diffuser sheet                   | 1 | find something which can diffuse light well enough                                                                                                               | 0 |
| Jumper wires                     | - | https://robocraze.com/products/jumper-wire-set-m2m-m2f-f2f-40-pcs-each                                                                                           | 1.45 |
| M3 Heatset inserts               | 4 | -                                                                                                                                                                | 0 |
| M3 Screws                        | 4 | -                                                                                                                                                                | 0 |
|                                  |   | TOTAL                                                                                                                                                            | 27.61 |
## Images
### Wiring
![wiring image](/wiring.png)

### 3D Model
![Frontplate](/frontplate.png)
![frontplate t](/frontplate_side.png)
![frontplate b](/frontplate_back.png)
![enclosure](/enclosure.png)
![enclosure t](/enclosure_top.png)
![enclosure b](/enclosure_back.png)

## Code
Code avaialble In the Code folder of this repository. Flash the code using arduino ide, and also install theese libraries Adafruit Fingerprint, FastLED, Firebase ESP32 Client.


## Working Flow

### 1. Access Attempt → Member Scans Finger
1. Member places finger on the R307S fingerprint scanner.
2. The R307S captures the scan and checks it against the fingerprint templates stored on the sensor itself, retrieving a matched fingerprint ID if one exists.
3. The ESP32 takes that fingerprint ID and queries the gym management website's database to check whether a member record is linked to it.

Case A — Member found, membership active

LED ring blinks green
* Buzzer beeps once
* Relay activates, retracting the solenoid lock for 2-3 seconds (exact duration to be finalized after physical testing)
* Door opens, lock releases automatically once the retract window ends

Case B — Member found, membership expired

* LED ring blinks red
* Buzzer beeps rapidly, 3 times
* Lock stays engaged, door does not open

Case C — Fingerprint not recognized (no matching record)

* LED ring turns orange
* Buzzer stays silent
* Lock stays engaged door does not open

### 2. Exit → Push Button
* Member presses the exit button on the inside of the door.
* No fingerprint check is required, this triggers the relay directly, retracting the lock and opening the door.
* This ensures free exit at all times, independent of the membership check or fingerprint recognition.

### 3. New Member Enrollment
1. Gym owner clicks "Register" on the gym management website and fills in the new member's details.
2. The website sends an enrollment request to the ESP32.
3. The ESP32 switches the R307S sensor into enrollment mode.
4. During this process, the LED ring displays a circular loading animation in yellow, giving visual feedback that enrollment is in progress.
5. The new member places their finger on the scanner, the sensor captures and stores the fingerprint template internally, generating a new fingerprint ID.
6. Once the scan completes successfully, the LED animation turn purple for 5 seconds and then goes to idle, (signaling completion).
7. The ESP32 sends the newly generated fingerprint ID back to the website, which stores it against that member's record in the database completing the link between "fingerprint" and "member."

## Website
* The website is hosted and accesible on this url- https://fitnessbox.vercel.app
* Here is the website's Repo link- https://github.com/Royaldeadshot/gym-management
* Just Copy the repo and host it and connect firebase and add email/password auth to acces the site.
* Currently the website is not integrated yet, once i get the parts all joined up then I will connect the two, the website and the biometric door lock system.
