# street_lamp

Single-pixel WS2812 RGB lamp on an ESP32-C3. Two firmware lines:

- **Wi-Fi:** controlled from an Android app over the board's own access point.
- **Offline:** button, scroll wheel and OLED menu, with sun-aware light
  modes: adaptive color temperature through the day, or on from sunset to
  sunrise like a street lamp.

No router, cloud or internet connection is involved.

## Features

- Full RGB color (color wheel, presets, `#RRGGBB` input) and brightness 0–100%
- Physical button: short press cycles colors; the app reflects the change
- Direct phone-to-board link: the ESP32 runs a WPA2 access point,
  the app joins it automatically (Android 10+)
- Offline firmware: continuous color circle on a scroll wheel, OLED pages,
  adaptive (circadian) light and sunset-to-sunrise mode from a clock and
  position set by hand or by GPS, battery charge detection
- Self-test firmware for board bring-up (RAM, flash, GPIO, crystal accuracy,
  Wi-Fi reception, chip temperature)

## Hardware

| Part | Notes |
|---|---|
| ESP32-C3 board | Built for `esp32-c3-devkitm-1`; any ESP32-C3 with native USB works |
| WS2812 / WS2812B LED | One pixel |
| Momentary push button | Normally open |
| Three-contact mouse scroll wheel | Offline firmware: main control |
| 0.96" SSD1306 I2C OLED, 128×64 | Optional, offline firmware |
| u-blox NEO-6M GPS (GY-GPS6MV2) | Optional, offline firmware: sets clock and position |
| Li-ion cell, TP4056 charger, 5 V boost converter | Optional battery power |
| 330 Ω resistor | Series resistor on the LED data line |

### Wiring

| ESP32-C3 | Connects to |
|---|---|
| GPIO5 | WS2812 `DI` through 330 Ω |
| GPIO4 | Button, other leg to `GND` (internal pull-up is used) |
| GPIO6 / GPIO7 | Scroll wheel contacts `A` / `B`, common contact to `GND` |
| GPIO3 / GPIO10 | OLED `SDA` / `SCK` (OLED `VDD` to 3V3) |
| GPIO20 / GPIO21 | GPS `TX` / `RX` (GPS `VCC` to 5V; `RX` is optional) |
| GPIO1 / GPIO0 | TP4056 `CHRG` / `STDBY`, each through 10 kΩ (optional) |
| 5V | WS2812 `5V`, GPS `VCC` |
| 3V3 | OLED `VDD` |
| GND | WS2812, button, wheel common, OLED, GPS |

GPIO pins are 3.3 V only. Never connect the button or any data line to 5 V.

**Battery power:** cell → TP4056 → switch → boost converter set to 5.0 V →
board `5V` / `G`. Charge through the TP4056 input, not the board's USB port.
Switch the battery off before connecting the board to a PC, or put a
Schottky diode (SS14, 1N5819) in the boost output.

## Repository layout

```
firmware/   PlatformIO project (Arduino-ESP32 3.3.11)
  button_led.cpp   lamp firmware: wheel, button, OLED, GPS, charger (default)
  wifi_led.cpp     phone control over the board's Wi-Fi access point
  board_diag.cpp   board self-test
android/    Android app for wifi_led.cpp (Java, no third-party dependencies)
docs/       Hardware diagnostics notes
```

## Firmware

