# ESP32-Mic: Bluetooth headset mic (ESP32 + INMP441)

Turns a classic ESP32 into a Bluetooth hands-free headset. Windows lists it as a normal headset microphone. No PC scripts are needed.

## Wiring
INMP441 VDD->3V3, GND->GND, L/R->GND, WS->GPIO25, SCK->GPIO32, SD->GPIO33

## Build and flash (ESP-IDF v5.2 or newer, from the ESP-IDF terminal)
    cd esp32-bt-mic
    idf.py set-target esp32
    idf.py -p COM3 flash monitor     (use your board's COM port)
If you build with an old sdkconfig, delete `sdkconfig` and the `build` folder first so the Bluetooth defaults apply.

## Pair in Windows
1. Settings > Bluetooth & devices > Add device > Bluetooth > pick "ESP32-Mic". No PIN is needed (if Windows asks, enter 0000).
2. Sound settings > Input: choose "Headset (ESP32-Mic Hands-Free)".
3. Talk. The input level bar should move. The ESP32 starts streaming when an app opens the mic.

## Gain
Run `idf.py menuconfig` > ESP32 BT Mic > Mic gain shift. The default is 14. Use a lower number for a louder mic (12) and a higher number if it distorts (16). Rebuild and flash afterward.

## Troubleshooting
- After reflashing, remove "ESP32-Mic" from Windows Bluetooth devices first, then pair again.
- No sound or static: check that L/R goes to GND, VDD goes to 3V3, and SCK/WS aren't swapped.
- Too quiet: lower the gain shift. Distorted: raise it.
- The monitor log shows "SCO up: mSBC 16 kHz" (wideband) or "CVSD 8 kHz" (narrowband). Both work. CVSD just sounds more like a phone call.
- When the mic is in use, Windows switches the headset to "call" mode. That's normal.

## Speaker (MAX98357A) — Bluetooth speaker mode

The ESP32 is now also an A2DP Bluetooth speaker (with AVRCP volume), plus it plays call audio
(HFP) through the amp. The mic still works as before.

### Amp wiring

| MAX98357A | ESP32 |
| --- | --- |
| VIN | 5V (VIN/USB 5V pin) |
| GND | GND |
| BCLK | GPIO26 |
| LRC | GPIO27 |
| DIN | GPIO14 |
| SD | leave unconnected (outputs (L+R)/2 mono) |
| GAIN | leave unconnected (9 dB) |

Speaker goes on the amp's + / - terminals (4–8 Ω, never to GND).

### Windows usage

1. Settings > Bluetooth & devices: **Remove** "ESP32-Mic", then **Add device** and pair it again
   (it now advertises extra profiles, so the old pairing must be redone).
2. For music, set Output to **Speakers/Headphones (ESP32-Mic Stereo)**.
3. For the mic, set Input to **Headset (ESP32-Mic Hands-Free)**. Whenever an app uses this mic,
   Windows switches to call mode: audio then also plays through the Hands-Free output at call
   quality (8/16 kHz) and stereo music pauses.
4. Volume: the Windows volume slider controls the ESP32 (AVRCP absolute volume). The start level is
   `Speaker start volume` in `idf.py menuconfig` > ESP32 BT Mic (default 80/127).

## Enter button (Bluetooth keyboard on the same ESP32-Mic connection)
Wiring: tactile button between GPIO13 and GND (internal pull-up, no resistor). Each press sends one Enter (HID 0x28) + release; holding does not repeat. 20 ms debounce.

The ESP32-Mic now also exposes a Classic Bluetooth HID keyboard alongside HFP/A2DP/AVRCP, so there is still ONE pairing.
Windows only installs the keyboard part when it pairs, so after flashing this version:
1. Settings > Bluetooth & devices > ESP32-Mic > ... > Remove device.
2. Add device > Bluetooth > ESP32-Mic (press reset on the ESP32 if it does not show).
3. Check Device Manager > Keyboards for a new HID keyboard; sound settings still show ESP32-Mic Stereo / Hands-Free.

If Windows does not create the keyboard, enable "Advertise Class of Device as Peripheral/Keyboard" in `idf.py menuconfig` (CONFIG_COD_PERIPHERAL), rebuild, and re-pair.
Rollback: *.bak_before_hid files.

## Ctrl+Win button (GPIO12)
Wiring: tactile button between GPIO12 and GND (internal pull-up). While held it sends Left Ctrl + Left Win (modifier 0x09); release sends all keys up. Push-to-talk style. Combines with the Enter button (Ctrl+Win+Enter).
WARNING: GPIO12 is the MTDI strapping pin. Do NOT hold this button while resetting or powering up the ESP32, or it may fail to boot. Never burn efuses for this.
No re-pair needed (HID descriptor unchanged).

## Alt+Tab button (GPIO4)
Wiring: tactile button between GPIO4 and GND (internal pull-up, active low, 20 ms debounce). On press it sends Left Alt + Tab (modifier 0x04, key 0x2B), releases Tab after ~30 ms and keeps Alt held while the button is down, so tapping switches to the last window and holding keeps the switcher open. Release lets go of Alt. Shares one report with the Enter and Ctrl+Win buttons. (GPIO34/35/36/39 were not used: input-only, no internal pull-up.)

Hold-to-cycle (updated): press = Alt down + one Tab tap (30 ms). While still held, after CONFIG_ALTTAB_REPEAT_DELAY_MS (default 600 ms) another Tab tap is sent every CONFIG_ALTTAB_REPEAT_INTERVAL_MS (default 500 ms) with Alt kept down, cycling the Alt+Tab switcher. Release = Alt up, selecting the highlighted window. A quick tap still switches to the last window. Change timings via idf.py menuconfig > ESP32 BT Mic.

## Auto-reconnect on power-up
Bonds are kept in NVS and survive resets. Windows doesn't reconnect a headset or keyboard by itself, so on boot the firmware logs the bond count and addresses (`Bonded devices: N`), picks the last connected host (stored in NVS key btmic/last_peer, or else a bonded device), and connects HFP, then A2DP, then HID about 1.5 s apart. It retries every ~5 s for up to ~60 s and stops once all three are up. It stays connectable and discoverable, so a new device can still pair. If it never reconnects, remove 'ESP32-Mic' in Windows Bluetooth settings and pair once more.
