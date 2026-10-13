// Board self-test driven over USB serial, one letter per test:
//   i  chip info        m  RAM test         f  flash write/read test
//   g  GPIO short test  t  time + temperature (crystal / heat checks)
//   s  Wi-Fi scan       a  Wi-Fi AP on      o  radio off
#include <Arduino.h>
#include <WiFi.h>
#include "driver/gpio.h"
#include "esp_chip_info.h"
#include "esp_partition.h"
#include "esp_system.h"

static void chipInfo() {
  esp_chip_info_t chip;
  esp_chip_info(&chip);
  Serial.printf("INFO model=%s revision=%u cores=%u features=0x%lx cpu_mhz=%lu xtal_mhz=%lu\n",
    ESP.getChipModel(), chip.revision, chip.cores, (unsigned long)chip.features,
    (unsigned long)getCpuFrequencyMhz(), (unsigned long)getXtalFrequencyMhz());
  Serial.printf("INFO flash=%lu speed=%lu heap=%lu max_block=%lu reset_reason=%d\n",
    (unsigned long)ESP.getFlashChipSize(), (unsigned long)ESP.getFlashChipSpeed(),
    (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMaxAllocHeap(), int(esp_reset_reason()));
}

static void ramTest() {
  const size_t size = ESP.getMaxAllocHeap() - 1024;
  uint32_t *block = static_cast<uint32_t *>(malloc(size));
  if (!block) { Serial.println("RAM alloc_failed"); return; }
  const size_t words = size / 4;
  size_t errors = 0;
  const uint32_t patterns[] = {0x00000000, 0xFFFFFFFF, 0xAAAAAAAA, 0x55555555};
  for (uint32_t pattern : patterns) {
    for (size_t i = 0; i < words; ++i) block[i] = pattern ^ i;
    for (size_t i = 0; i < words; ++i) if (block[i] != (pattern ^ i)) ++errors;
  }
  free(block);
  Serial.printf("RAM tested_bytes=%u errors=%u\n", unsigned(words * 4), unsigned(errors));
}

static void flashTest() {
  // Uses the unused SPIFFS data partition; nothing in this project stores data there.
  const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                         ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
  if (!part) { Serial.println("FLASH no_spiffs_partition"); return; }
  const size_t length = 64 * 1024;
  static uint8_t buffer[4096];
  esp_err_t erased = esp_partition_erase_range(part, 0, length);
  size_t errors = 0;
  for (size_t offset = 0; offset < length && erased == ESP_OK; offset += sizeof(buffer)) {
    for (size_t i = 0; i < sizeof(buffer); ++i) buffer[i] = uint8_t((offset + i) * 31 + 7);
    esp_partition_write(part, offset, buffer, sizeof(buffer));
  }
  for (size_t offset = 0; offset < length && erased == ESP_OK; offset += sizeof(buffer)) {
    esp_partition_read(part, offset, buffer, sizeof(buffer));
    for (size_t i = 0; i < sizeof(buffer); ++i) if (buffer[i] != uint8_t((offset + i) * 31 + 7)) ++errors;
  }
  esp_partition_erase_range(part, 0, length);
  Serial.printf("FLASH partition_at=0x%lx tested_bytes=%u erase=%s errors=%u\n",
    (unsigned long)part->address, unsigned(length), esp_err_to_name(erased), unsigned(errors));
}

static void gpioTest() {
  // GPIO11-17 belong to the internal flash, GPIO18/19 to USB.
  const gpio_num_t pins[] = {GPIO_NUM_0, GPIO_NUM_1, GPIO_NUM_2, GPIO_NUM_3, GPIO_NUM_4,
                             GPIO_NUM_5, GPIO_NUM_6, GPIO_NUM_7, GPIO_NUM_8, GPIO_NUM_9,
                             GPIO_NUM_10, GPIO_NUM_20, GPIO_NUM_21};
  for (gpio_num_t pin : pins) {
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY);   delayMicroseconds(200);
    const int pullUp = gpio_get_level(pin);
    gpio_set_pull_mode(pin, GPIO_PULLDOWN_ONLY); delayMicroseconds(200);
    const int pullDown = gpio_get_level(pin);
    // Drive briefly: a pin that cannot follow its own output is shorted.
    gpio_set_pull_mode(pin, GPIO_FLOATING);
    gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_level(pin, 1); delayMicroseconds(50);
    const int driveHigh = gpio_get_level(pin);
    gpio_set_level(pin, 0); delayMicroseconds(50);
    const int driveLow = gpio_get_level(pin);
    gpio_reset_pin(pin);
    Serial.printf("GPIO pin=%d pullup=%d pulldown=%d drive1=%d drive0=%d %s\n", int(pin),
      pullUp, pullDown, driveHigh, driveLow,
      (pullUp == 1 && pullDown == 0 && driveHigh == 1 && driveLow == 0) ? "ok" : "CHECK");
  }
}

static void timeAndTemperature() {
  Serial.printf("TIME us=%llu temp_c=%.1f\n", (unsigned long long)esp_timer_get_time(), temperatureRead());
}

static void wifiScan() {
  if (WiFi.getMode() != WIFI_STA) { WiFi.mode(WIFI_STA); WiFi.disconnect(false, false); }
  const int count = WiFi.scanNetworks(false, true, true, 400);
  for (int i = 0; i < count; ++i)
    Serial.printf("SCAN bssid=%s channel=%ld rssi=%ld\n", WiFi.BSSIDstr(i).c_str(),
      (long)WiFi.channel(i), (long)WiFi.RSSI(i));
  Serial.printf("SCAN done count=%d\n", count);
  WiFi.scanDelete();
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  WiFi.persistent(false);
  WiFi.mode(WIFI_OFF);
}

void loop() {
  if (!Serial.available()) { delay(1); return; }
  switch (Serial.read()) {
    case 'i': chipInfo(); break;
    case 'm': ramTest(); break;
    case 'f': flashTest(); break;
    case 'g': gpioTest(); break;
    case 't': timeAndTemperature(); break;
    case 's': wifiScan(); break;
    case 'a':
      WiFi.mode(WIFI_AP);
      WiFi.softAP("C3DIAG", "esp32diag", 6);
      WiFi.setTxPower(WIFI_POWER_19_5dBm);
      Serial.println("RADIO ap_on");
      break;
    case 'o': WiFi.mode(WIFI_OFF); Serial.println("RADIO off"); break;
  }
}
