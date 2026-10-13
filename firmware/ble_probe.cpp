#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>

// Diagnostic-only firmware; the normal environment excludes this file.
constexpr char PROBE_SERVICE[] = "a7c00001-5a64-4b21-9d1c-49f39417a601";
constexpr char PROBE_STATE[] = "a7c00002-5a64-4b21-9d1c-49f39417a601";

class ProbeCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *) override { Serial.println("PROBE connected"); }
  void onDisconnect(BLEServer *) override {
    Serial.println("PROBE disconnected");
    BLEDevice::startAdvertising();
  }
};

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  BLEDevice::init("ESP32LED");
  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new ProbeCallbacks());
  BLEService *service = server->createService(PROBE_SERVICE);
  BLECharacteristic *state = service->createCharacteristic(PROBE_STATE,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
  uint8_t value[] = {255, 0, 0, 60};
  state->setValue(value, sizeof(value));
  service->start();
  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(PROBE_SERVICE);
  advertising->setScanResponse(true);
  BLEDevice::startAdvertising();
}

void loop() {
  static uint32_t last = 0;
  if (millis() - last >= 2000 && Serial.availableForWrite() >= 80) {
    last = millis();
    Serial.printf("PROBE advertising=%d uptime=%lu\n",
      BLEDevice::getAdvertising()->isAdvertising(), (unsigned long)last);
  }
  delay(10);
}
