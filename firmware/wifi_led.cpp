// Wi-Fi version of the LED controller. The board's BLE receiver does not
// hear any packets (see VALIDATION.md), while Wi-Fi reception works, so the
// phone connects to the board's own access point instead. No router needed.
#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <WiFi.h>

constexpr uint8_t LED_PIN = 5;
constexpr uint8_t BUTTON_PIN = 4;
constexpr uint32_t RELEASE_DEBOUNCE_MS = 20;

// Must match WifiLedConnection.java in the Android app.
constexpr char AP_SSID[] = "ESP32-LED";
constexpr char AP_PASSWORD[] = "esp32led";  // WPA2: at least 8 characters
constexpr uint16_t PORT = 4210;
// The app treats a silent link as lost after a few seconds.
constexpr uint32_t HEARTBEAT_MS = 2000;

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

WiFiServer server(PORT);
WiFiClient client;
uint8_t frame[4];
size_t frameLength = 0;
uint32_t lastSentMs = 0;

void showColor() {
  // Scale from the original RGB bytes each time, avoiding cumulative loss
  // when changing NeoPixel's global brightness repeatedly.
  led.setPixelColor(0, (uint16_t(currentRgb[0]) * brightness + 127) / 255,
                      (uint16_t(currentRgb[1]) * brightness + 127) / 255,
                      (uint16_t(currentRgb[2]) * brightness + 127) / 255);
  led.show();
}

// Protocol: 4-byte frames [R, G, B, brightness] in both directions.
void publishState() {
  if (!client.connected()) return;
  const uint8_t state[4] = {currentRgb[0], currentRgb[1], currentRgb[2], brightness};
  client.write(state, sizeof(state));
  lastSentMs = millis();
}

void serviceClient() {
  // A new connection replaces the old one: after the phone leaves the app
  // the previous socket may still look open for a while.
  WiFiClient incoming = server.accept();
  if (incoming) {
    client.stop();
    client = incoming;
    client.setNoDelay(true);
    frameLength = 0;
    publishState();
  }
  if (!client.connected()) return;

  bool changed = false;
  while (client.available() > 0) {
    const int value = client.read();
    if (value < 0) break;
    frame[frameLength++] = uint8_t(value);
    if (frameLength == sizeof(frame)) {
      frameLength = 0;
      memcpy(currentRgb, frame, 3);
      brightness = frame[3];
      changed = true;
    }
  }
  if (changed) {
    showColor();
    publishState();
  } else if (millis() - lastSentMs >= HEARTBEAT_MS) {
    publishState();
  }
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

  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  const bool apStarted = WiFi.softAP(AP_SSID, AP_PASSWORD, 6, 0, 1);  // channel 6, visible, one phone
  // ESP32-C3 Super Mini boards are often unstable at full Wi-Fi TX power.
  const bool powerSet = WiFi.setTxPower(WIFI_POWER_8_5dBm);
  server.begin();
  server.setNoDelay(true);
#if LED_DIAGNOSTICS
  Serial.printf("WIFI ap=%d power_set=%d\n", apStarted, powerSet);
#else
  (void)apStarted; (void)powerSet;
#endif
}

void loop() {
  serviceClient();

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
  // Change Wi-Fi TX power without reflashing: h = 19.5, m = 8.5, l = 2 dBm.
  if (Serial.available()) {
    const int command = Serial.read();
    if (command == 'h') WiFi.setTxPower(WIFI_POWER_19_5dBm);
    else if (command == 'm') WiFi.setTxPower(WIFI_POWER_8_5dBm);
    else if (command == 'l') WiFi.setTxPower(WIFI_POWER_2dBm);
  }
  static uint32_t lastDiagnosticMs = 0;
  if (now - lastDiagnosticMs >= 2000 && Serial.availableForWrite() >= 120) {
    lastDiagnosticMs = now;
    Serial.printf("WIFI ssid=%s ip=%s mac=%s channel=%d power_qdbm=%d stations=%d client=%d uptime=%lu\n",
      WiFi.softAPSSID().c_str(), WiFi.softAPIP().toString().c_str(), WiFi.softAPmacAddress().c_str(),
      WiFi.channel(), int(WiFi.getTxPower()), WiFi.softAPgetStationNum(), int(client.connected()),
      (unsigned long)now);
  }
#endif

  delay(1);
}
