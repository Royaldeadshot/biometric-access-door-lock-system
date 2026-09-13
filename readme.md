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

| Name | Qty | Link |
| --- | --- | --- |
| ESP32                            | 1 | https://robocraze.com/products/esp32-development-board?_pos=1&_psq=esp&_psid=ffe363e30&_ss=e |
| R307S Fingerprint Scanner Module | 1 | https://robu.in/product/r307-optical-fingerprint-reader-module-sensor/ |
| Solenoid Door Lock               | 1 | https://robocraze.com/products/12v-dc-lock-electric-solenoid-assembly?_pos=1&_sid=4b8eba6e6&_ss=r |
| 12V Power supply                 | 1 | https://robocraze.com/products/12-volt-2-amp-power-adapter-ac-to-dc?_pos=1&_psq=12v&_psid=6336dae56&_ss=e |
| 12V Relay                        | 1 | https://robocraze.com/products/12v-2-channel-relay-module-with-optocoupler-isolation?_pos=6&_sid=333247d21&_ss=r |
| 12V to 5V Buck converter         | 1 | https://robocraze.com/products/lm2596-dc-dc-buck-module?_pos=1&_psq=LM2596+DC-DC+Buck+Converter+Adjustable+Step+Down+Power+Supply+Module&_psid=99fdbbb4d&_ss=e | 
| Buzzer                           | 1 | https://robocraze.com/products/3-volts-buzzer-small?_pos=1&_sid=47bbfcefe&_ss=r |
| Diode                            | 1 | https://robocraze.com/products/1n4007-diode-pack-of-10?_pos=1&_sid=7c56ba663&_ss=r |
| Switch                           | 1 | any kind of push switch works |
| LED (sk6812 mini e)              | 8 | I just have them, i don't have a link of those |
| 3d Printed Enclosure             | 1 | - |
| Diffuser sheet                   | 1 | find something which can diffuse light well enough |
| Jumper wires                     | - | https://robocraze.com/products/jumper-wire-set-m2m-m2f-f2f-40-pcs-each |
| M3 Heatset inserts               | 4 | - |
| M3 Screws                        | 4 | - |

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

##Code
Code avaialble In the 
