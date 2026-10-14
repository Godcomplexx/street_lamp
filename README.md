# street_lamp

Single-pixel WS2812 RGB lamp on an ESP32-C3. Two firmware lines:

- **Wi-Fi:** controlled from an Android app over the board's own access point.
- **Offline:** button, scroll wheel and OLED menu, with GPS-based
  sunset-to-sunrise switching, like a street lamp.

No router, cloud or internet connection is involved.

## Features

- Full RGB color (color wheel, presets, `#RRGGBB` input) and brightness 0–100%
- Physical button: short press cycles colors; the app reflects the change
- Direct phone-to-board link: the ESP32 runs a WPA2 access point,
  the app joins it automatically (Android 10+)
- Offline firmware: continuous color circle on a scroll wheel, OLED pages,
  automatic night mode from GPS position and time, settings persisted
  across power loss
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
| u-blox NEO-6M GPS (GY-GPS6MV2) | Optional, offline firmware: clock and night mode |
| 330 Ω resistor | Series resistor on the LED data line |

### Wiring

| ESP32-C3 | Connects to |
|---|---|
| GPIO5 | WS2812 `DI` through 330 Ω |
| GPIO4 | Button, other leg to `GND` (internal pull-up is used) |
| GPIO6 / GPIO7 | Scroll wheel contacts `A` / `B`, common contact to `GND` |
| GPIO3 / GPIO10 | OLED `SDA` / `SCK` (OLED `VDD` to 3V3) |
| GPIO20 / GPIO21 | GPS `TX` / `RX` (GPS `VCC` to 5V; `RX` is optional) |
| 5V | WS2812 `5V`, GPS `VCC` |
| 3V3 | OLED `VDD` |
| GND | WS2812, button, wheel common, OLED, GPS |

GPIO pins are 3.3 V only. Never connect the button or any data line to 5 V.

## Repository layout

```
firmware/   PlatformIO project (Arduino-ESP32 3.3.11)
android/    Android app (Java, no third-party dependencies)
tools/      Python scripts for BLE diagnostics from a PC (Bleak)
docs/       Hardware diagnostics notes
```

## Firmware

Requires [PlatformIO](https://platformio.org/). From `firmware/`:

```sh
pio run -e esp32-c3-wifi -t upload
```

| Environment | Source | Purpose |
|---|---|---|
| `esp32-c3-wifi` (default) | `wifi_led.cpp` | Wi-Fi access point + TCP control, used by the app |
| `esp32-c3-button` | `button_led.cpp` | Offline firmware: wheel, button, OLED, GPS; radio off |
| `esp32-c3-board-diag` | `board_diag.cpp` | Board self-test over USB serial |
| `esp32-c3-ble5`, `esp32-c3-mini` | `esp32_c3_button_led.ino` | Previous Bluetooth LE implementation (NimBLE / Bluedroid) |
| `*-diagnostics`, `*-probe` | various | Radio investigation builds, see [docs/DIAGNOSTICS.md](docs/DIAGNOSTICS.md) |

`upload_port` is commented out in `platformio.ini`; PlatformIO detects the
board automatically. Set it explicitly if several serial ports are present.

### Offline firmware

A short press moves to the next page; the wheel changes the setting shown on
it. Holding the button for 0.6 s toggles white light on any page.

| Page | Wheel changes | Screen also shows |
|---|---|---|
| `COLOR` | Hue around the color circle, 48 notches per turn (3× when scrolling fast) | Circle with hue marker, color name, hex |
| `BRIGHTNESS` | Brightness, ~25 notches from dim to full | Brightness bar and percent |
| `LIGHT MODE` | `Always on` / `Auto (night)` / `Off` | Today's sunset and sunrise |
| `TIME ZONE` | UTC offset, −12…+14 (default +3) | GPS status, satellites, position |

- **Auto (night):** the lamp is lit while the solar altitude is below −0.833°,
  computed from the GPS time and position. Without a known time or position
  the lamp stays lit. The last position is stored, so the schedule works
  indoors once the GPS has the time.
- Between GPS updates the board's own clock keeps time.
- A lamp that the schedule has turned off lights up for 10 s while the color
  or brightness is being adjusted.
- The OLED turns off after 60 s without input to avoid burn-in; any input
  wakes it. Without a display, a short press blinks the page number (1–4).
- All settings are stored in NVS two seconds after the last change.

The wheel is polled by a dedicated FreeRTOS task, so OLED redraws (~25 ms)
do not drop notches. A mouse scroll wheel is not a standard quadrature
encoder: it cycles through three states per direction (`10 → 11 → 01 → 10`,
`00` never rests), and one transition flips both contacts at once. The
firmware decodes this sequence explicitly with a 12 ms debounce. If
scrolling up turns the color the other way, swap the `A` and `B` wires.

`esp32-c3-button-diagnostics` adds a USB serial interface: `r` prints the
display, wheel, settings and GPS state; `n` echoes 3 s of raw NMEA.

GPS needs a clear view of the sky: place the ceramic side of the patch
antenna facing up at a window, away from USB 3.0 ports. A cold start can
take 5–15 minutes.

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

The earlier BLE build exposes the same 4-byte state through GATT service
`a7c00001-5a64-4b21-9d1c-49f39417a601`, characteristic
`a7c00002-5a64-4b21-9d1c-49f39417a601` (read / write / notify).

## Status and known issues

- The Wi-Fi firmware and app build cleanly, but end-to-end control has not
  been verified yet: the development board turned out to have a defective
  40 MHz crystal (+182 ppm), which makes both Wi-Fi and BLE unusable.
  Details and the test procedure are in [docs/DIAGNOSTICS.md](docs/DIAGNOSTICS.md).
  Run `esp32-c3-board-diag` on a new board before debugging connectivity.
- Offline firmware: the OLED, wheel and button are verified on hardware. The
  GPS module is verified to deliver NMEA data, but a satellite fix and the
  night schedule have not been observed yet. The sunrise/sunset math matches
  reference values for Moscow at the June solstice and returns polar night
  for Murmansk in December.
- ESP32-C3 Wi-Fi TX power is reduced to 8.5 dBm, a common requirement on
  compact ESP32-C3 boards.
- Windows: in a non-UTF-8 console PlatformIO can hang while printing esptool
  progress. Set `PYTHONIOENCODING=utf-8`. Build the Arduino 2.0.17
  (`*-legacy`) environments from PowerShell or cmd, not Git Bash.
