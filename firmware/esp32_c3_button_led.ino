#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#if LED_STANDALONE_NIMBLE
#include <NimBLEDevice.h>
#else
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLE2902.h>
#endif

constexpr uint8_t LED_PIN = 5;
constexpr uint8_t BUTTON_PIN = 4;
constexpr uint32_t RELEASE_DEBOUNCE_MS = 20;

Adafruit_NeoPixel led(1, LED_PIN, NEO_GRB + NEO_KHZ800);

const uint8_t colors[][3] = {
  {255,   0,   0}, // Красный
  {  0, 255,   0}, // Зелёный
  {  0,   0, 255}, // Синий
  {255, 255,   0}, // Жёлтый
  {  0, 255, 255}, // Голубой
  {255,   0, 255}, // Фиолетовый
  {255, 255, 255}  // Белый
};

constexpr size_t COLOR_COUNT = sizeof(colors) / sizeof(colors[0]);
size_t colorIndex = 0;
int lastReading = HIGH;
uint32_t lastChangeMs = 0;
bool buttonLatched = false;
uint8_t currentRgb[3] = {255, 0, 0};
uint8_t brightness = 60;
BLECharacteristic *stateCharacteristic = nullptr;
portMUX_TYPE commandMux = portMUX_INITIALIZER_UNLOCKED;
uint8_t pendingCommand[4];
uint8_t reportedState[4] = {255, 0, 0, 60};
bool commandPending = false;
bool connected = false;
bool restartAdvertising = false;
constexpr char SERVICE_UUID[] = "a7c00001-5a64-4b21-9d1c-49f39417a601";
constexpr char STATE_UUID[] = "a7c00002-5a64-4b21-9d1c-49f39417a601";

void startLEDAdvertising() {
#if LED_STANDALONE_NIMBLE
  const bool started = BLEDevice::startAdvertising(0);
#if LED_DIAGNOSTICS
  Serial.printf("BLE5 advertising start=%d\n", started);
#endif
#else
  BLEDevice::startAdvertising();
#endif
}

void setLEDTransmitPower(esp_power_level_t power) {
#if LED_STANDALONE_NIMBLE
  BLEDevice::setPowerLevel(power);
  BLEDevice::setPowerLevel(power, ESP_BLE_PWR_TYPE_ADV);
#else
  BLEDevice::setPower(power);
  BLEDevice::setPower(power, ESP_BLE_PWR_TYPE_ADV);
#endif
}

