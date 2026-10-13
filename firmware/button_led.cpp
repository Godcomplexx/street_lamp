// Button-only LED controller: no Wi-Fi or Bluetooth. For boards whose radio
// does not work (see VALIDATION.md).
//   short press  -> next color (on release)
//   hold         -> brightness ramps; each new hold reverses the direction
// Color and brightness survive power loss.
#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <Preferences.h>

constexpr uint8_t LED_PIN = 5;
constexpr uint8_t BUTTON_PIN = 4;
constexpr uint32_t DEBOUNCE_MS = 20;
constexpr uint32_t HOLD_MS = 400;        // longer than this is a hold, not a press
constexpr uint32_t RAMP_STEP_MS = 20;
constexpr uint8_t MIN_BRIGHTNESS = 3;
constexpr uint32_t SAVE_DELAY_MS = 2000; // limits flash writes while adjusting

Adafruit_NeoPixel led(1, LED_PIN, NEO_GRB + NEO_KHZ800);
Preferences settings;

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
uint8_t brightness = 60;
bool rampUp = true;

bool pressed = false;        // debounced state
int lastReading = HIGH;
uint32_t lastChangeMs = 0;
uint32_t pressStartMs = 0;
uint32_t lastRampMs = 0;
bool holding = false;
bool saveNeeded = false;
uint32_t lastEditMs = 0;

void showColor() {
  const uint8_t *rgb = colors[colorIndex];
  led.setPixelColor(0, (uint16_t(rgb[0]) * brightness + 127) / 255,
                      (uint16_t(rgb[1]) * brightness + 127) / 255,
                      (uint16_t(rgb[2]) * brightness + 127) / 255);
  led.show();
}

void edited(uint32_t now) {
  showColor();
  saveNeeded = true;
  lastEditMs = now;
}

// Steps proportional to the current level look even to the eye:
// about 2 seconds from the dimmest to the brightest.
void rampBrightness(uint32_t now) {
  const int step = max(1, brightness / 20);
  const int next = rampUp ? min(255, brightness + step) : max(int(MIN_BRIGHTNESS), brightness - step);
  if (next != brightness) {
    brightness = uint8_t(next);
    edited(now);
  }
}

void setup() {
  // Nothing here needs full speed; a slower CPU keeps the chip cooler.
  setCpuFrequencyMhz(80);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  settings.begin("led", false);
  colorIndex = settings.getUChar("color", 0) % COLOR_COUNT;
  brightness = max(MIN_BRIGHTNESS, settings.getUChar("bright", 60));

  led.begin();
  showColor();

  lastReading = digitalRead(BUTTON_PIN);
  // A button held during power-up counts as pressed from this moment.
  pressed = (lastReading == LOW);
  lastChangeMs = pressStartMs = millis();
}

void loop() {
  const int reading = digitalRead(BUTTON_PIN);
  const uint32_t now = millis();

  if (reading != lastReading) {
    lastReading = reading;
    lastChangeMs = now;
  }

  if (now - lastChangeMs >= DEBOUNCE_MS && (reading == LOW) != pressed) {
    pressed = (reading == LOW);
    if (pressed) {
      pressStartMs = now;
    } else {
      if (holding) rampUp = !rampUp;          // next hold goes the other way
      else {
        colorIndex = (colorIndex + 1) % COLOR_COUNT;
        edited(now);
      }
      holding = false;
    }
  }

  if (pressed && !holding && now - pressStartMs >= HOLD_MS) {
    holding = true;
    lastRampMs = now - RAMP_STEP_MS;
  }
  if (pressed && holding && now - lastRampMs >= RAMP_STEP_MS) {
    lastRampMs = now;
    rampBrightness(now);
  }

  if (saveNeeded && now - lastEditMs >= SAVE_DELAY_MS) {
    settings.putUChar("color", uint8_t(colorIndex));
    settings.putUChar("bright", brightness);
    saveNeeded = false;
  }

  // Repeat the current color in case LED power stabilized after setup().
  static uint32_t lastRefreshMs = 0;
  if (now - lastRefreshMs >= 500) {
    lastRefreshMs = now;
    showColor();
  }

  delay(1);
}
