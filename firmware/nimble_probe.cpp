#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <NimBLEDevice.h>

// Independent BLE implementation for diagnosing this board, excluded from the
// normal build. Uses the same app protocol and preserves LED/button operation.
namespace {
constexpr char SERVICE[] = "a7c00001-5a64-4b21-9d1c-49f39417a601";
constexpr char STATE[] = "a7c00002-5a64-4b21-9d1c-49f39417a601";
Adafruit_NeoPixel pixel(1, 5, NEO_GRB + NEO_KHZ800);
NimBLECharacteristic* characteristic;
portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
uint8_t state[4] = {255, 0, 0, 60};
uint8_t pending[4];
bool hasPending = false;
#if CONFIG_BT_NIMBLE_EXT_ADV
bool restartExtended = false;
class Connections : public NimBLEServerCallbacks {
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) override {
    portENTER_CRITICAL(&stateMux);
    restartExtended = true;
    portEXIT_CRITICAL(&stateMux);
  }
} connections;
#endif
const uint8_t colors[][3] = {
  {255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0},
  {0, 255, 255}, {255, 0, 255}, {255, 255, 255}
};
class Writes : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    uint8_t packet[4];
    portENTER_CRITICAL(&stateMux);
    memcpy(packet, state, 4);
    portEXIT_CRITICAL(&stateMux);
    c->setValue(packet, 4);
  }
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
    const auto value = c->getValue();
    if (value.size() != 4) { onRead(c, info); return; }
    portENTER_CRITICAL(&stateMux);
    memcpy(pending, value.data(), 4);
    hasPending = true;
    portEXIT_CRITICAL(&stateMux);
  }
} writes;
void apply(const uint8_t* packet) {
  portENTER_CRITICAL(&stateMux);
  memcpy(state, packet, 4);
  portEXIT_CRITICAL(&stateMux);
  pixel.setPixelColor(0, (uint16_t(packet[0]) * packet[3] + 127) / 255,
                        (uint16_t(packet[1]) * packet[3] + 127) / 255,
                        (uint16_t(packet[2]) * packet[3] + 127) / 255);
  pixel.show();
  if (characteristic) {
    characteristic->setValue(packet, 4);
    if (NimBLEDevice::getServer()->getConnectedCount()) characteristic->notify();
  }
}
}
void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  pinMode(4, INPUT_PULLUP);
  pixel.begin();
  const uint8_t initial[4] = {255, 0, 0, 60};
  apply(initial);
  NimBLEDevice::init("ESP32LED");
  NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_PUBLIC);
  NimBLEDevice::setPower(0);
  NimBLEServer* server = NimBLEDevice::createServer();
#if CONFIG_BT_NIMBLE_EXT_ADV
  server->setCallbacks(&connections);
#else
  server->advertiseOnDisconnect(true);
#endif
  NimBLEService* service = server->createService(SERVICE);
  characteristic = service->createCharacteristic(STATE,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY);
  characteristic->setValue(state, 4);
  characteristic->setCallbacks(&writes);
  server->start();
#if CONFIG_BT_NIMBLE_EXT_ADV
  // Bluetooth 5 allows connectable, non-scannable advertising: receiving a
  // scan response is no longer a prerequisite for the phone to discover us.
  NimBLEExtAdvertisement advertisement;
  advertisement.setName("C3EXT");
  advertisement.setCompleteServices(NimBLEUUID(SERVICE));
  advertisement.setFlags(0x06);
  advertisement.setLegacyAdvertising(false);
  advertisement.setConnectable(true);
  advertisement.setScannable(false);
  auto* advertising = NimBLEDevice::getAdvertising();
  advertising->setInstanceData(0, advertisement);
  advertising->start(0);
#else
  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->setName("ESP32LED");
  advertising->addServiceUUID(SERVICE);
  advertising->enableScanResponse(true);
  advertising->setConnectableMode(BLE_GAP_CONN_MODE_UND);
  advertising->setDiscoverableMode(BLE_GAP_DISC_MODE_GEN);
  advertising->start();
#endif
}
void loop() {
  uint8_t packet[4];
  portENTER_CRITICAL(&stateMux);
  const bool changed = hasPending;
  if (changed) { memcpy(packet, pending, 4); hasPending = false; }
#if CONFIG_BT_NIMBLE_EXT_ADV
  const bool restart = restartExtended;
  restartExtended = false;
#endif
  portEXIT_CRITICAL(&stateMux);
#if CONFIG_BT_NIMBLE_EXT_ADV
  if (restart) NimBLEDevice::getAdvertising()->start(0);
#endif
  if (changed) apply(packet);
  static bool pressed = digitalRead(4) == LOW;
  static uint32_t releaseSince = 0;
  static size_t color = 0;
  const uint32_t now = millis();
  if (digitalRead(4) == LOW) {
    releaseSince = now;
    if (!pressed) {
      pressed = true;
      color = (color + 1) % 7;
      memcpy(packet, colors[color], 3);
      packet[3] = state[3];
      apply(packet);
    }
  } else if (now - releaseSince >= 20) pressed = false;
  static uint32_t last = 0;
  if (now - last >= 2000 && Serial.availableForWrite() >= 96) {
    last = now;
    Serial.printf("NIMBLE advertising=%d connected=%u uptime=%lu\n",
      NimBLEDevice::getAdvertising()->isAdvertising(),
      NimBLEDevice::getServer()->getConnectedCount(), (unsigned long)now);
  }
  delay(1);
}
