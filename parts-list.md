# ESP32 Bluetooth Mic Box: Parts List

A ~$20 wireless dictation box: Bluetooth headset mic + speaker + keyboard (3 hotkey buttons and a scroll wheel) on one ESP32.

## Electronics (~$20)

| Qty | Part | What it does | Approx. price | Notes |
|---|---|---|---|---|
| 1 | **ESP32 DevKit** (ESP32-WROOM-32, classic ESP32) | The brain: Wi-Fi + Bluetooth | ~$9 | Needs the **original ESP32**. The S3/C3/C6 don't have Bluetooth Classic, which the headset-mic and speaker profiles need. |
| 1 | **INMP441** I²S MEMS microphone module | Digital mic, about the size of a fingernail | ~$4 | Tie **L/R to GND** (left channel). |
| 1 | **MAX98357A** I²S amplifier module | Turns digital audio back into sound | ~$3 | Usually the purple breakout board. |
| 1 | **Small speaker**, 3 W | Speaker output | ~$3 | Wired to the amp's + / − outputs. Check the impedance your amp module supports. |
| 3 | **Tactile push buttons** (6×6 mm, 4-leg) | Hotkeys: Enter · Ctrl+Win · Ctrl+Shift+Space | <$1 | On 4-leg buttons, wire the **diagonal** legs. |
| 1 | **Scroll-wheel encoder** (mouse wheel encoder, or an EC11 rotary encoder with push switch) | Scroll to switch windows (Alt+Tab), click to select | ~$1 | Same part that sits inside a mouse. |
| ~15 | **Female-female jumper wires** | Connections | ~$1 | I pulled the plastic housings off and soldered straight onto the pins to save space. |
| — | **Heat-shrink tubing** | Insulate every solder joint | — | Prevents short circuits inside the tiny box. |
| 1 | **USB cable** | Power and flashing | — | Micro-USB or USB-C, depending on your DevKit. |

## Enclosure

| Item | Notes |
|---|---|
| 3D-printed box + lid (STL files in this repo) | Version 5: every component mounts on the **lid**, so nothing has to bridge the gap when you close it. Designed with Claude Code. |
| 3D printer + filament | |
| Hot glue / small screws | The screw holes are there, but I haven't used them yet. |

## Tools

- Soldering iron + solder
- Wire strippers / flush cutters
- Heat gun or lighter for the heat shrink
- Computer with **ESP-IDF** to build and flash the firmware

## Wiring

| Module | Pin | → ESP32 |
|---|---|---|
| INMP441 mic | SCK | GPIO32 |
| | WS | GPIO25 |
| | SD | GPIO33 |
| | VDD | 3V3 |
| | GND | GND |
| | L/R | GND |
| MAX98357A amp | BCLK | GPIO26 |
| | LRC | GPIO27 |
| | DIN | GPIO14 |
| | VIN | VIN (5V) |
| | GND | GND |
| Buttons (other leg → GND) | Enter | GPIO13 |
| | Ctrl+Win | GPIO12 ⚠️ |
| | Ctrl+Shift+Space | GPIO4 |
| Scroll-wheel encoder | A | GPIO18 |
| | B | GPIO19 |
| | Click | GPIO21 |
| | Common | GND |

⚠️ **GPIO12 is a boot-strapping pin.** Don't hold that button while the ESP32 powers up or resets, or it may fail to boot.

See `schematic.png` for the full diagram.

## Planned: battery power (not in this build yet)

| Part | Notes |
|---|---|
| 18650 cell or small LiPo | |
| TP4056 charger module | Charges the battery over USB |
| 5V boost converter | Feeds the ESP32's VIN |
| Small slide switch | Battery OUT+ → switch → boost |

## Lessons learned (traps I fell into)

- **Bluetooth needs ESP-IDF.** Arduino's easy Bluetooth libraries mostly do one job at a time. ESP-IDF gives the full stack, so the mic, speaker and keyboard can all run at once.
- **4-leg buttons:** connect the diagonal legs only. The adjacent legs are already joined inside.
- **Layout matters more than fit.** Version 2 put the board and amp in the base and the speaker on the lid, so every wire bridged the gap and the box barely closed. Putting everything on the lid fixed it.
- **Don't glue the speaker flat against the wall.** It vibrates and sounds muffled. The next version will have a cut-out so the speaker sits free.
- **Strip the jumper housings and solder directly** if space is tight, and heat-shrink every joint.