class LedServerCallbacks : public BLEServerCallbacks {
#if LED_STANDALONE_NIMBLE
  void onConnect(BLEServer *, NimBLEConnInfo &) override {
#else
  void onConnect(BLEServer *) override {
#endif
    portENTER_CRITICAL(&commandMux);
    connected = true;
    portEXIT_CRITICAL(&commandMux);
  }
#if LED_STANDALONE_NIMBLE
  void onDisconnect(BLEServer *, NimBLEConnInfo &, int) override {
#else
  void onDisconnect(BLEServer *) override {
#endif
    portENTER_CRITICAL(&commandMux);
    connected = false;
    restartAdvertising = true;
    portEXIT_CRITICAL(&commandMux);
  }
};

class LedWriteCallbacks : public BLECharacteristicCallbacks {
#if LED_STANDALONE_NIMBLE
  void onRead(BLECharacteristic *characteristic, NimBLEConnInfo &) override {
#else
  void onRead(BLECharacteristic *characteristic) override {
#endif
    uint8_t state[4];
    portENTER_CRITICAL(&commandMux);
    memcpy(state, reportedState, sizeof(state));
    portEXIT_CRITICAL(&commandMux);
    characteristic->setValue(state, sizeof(state));
  }
#if LED_STANDALONE_NIMBLE
  void onWrite(BLECharacteristic *characteristic, NimBLEConnInfo &info) override {
#else
  void onWrite(BLECharacteristic *characteristic) override {
#endif
    const auto value = characteristic->getValue();
    if (value.length() != 4) {
#if LED_STANDALONE_NIMBLE
      onRead(characteristic, info);
#else
      onRead(characteristic);
#endif
      return;
    }
    portENTER_CRITICAL(&commandMux);
    memcpy(pendingCommand, value.c_str(), 4);
    commandPending = true;
    portEXIT_CRITICAL(&commandMux);
  }
};

void publishState() {
  if (!stateCharacteristic) return;
  uint8_t state[4] = {currentRgb[0], currentRgb[1], currentRgb[2], brightness};
  portENTER_CRITICAL(&commandMux);
  memcpy(reportedState, state, sizeof(state));
  portEXIT_CRITICAL(&commandMux);
  stateCharacteristic->setValue(state, sizeof(state));
  portENTER_CRITICAL(&commandMux);
  const bool notifyClient = connected;
  portEXIT_CRITICAL(&commandMux);
  if (notifyClient) stateCharacteristic->notify();
}

void setupBluetooth() {
#if LED_STANDALONE_NIMBLE
  const bool initialized = BLEDevice::init("ESP32 LED");
#if LED_DIAGNOSTICS
  Serial.printf("BLE5 init=%d\n", initialized);
#endif
  if (!initialized) return;
#else
  BLEDevice::init("ESP32 LED");
#endif
#if LED_STANDALONE_NIMBLE
  BLEDevice::setOwnAddrType(BLE_OWN_ADDR_PUBLIC);
  setLEDTransmitPower(ESP_PWR_LVL_N0);
#else
  setLEDTransmitPower(ESP_PWR_LVL_P9);
#endif
  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new LedServerCallbacks());
  BLEService *service = server->createService(SERVICE_UUID);
  stateCharacteristic = service->createCharacteristic(
#if LED_STANDALONE_NIMBLE
    STATE_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY);
#else
    STATE_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE |
                BLECharacteristic::PROPERTY_NOTIFY);
#endif
#if defined(CONFIG_BLUEDROID_ENABLED)
  stateCharacteristic->addDescriptor(new BLE2902());
#endif
  stateCharacteristic->setCallbacks(new LedWriteCallbacks());
  publishState();
#if LED_STANDALONE_NIMBLE
  const bool serverStarted = server->start();
#if LED_DIAGNOSTICS
  Serial.printf("BLE5 server start=%d\n", serverStarted);
#endif
  if (!serverStarted) return;
  // Connectable Bluetooth 5 advertising without a scan response.
#if LED_CODED_PHY
  NimBLEExtAdvertisement advertisement(BLE_HCI_LE_PHY_CODED, BLE_HCI_LE_PHY_CODED);
#else
  NimBLEExtAdvertisement advertisement;
#endif
  advertisement.setName("ESP32LED");
  advertisement.setCompleteServices(NimBLEUUID(SERVICE_UUID));
  advertisement.setFlags(0x06);
  advertisement.setLegacyAdvertising(false);
  advertisement.setConnectable(true);
  advertisement.setScannable(false);
  const bool configured = BLEDevice::getAdvertising()->setInstanceData(0, advertisement);
#if LED_DIAGNOSTICS
  Serial.printf("BLE5 configured=%d\n", configured);
#endif
  if (!configured) return;
#else
  service->start();
  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  // Keep a complete name in a short primary packet, visible without receiving
  // a scan response. Put the 128-bit service UUID in the scan response.
  BLEAdvertisementData advertisement;
#if LED_VISIBILITY_PROBE
  // Non-connectable, non-scannable beacon: tests reception on the phone
  // without requiring the ESP32 to receive a scan request or respond to one.
  advertisement.setFlags(0x04);
  advertisement.setName("C3TEST");
#else
  advertisement.setFlags(0x06);
  advertisement.setName("ESP32LED");
#endif
  advertising->setAdvertisementData(advertisement);
  BLEAdvertisementData scanResponse;
  scanResponse.setCompleteServices(BLEUUID(SERVICE_UUID));
  scanResponse.setName("ESP32 LED");
  advertising->setScanResponseData(scanResponse);
#if defined(CONFIG_NIMBLE_ENABLED)
#if LED_VISIBILITY_PROBE
  advertising->setAdvertisementType(BLE_GAP_CONN_MODE_NON);
#else
  advertising->setAdvertisementType(BLE_GAP_CONN_MODE_UND);
#endif
#else
#if LED_VISIBILITY_PROBE
  advertising->setAdvertisementType(ADV_TYPE_NONCONN_IND);
#else
  advertising->setAdvertisementType(ADV_TYPE_IND);
#endif
#endif
  advertising->setScanFilter(false, false);
#if LED_VISIBILITY_PROBE
  advertising->setScanResponse(false);
#else
  advertising->setScanResponse(true);
#endif
#endif // LED_STANDALONE_NIMBLE
  startLEDAdvertising();
}

void showColor() {
  // Scale from the original RGB bytes each time, avoiding cumulative loss
  // when changing NeoPixel's global brightness repeatedly.
  led.setPixelColor(0, (uint16_t(currentRgb[0]) * brightness + 127) / 255,
                      (uint16_t(currentRgb[1]) * brightness + 127) / 255,
                      (uint16_t(currentRgb[2]) * brightness + 127) / 255);
  led.show();
}

void setup() {
#if LED_DIAGNOSTICS
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
#endif
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  led.begin();
  showColor();           // Горит красным сразу после запуска

  lastReading = digitalRead(BUTTON_PIN);
  buttonLatched = (lastReading == LOW);
  lastChangeMs = millis();
  setupBluetooth();
}

void loop() {
  uint8_t command[4];
  portENTER_CRITICAL(&commandMux);
  const bool applyCommand = commandPending;
  if (applyCommand) {
    memcpy(command, pendingCommand, sizeof(command));
    commandPending = false;
  }
  const bool advertiseAgain = restartAdvertising;
  restartAdvertising = false;
  portEXIT_CRITICAL(&commandMux);
  if (advertiseAgain) startLEDAdvertising();
  if (applyCommand) {
    memcpy(currentRgb, command, 3);
    brightness = command[3];
    showColor();
    publishState();
  }

  const int reading = digitalRead(BUTTON_PIN);
  const uint32_t now = millis();

  if (reading != lastReading) {
    lastReading = reading;
    lastChangeMs = now;
  }

  // Respond immediately; rearm only after a stable release.
  if (reading == LOW && !buttonLatched) {
    buttonLatched = true;
    colorIndex = (colorIndex + 1) % COLOR_COUNT;
    memcpy(currentRgb, colors[colorIndex], sizeof(currentRgb));
    showColor();
    publishState();
  } else if (reading == HIGH && now - lastChangeMs >= RELEASE_DEBOUNCE_MS) {
    buttonLatched = false;
  }

  // Repeat the current color in case LED power stabilized after setup().
  static uint32_t lastRefreshMs = 0;
  if (now - lastRefreshMs >= 500) {
    lastRefreshMs = now;
    showColor();
  }

#if LED_DIAGNOSTICS
  // Change RF power without reflashing while investigating connection failures.
  if (Serial.available()) {
    const int command = Serial.read();
    if (command == '0' || command == '-' || command == '+') {
      const esp_power_level_t power = command == '0' ? ESP_PWR_LVL_N0 :
                                     command == '-' ? ESP_PWR_LVL_N12 : ESP_PWR_LVL_P9;
      setLEDTransmitPower(power);
      Serial.printf("BLE power=%d dBm\n", command == '0' ? 0 : command == '-' ? -12 : 9);
    }
  }
  static uint32_t lastDiagnosticMs = 0;
  if (now - lastDiagnosticMs >= 2000 && Serial.availableForWrite() >= 96) {
    lastDiagnosticMs = now;
    portENTER_CRITICAL(&commandMux);
    const bool clientConnected = connected;
    portEXIT_CRITICAL(&commandMux);
#if LED_STANDALONE_NIMBLE
    Serial.printf("BLE5 advertising=%d connected=%d count=%u uptime=%lu\n",
      BLEDevice::getAdvertising()->isAdvertising(), clientConnected,
      BLEDevice::getServer()->getConnectedCount(), (unsigned long)now);
#else
    Serial.printf("BLE connected=%d uptime=%lu\n", clientConnected, (unsigned long)now);
#endif
  }
#endif

  delay(1);
}
