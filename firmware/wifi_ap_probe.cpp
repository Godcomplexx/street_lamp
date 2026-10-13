// Minimal access point through the ESP-IDF Wi-Fi driver, bypassing the
// Arduino WiFi library: open network "C3OPEN", channel 1, 802.11b only.
// Checks whether anyone can receive this board's Wi-Fi transmissions.
#include <Arduino.h>
#include <string.h>
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

static esp_err_t step(const char *name, esp_err_t result) {
  Serial.printf("APPROBE %s=%s\n", name, esp_err_to_name(result));
  return result;
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  delay(1500);
  esp_err_t nvs = nvs_flash_init();
  if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs = nvs_flash_init();
  }
  step("nvs", nvs);
  step("netif", esp_netif_init());
  step("event_loop", esp_event_loop_create_default());
  esp_netif_create_default_wifi_ap();
  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
  step("init", esp_wifi_init(&init));
  step("storage", esp_wifi_set_storage(WIFI_STORAGE_RAM));
  step("mode", esp_wifi_set_mode(WIFI_MODE_AP));

  wifi_config_t config = {};
  strcpy(reinterpret_cast<char *>(config.ap.ssid), "C3OPEN");
  config.ap.ssid_len = strlen("C3OPEN");
  config.ap.channel = 1;
  config.ap.authmode = WIFI_AUTH_OPEN;
  config.ap.max_connection = 1;
  config.ap.beacon_interval = 100;
  step("config", esp_wifi_set_config(WIFI_IF_AP, &config));
  step("protocol_11b", esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11B));
  step("start", esp_wifi_start());
  step("power", esp_wifi_set_max_tx_power(52));  // 13 dBm, in 0.25 dBm units
}

void loop() {
  wifi_config_t config = {};
  esp_wifi_get_config(WIFI_IF_AP, &config);
  uint8_t primary = 0;
  wifi_second_chan_t secondary;
  esp_wifi_get_channel(&primary, &secondary);
  int8_t power = 0;
  esp_wifi_get_max_tx_power(&power);
  uint8_t mac[6] = {};
  esp_wifi_get_mac(WIFI_IF_AP, mac);
  wifi_sta_list_t stations = {};
  esp_wifi_ap_get_sta_list(&stations);
  if (Serial.availableForWrite() >= 120) {
    Serial.printf("APPROBE ssid=%s channel=%u power_qdbm=%d mac=%02x:%02x:%02x:%02x:%02x:%02x stations=%d uptime=%lu\n",
      reinterpret_cast<const char *>(config.ap.ssid), primary, power,
      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], stations.num, millis());
  }
  delay(2000);
}
