// Button-only LED controller: no Wi-Fi or Bluetooth. For boards whose radio
// does not work (see VALIDATION.md).
//   short press  -> next color (on release)
//   hold         -> brightness ramps; each new hold reverses the direction
//   wheel        -> brightness up / down, one step per notch
// Color and brightness survive power loss.
#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <Preferences.h>

constexpr uint8_t LED_PIN = 5;
constexpr uint8_t BUTTON_PIN = 4;
// Three-contact mouse wheel: A and B to these pins, common contact to GND.
// If scrolling "up" dims the light, swap the A and B wires.
constexpr uint8_t WHEEL_A_PIN = 6;
constexpr uint8_t WHEEL_B_PIN = 7;
constexpr uint32_t WHEEL_DEBOUNCE_MS = 12;
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

uint8_t wheelRaw = 0;        // last sampled A/B levels (bit1 = A, bit0 = B)
uint8_t wheelStable = 0;     // last accepted A/B state
uint32_t wheelChangedMs = 0;
int wheelPosition = 0;       // for diagnostics only

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

uint8_t readWheel() {
  return uint8_t((digitalRead(WHEEL_A_PIN) << 1) | digitalRead(WHEEL_B_PIN));
}

// A mouse wheel is not a regular quadrature encoder: it cycles through three
// states, 10 -> 11 -> 01 -> 10, one per notch (00 never rests), and the
// 01 -> 10 step flips both contacts at once. Index: (accepted << 2) | new.
const int8_t wheelSteps[16] = {
  0, 0, 0, 0,
  0, 0, 1, -1,
  0, -1, 0, 1,
  0, 1, -1, 0,
};

void pollWheel(uint32_t now) {
  const uint8_t sample = readWheel();
  if (sample != wheelRaw) {
    wheelRaw = sample;
    wheelChangedMs = now;
  }
  if (wheelRaw == wheelStable || wheelRaw == 0 || now - wheelChangedMs < WHEEL_DEBOUNCE_MS) return;
  const int step = wheelSteps[(wheelStable << 2) | wheelRaw];
  wheelStable = wheelRaw;
  if (step == 0) return;
  wheelPosition += step;
  // One notch changes brightness by about a fifth: ~25 notches from dim to full.
  const int level = step > 0 ? min(255, brightness + max(1, brightness / 5))
                             : max(int(MIN_BRIGHTNESS), brightness - max(1, brightness / 6));
  if (level != brightness) {
    brightness = uint8_t(level);
    edited(now);
  }
}

#if LED_DIAGNOSTICS
// Send 'r' over USB serial for the current wheel state.
void diagnosticsLoop() {
  if (!Serial.available() || Serial.read() != 'r') return;
  Serial.printf("DIAG wheel_ab=%u%u position=%d brightness=%u color=%u\n",
    (wheelStable >> 1) & 1, wheelStable & 1, wheelPosition, brightness, unsigned(colorIndex));
}
#endif

void setup() {
  // Nothing here needs full speed; a slower CPU keeps the chip cooler.
  setCpuFrequencyMhz(80);
#if LED_DIAGNOSTICS
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
#endif
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(WHEEL_A_PIN, INPUT_PULLUP);
  pinMode(WHEEL_B_PIN, INPUT_PULLUP);
  wheelRaw = wheelStable = readWheel();

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

  pollWheel(now);
#if LED_DIAGNOSTICS
  diagnosticsLoop();
#endif

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