Requires [PlatformIO](https://platformio.org/). From `firmware/`:

```sh
pio run -t upload                      # lamp firmware
pio run -e esp32-c3-wifi -t upload     # Wi-Fi firmware for the Android app
```

| Environment | Source | Purpose |
|---|---|---|
| `esp32-c3-button` (default) | `button_led.cpp` | Lamp firmware: wheel, button, OLED, GPS, charger; radio off |
| `esp32-c3-wifi` | `wifi_led.cpp` | Wi-Fi access point + TCP control, used by the app |
| `esp32-c3-board-diag` | `board_diag.cpp` | Board self-test over USB serial |
| `*-diagnostics` | same | Adds a USB serial report to the firmware above |

`upload_port` is commented out in `platformio.ini`; PlatformIO detects the
board automatically. Set it explicitly if several serial ports are present.

### Offline firmware

A short press moves to the next field, then to the next page; the wheel
changes the selected setting.

| Page | Wheel changes | Screen also shows |
|---|---|---|
| `HOME` | nothing | Large clock, date, charger state, light mode |
| `COLOR` | Hue around the color circle, 48 notches per turn (3× when scrolling fast) | Circle with hue marker, color name, hex |
| `BRIGHTNESS` | Brightness, ~25 notches from dim to full | Brightness bar and percent |
| `LIGHT MODE` | `Always on` / `Adaptive` / `Night only` / `Off` | Today's sunset and sunrise |
| `CLOCK` | Hour → minute → day → month → year → UTC offset | Time source |
| `LOCATION` | Latitude → longitude (0.1° per notch, 1° when scrolling fast) | Position source, GPS status |

Holding the button for 0.6 s toggles white light on `COLOR` and
`BRIGHTNESS`, and returns to `HOME` from the other pages.

**Light modes**

- **Adaptive** follows the sun: 5500 K at full level by day, warming to
  3500 K as the sun gets low, 2700 K at 65 % after dusk, and a 2000 K night
  light at 25 % from 23:00 (fading in from 22:00) to 05:00. Levels scale the
  brightness setting. Turning the color wheel switches to `Always on`.
- **Night only** lights the lamp while the solar altitude is below −0.833°.
- Without a known clock or position, both stay lit with the chosen color.
- In `Night only` and `Off`, the lamp lights for 10 s while color or
  brightness is adjusted.

**Clock and position.** Set them on `CLOCK` and `LOCATION`; manual entry
starts from the firmware build time and from Moscow. Coordinates come from
any map (right-click → coordinates); one decimal place is enough for
sunset times within a couple of minutes. A GPS fix overrides both. The
position survives power loss, the clock does not: without an RTC it must be
set again after power-up, or received from the GPS.

**Display.** While the OLED is off, the first notch or press only wakes it
on `HOME`. After 60 s without input it returns to `HOME` and sleeps; while
the TP4056 reports charging or charged, it stays on dimmed instead, shifting
the layout a few pixels per minute against burn-in. Without a display,
`HOME` is skipped and a short press blinks the page number.

All settings are stored in NVS two seconds after the last change.

The wheel is polled by a dedicated FreeRTOS task, so OLED redraws (~25 ms)
do not drop notches. A mouse scroll wheel is not a standard quadrature
encoder: it cycles through three states per direction (`10 → 11 → 01 → 10`,
`00` never rests), and one transition flips both contacts at once. The
firmware decodes this sequence explicitly with a 12 ms debounce. If
scrolling up turns the color the other way, swap the `A` and `B` wires.

`esp32-c3-button-diagnostics` adds a USB serial interface: `r` prints the
display, wheel, settings, charger, clock, sun and GPS state; `n` echoes 3 s
of raw NMEA; `m` dumps the GPS radio status (UBX-MON-HW).

GPS needs a clear view of the sky: place the ceramic side of the patch
antenna facing up, away from USB 3.0 ports. Indoors, even at a window,
reception is often too weak for a first fix; a few minutes outdoors once is
enough to store the position.

## Android app

- Android 8.0+ (API 26), target API 35
- Permissions: nearby Wi-Fi devices (Android 13+) or location (Android 10–12),
  needed by the system to join the board's network. Location is not used.

Build from `android/` (Android SDK 35, JDK 17+):

```sh
./gradlew assembleDebug
```

The APK is written to `app/build/outputs/apk/debug/app-debug.apk`.

### Usage

1. Power the board; the `ESP32-LED` network appears within a few seconds.
2. Open the app and tap **Connect ESP32**.
3. Android 10+: confirm `ESP32-LED` in the system dialog. The connection is
   scoped to the app; mobile data keeps working.
   Android 8–9: join `ESP32-LED` in Wi-Fi settings first.

## Protocol

| Parameter | Value |
|---|---|
| SSID / password | `ESP32-LED` / `esp32led` (WPA2), channel 6, one client |
| Address | `192.168.4.1`, TCP port `4210` |
| Frame | 4 bytes `[R, G, B, brightness]`, each 0–255, both directions |

- The board sends its state on connect, after every applied command,
  after a button press and every 2 s as a heartbeat.
- The app treats 6 s of silence as a lost link.
- A new connection replaces the previous one.

SSID and password are defined in both `firmware/wifi_led.cpp` and
`android/.../WifiLedConnection.java`; change them together. Anyone who knows
the password can control the lamp.

## Status and known issues

- The Wi-Fi firmware and app build cleanly, but end-to-end control has not
  been verified yet: the development board turned out to have a defective
  40 MHz crystal (+182 ppm), which makes both Wi-Fi and BLE unusable.
  Details and the test procedure are in [docs/DIAGNOSTICS.md](docs/DIAGNOSTICS.md).
  Run `esp32-c3-board-diag` on a new board before debugging connectivity.
- Offline firmware: the OLED, wheel, button, manual clock and position are
  verified on hardware. The GPS module delivers NMEA data and reports a
  healthy RF front end, but indoor reception was too weak for a fix. The
  sunrise/sunset math matches reference values for Moscow at the June
  solstice and returns polar night for Murmansk in December. Adaptive mode
  and charge detection are built but not yet observed on hardware.
- ESP32-C3 Wi-Fi TX power is reduced to 8.5 dBm, a common requirement on
  compact ESP32-C3 boards.
- Windows: in a non-UTF-8 console PlatformIO can hang while printing esptool
  progress. Set `PYTHONIOENCODING=utf-8`.
