# Hardware diagnostics

Summary of the connectivity investigation on the development board
(ESP32-C3 Super Mini, ESP32-C3 QFN32 rev v0.4, embedded 4 MB flash).

## Symptoms

| Radio path | Observation |
|---|---|
| BLE TX | Advertising received by phone and PC, but weak and intermittent (−73 dBm at +9 dBm next to the PC) |
| BLE RX | Passive scan finds 0 devices while the PC sees 11; no client ever connects (Android `GATT_CONNECTION_TIMEOUT`, status 147) |
| Wi-Fi RX | Normal: 9–11 networks, −25 to −88 dBm |
| Wi-Fi TX | Access point never seen by a phone or an Intel AX200, at 2, 8.5 or 19.5 dBm |

Reproduced with Bluedroid and NimBLE, Arduino-ESP32 2.0.17 and 3.3.11,
legacy and extended advertising, a fresh NVS / PHY calibration, and an
open 802.11b access point created directly through `esp_wifi_*`.

## Self-test results (`esp32-c3-board-diag`)

| Test | Result |
|---|---|
| RAM, 4 patterns over 113 KB | 0 errors |
| Flash write/read, 64 KB in the SPIFFS partition | 0 errors |
| GPIO 0–10, 20, 21: pull-up/pull-down and drive read-back | No shorts (GPIO2/8/9 read high with pull-down due to on-board pull-ups) |
| Chip temperature | 37 °C with radio off, 53 °C after 15 s of AP at 19.5 dBm |
| Crystal vs PC clock, 150 s | **+182 ppm** (27.4 ms gained, 0.5 ms USB round trip) |

## Conclusion

A +182 ppm crystal shifts the 2.4 GHz carrier by about 445 kHz. Wi-Fi
allows ±20 ppm and Bluetooth LE ±50 ppm. This explains every symptom: other
receivers reject the board's Wi-Fi frames and only partly capture its BLE
advertising, and the board's BLE receiver misses incoming packets. The
ESP32-C3 has no supported crystal trimming, so the board must be replaced
(or its crystal load capacitors corrected). All other subsystems are healthy.

## Running the self-test

Flash `esp32-c3-board-diag`, open the serial port at 115200 baud
(DTR/RTS off) and send single characters:

| Key | Test |
|---|---|
| `i` | Chip info and reset reason |
| `m` | RAM test |
| `f` | Flash write/read test (erases 64 KB of the SPIFFS partition) |
| `g` | GPIO short test; disconnect external wiring first |
| `t` | `esp_timer` microseconds and chip temperature |
| `s` | Wi-Fi scan (BSSID, channel, RSSI) |
| `a` / `o` | Wi-Fi AP on at 19.5 dBm / radio off |

To measure the crystal, send `t` twice a few minutes apart and compare the
elapsed board time with the PC clock, using the sample with the lowest round
trip. A healthy board stays within about ±20 ppm; the PC clock limits the
accuracy to roughly the same amount.
