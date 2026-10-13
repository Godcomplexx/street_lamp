#include <Arduino.h>
#include <WiFi.h>

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, false);
}

void loop() {
  // Passive reception only; no network is joined and no names are logged.
  const int count = WiFi.scanNetworks(false, true, true, 300);
  int strongest = -127;
  for (int i = 0; i < count; ++i) strongest = max(strongest, static_cast<int>(WiFi.RSSI(i)));
  if (Serial.availableForWrite() >= 80) {
    Serial.printf("WIFIPROBE received=%d strongest_rssi=%d\n", count, strongest);
  }
  WiFi.scanDelete();
  delay(1000);
}
