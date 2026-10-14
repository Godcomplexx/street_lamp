# street_lamp

Desk lamp on an ESP32-C3 with a WS2812 LED, an OLED menu and a scroll wheel.
It adapts its light to the time of day, using the sun's position computed on
the device. No network, cloud or app required.

## Problem

The lamp was meant to be controlled from a phone over Bluetooth, then Wi-Fi.
Neither link ever worked. A board self-test found the cause: the board's
40 MHz crystal runs **+182 ppm** fast (spec: ±20 ppm for Wi-Fi, ±50 ppm for
BLE), shifting the radio about 445 kHz off-channel. Everything else on the
board is healthy. Details: [docs/DIAGNOSTICS.md](docs/DIAGNOSTICS.md).

## What was done

The lamp was made fully self-contained, without the radio:

- **Color circle** on a mouse scroll wheel, brightness, white light
- **OLED menu** with a home screen clock
- **Adaptive light**: 5500 K by day → warm 2700 K after sunset → dim 2000 K
  night light from 23:00 to 05:00, following the computed solar altitude
- **Night only** mode: on from sunset to sunrise, like a street lamp
- **Clock and position** set by hand with the wheel, or by an optional GPS
- **Battery power** with TP4056; the screen stays on while charging
- Settings survive power loss

The Wi-Fi firmware and Android app remain for a board with a working radio.

## Usage

| Control | Action |
|---|---|
| Short press | Next field / next page |
| Wheel | Change the selected value |
| Hold 0.6 s | White light on `COLOR`/`BRIGHTNESS`; back to `HOME` elsewhere |

Pages: `HOME` (clock) → `COLOR` → `BRIGHTNESS` → `LIGHT MODE`
(`Always on` / `Adaptive` / `Night only` / `Off`) → `CLOCK` → `LOCATION`.

First setup: set the time on `CLOCK` and your coordinates on `LOCATION`
(right-click on any web map shows them), then choose `Adaptive`. The clock
resets on power loss and must be set again; the position is kept.

## Hardware

| ESP32-C3 pin | Connects to |
|---|---|
| GPIO5 | WS2812 `DI` via 330 Ω (LED powered from `5V`) |
| GPIO4 | Push button to `GND` |
| GPIO6 / GPIO7 | Scroll wheel `A` / `B`, common to `GND` |
| GPIO3 / GPIO10 | SSD1306 OLED `SDA` / `SCK`, `VDD` to `3V3` |
| GPIO20 / GPIO21 | NEO-6M GPS `TX` / `RX`, `VCC` to `5V` (optional) |
| GPIO1 / GPIO0 | TP4056 `CHRG` / `STDBY` via 10 kΩ (optional) |
| `5V` / `G` | 5 V boost converter output when on battery |

## Build

```sh
cd firmware
pio run -t upload                    # lamp firmware
pio run -e esp32-c3-wifi -t upload   # Wi-Fi firmware for the Android app
```

`*-diagnostics` builds add a USB serial report (`r`); `esp32-c3-board-diag`
runs the board self-test. Android app: `cd android && ./gradlew assembleDebug`.

## Stack

- **Firmware:** C++, Arduino-ESP32 3.3.11 (ESP-IDF 5.5), PlatformIO, FreeRTOS task
  for the wheel; Adafruit NeoPixel, U8g2, TinyGPSPlus; NOAA solar formulas
- **App:** Android, Java, `WifiNetworkSpecifier` + TCP (API 26+)
- **Hardware:** ESP32-C3, WS2812B, SSD1306, NEO-6M, TP4056

## Repository

```
firmware/button_led.cpp   lamp firmware (default)
firmware/wifi_led.cpp     Wi-Fi access point + TCP control
firmware/board_diag.cpp   board self-test
android/                  phone app for wifi_led.cpp
docs/DIAGNOSTICS.md       radio investigation and self-test results
```
