#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEScan.h>

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  BLEDevice::init("RXPROBE");
  BLEDevice::getScan()->setActiveScan(false);
}

void loop() {
  BLEScanResults results = BLEDevice::getScan()->start(8, false);
  int strongest = -127;
  for (int i = 0; i < results.getCount(); ++i) {
    strongest = max(strongest, results.getDevice(i).getRSSI());
  }
  if (Serial.availableForWrite() >= 80) {
    Serial.printf("RXPROBE received=%d strongest_rssi=%d\n", results.getCount(), strongest);
  }
  BLEDevice::getScan()->clearResults();
  delay(1000);
}
