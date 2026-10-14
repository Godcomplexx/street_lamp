// Offline LED controller: no Wi-Fi or Bluetooth. For boards whose radio
// does not work (see VALIDATION.md).
//   short press  -> next page: COLOR, BRIGHTNESS, LIGHT MODE, TIME ZONE
//   wheel        -> changes the setting of the current page
//   hold         -> white light on / off
// LIGHT MODE "Auto" turns the lamp on between sunset and sunrise, computed
// from GPS position and time. An optional SSD1306 OLED shows the pages;
// without it, a short press blinks the page number (1-4).
// All settings and the last GPS position survive power loss.
#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <Preferences.h>
#include <TinyGPS++.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <math.h>

constexpr uint8_t LED_PIN = 5;
constexpr uint8_t BUTTON_PIN = 4;
// Three-contact mouse wheel: A and B to these pins, common contact to GND.
// If scrolling "up" turns the color the other way, swap the A and B wires.
constexpr uint8_t WHEEL_A_PIN = 6;
constexpr uint8_t WHEEL_B_PIN = 7;
// 0.96" I2C OLED, powered from 3V3.
constexpr uint8_t OLED_SDA_PIN = 3;
constexpr uint8_t OLED_SCL_PIN = 10;
// NEO-6M GPS, powered from 5V: module TX -> GPS_RX_PIN, module RX -> GPS_TX_PIN.
constexpr uint8_t GPS_RX_PIN = 20;
constexpr uint8_t GPS_TX_PIN = 21;
constexpr uint32_t GPS_BAUD = 9600;

constexpr uint32_t WHEEL_DEBOUNCE_MS = 12;
constexpr uint16_t HUE_STEP = 65536 / 48;   // 48 notches per full circle
constexpr uint32_t FAST_SCROLL_MS = 40;     // notches closer than this move 3x
constexpr uint32_t DEBOUNCE_MS = 20;
constexpr uint32_t HOLD_MS = 600;           // longer than this is a hold, not a press
constexpr uint8_t MIN_BRIGHTNESS = 3;
constexpr uint32_t SAVE_DELAY_MS = 2000;    // limits flash writes while adjusting
constexpr uint32_t BLINK_MS = 120;
constexpr uint32_t DRAW_INTERVAL_MS = 30;
constexpr uint32_t SCREEN_OFF_MS = 60000;   // protects the OLED from burn-in
constexpr uint32_t PREVIEW_MS = 10000;      // a lamp switched off by schedule lights up while adjusted
constexpr uint32_t SCHEDULE_MS = 10000;
constexpr double SUNSET_ALTITUDE = -0.833;  // degrees: refraction plus the sun's radius

Adafruit_NeoPixel led(1, LED_PIN, NEO_GRB + NEO_KHZ800);
Preferences settings;
U8G2_SSD1306_128X64_NONAME_F_HW_I2C screen(U8G2_R0, U8X8_PIN_NONE, OLED_SCL_PIN, OLED_SDA_PIN);
TinyGPSPlus gps;

enum class Page : uint8_t { Color, Brightness, LightMode, TimeZone };
constexpr uint8_t PAGE_COUNT = 4;
enum class LightMode : uint8_t { AlwaysOn, Auto, Off };
constexpr uint8_t LIGHT_MODE_COUNT = 3;

uint16_t hue = 0;            // 0..65535 around the circle, 0 = red
bool white = false;
uint8_t brightness = 60;
LightMode lightMode = LightMode::AlwaysOn;
int8_t utcOffsetHours = 3;
Page page = Page::Color;

bool pressed = false;        // debounced state
int lastReading = HIGH;
uint32_t lastChangeMs = 0;
uint32_t pressStartMs = 0;
bool holdHandled = false;
bool saveNeeded = false;
uint32_t lastEditMs = 0;
uint32_t lastInputMs = 0;

uint8_t blinksLeft = 0;      // off/on phases still to show
uint32_t blinkPhaseMs = 0;

uint8_t screenAddress = 0;   // 0 when no display answers
bool screenDirty = true;
bool screenOn = true;
uint32_t lastDrawMs = 0;

// Time: the GPS sets it; between fixes the board's own clock keeps counting.
bool timeKnown = false;
int64_t utcAtSync = 0;       // Unix seconds at syncMs
uint32_t syncMs = 0;
bool locationKnown = false;
float latitude = 0, longitude = 0;
bool locationSavedThisBoot = false;
bool sunUp = false;          // last schedule result, valid when timeKnown && locationKnown
uint32_t lastScheduleMs = 0;
int lastShownMinute = -1;

// The wheel is polled by its own task so that a screen redraw (~25 ms)
// cannot swallow notches. The loop picks up the accumulated steps.
portMUX_TYPE wheelMux = portMUX_INITIALIZER_UNLOCKED;
int pendingSteps = 0;        // one per notch
int pendingHueSteps = 0;     // same, but tripled while scrolling fast
uint8_t wheelStable = 0;     // last accepted A/B state (bit1 = A, bit0 = B)
int wheelPosition = 0;       // for diagnostics only

// ---------- Calendar and sun ----------

// Days since 1970-01-01 for a proleptic Gregorian date (H. Hinnant's algorithm).
int32_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const int yoe = y - era * 400;
  const int doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

void civilFromDays(int32_t z, int &y, int &m, int &d) {
  z += 719468;
  const int era = (z >= 0 ? z : z - 146096) / 146097;
  const int doe = z - era * 146097;
  const int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y = yoe + era * 400 + (m <= 2);
}

int64_t utcNow() {
  return utcAtSync + int64_t(millis() - syncMs) / 1000;
}

int64_t localNow() {
  return utcNow() + int64_t(utcOffsetHours) * 3600;
}

double toRadians(double angle) { return angle * M_PI / 180.0; }
double toDegrees(double angle) { return angle * 180.0 / M_PI; }

// Solar altitude in degrees (NOAA low-precision formulas, ~0.01 degree).
double sunAltitude(int64_t unixSeconds, double lat, double lon) {
  const double d = (unixSeconds - 946728000LL) / 86400.0;  // days since J2000.0
  const double g = toRadians(fmod(357.529 + 0.98560028 * d, 360.0));
  const double q = fmod(280.459 + 0.98564736 * d, 360.0);
  const double ecliptic = toRadians(q + 1.915 * sin(g) + 0.020 * sin(2 * g));
  const double tilt = toRadians(23.439 - 0.00000036 * d);
  const double rightAscension = atan2(cos(tilt) * sin(ecliptic), cos(ecliptic));
  const double declination = asin(sin(tilt) * sin(ecliptic));
  const double siderealHours = fmod(18.697374558 + 24.06570982441908 * d, 24.0);
  const double hourAngle = toRadians(siderealHours * 15.0 + lon) - rightAscension;
  return toDegrees(asin(sin(toRadians(lat)) * sin(declination) +
                        cos(toRadians(lat)) * cos(declination) * cos(hourAngle)));
}

// Sunrise or sunset in UTC minutes for a day of the year ("Almanac for
// Computers" method). Returns -1 when the sun does not cross the horizon.
int sunEventUtcMinutes(int dayOfYear, double lat, double lon, bool rising) {
  const double lonHours = lon / 15.0;
  const double t = dayOfYear + ((rising ? 6.0 : 18.0) - lonHours) / 24.0;
  const double meanAnomaly = 0.9856 * t - 3.289;
  double trueLongitude = meanAnomaly + 1.916 * sin(toRadians(meanAnomaly)) +
                         0.020 * sin(toRadians(2 * meanAnomaly)) + 282.634;
  trueLongitude = fmod(trueLongitude + 360.0, 360.0);
  double ra = fmod(toDegrees(atan(0.91764 * tan(toRadians(trueLongitude)))) + 360.0, 360.0);
  ra += floor(trueLongitude / 90.0) * 90.0 - floor(ra / 90.0) * 90.0;
  ra /= 15.0;
  const double sinDec = 0.39782 * sin(toRadians(trueLongitude));
  const double cosDec = cos(asin(sinDec));
  const double cosH = (cos(toRadians(90.0 - SUNSET_ALTITUDE)) - sinDec * sin(toRadians(lat))) /
                      (cosDec * cos(toRadians(lat)));
  if (cosH > 1.0 || cosH < -1.0) return -1;
  const double h = (rising ? 360.0 - toDegrees(acos(cosH)) : toDegrees(acos(cosH))) / 15.0;
  const double localMean = h + ra - 0.06571 * t - 6.622;
  const double utcHours = fmod(localMean - lonHours + 48.0, 24.0);
  return int(lround(utcHours * 60.0)) % 1440;
}

// Today's (local date) sunrise or sunset as local minutes since midnight, or -1.
int sunEventLocalMinutes(bool rising) {
  int y, m, d;
  civilFromDays(int32_t(localNow() / 86400), y, m, d);
  const int dayOfYear = daysFromCivil(y, m, d) - daysFromCivil(y, 1, 1) + 1;
  const int utcMinutes = sunEventUtcMinutes(dayOfYear, latitude, longitude, rising);
  if (utcMinutes < 0) return -1;
  return ((utcMinutes + utcOffsetHours * 60) % 1440 + 1440) % 1440;
}

// ---------- GPS ----------

void readGps(uint32_t now) {
  while (Serial1.available()) gps.encode(Serial1.read());

  if (gps.time.isUpdated() && gps.time.isValid() && gps.date.isValid() &&
      gps.date.year() >= 2024 && gps.date.year() <= 2099) {
    const int32_t days = daysFromCivil(gps.date.year(), gps.date.month(), gps.date.day());
    utcAtSync = int64_t(days) * 86400 + gps.time.hour() * 3600 + gps.time.minute() * 60 + gps.time.second();
    // The sentence describes the moment it started; it arrived a little later.
    syncMs = now - min<uint32_t>(gps.time.age(), 1000);
    timeKnown = true;
  }

  if (gps.location.isUpdated() && gps.location.isValid()) {
    const float lat = gps.location.lat(), lon = gps.location.lng();
    const bool moved = !locationKnown || fabsf(lat - latitude) > 0.05f || fabsf(lon - longitude) > 0.05f;
    latitude = lat;
    longitude = lon;
    locationKnown = true;
    // One write per boot (or after moving) is enough for sunset times.
    if (moved || !locationSavedThisBoot) {
      settings.putFloat("lat", latitude);
      settings.putFloat("lon", longitude);
      settings.putBool("hasloc", true);
      locationSavedThisBoot = true;
    }
  }
}

const char *gpsStatus() {
  static char text[24];
  if (gps.charsProcessed() < 10 && millis() > 5000) return "GPS: no data";
  const unsigned sats = gps.satellites.isValid() ? gps.satellites.value() : 0;
  if (gps.location.isValid() && gps.location.age() < 5000)
    snprintf(text, sizeof(text), "GPS: fix, %u sats", sats);
  else
    snprintf(text, sizeof(text), "GPS: search, %u sats", sats);
  return text;
}

// ---------- Light output ----------

void updateSchedule(uint32_t now) {
  if (now - lastScheduleMs < SCHEDULE_MS && lastScheduleMs != 0) return;
  lastScheduleMs = now;
  if (!timeKnown || !locationKnown) return;
  const bool up = sunAltitude(utcNow(), latitude, longitude) > SUNSET_ALTITUDE;
  if (up != sunUp) screenDirty = true;
  sunUp = up;
}

bool lampLit(uint32_t now) {
  // While the user is adjusting, show the light even if the schedule says off.
  const bool adjusting = now - lastInputMs < PREVIEW_MS && (page == Page::Color || page == Page::Brightness);
  switch (lightMode) {
    case LightMode::AlwaysOn: return true;
    case LightMode::Off: return adjusting;
    case LightMode::Auto:
      // Without time or position a street lamp stays on.
      return adjusting || !timeKnown || !locationKnown || !sunUp;
  }
  return true;
}

uint32_t baseColor() {
  return white ? 0xFFFFFF : Adafruit_NeoPixel::ColorHSV(hue);
}

void showColor() {
  if (blinksLeft % 2 == 1 || !lampLit(millis())) {  // odd phases are the "off" part of a blink
    led.setPixelColor(0, 0);
    led.show();
    return;
  }
  // Gamma-correct the full-strength color so the circle looks even,
  // then scale it to the brightness setting.
  const uint32_t rgb = Adafruit_NeoPixel::gamma32(baseColor());
  led.setPixelColor(0, (uint16_t((rgb >> 16) & 0xFF) * brightness + 127) / 255,
                      (uint16_t((rgb >> 8) & 0xFF) * brightness + 127) / 255,
                      (uint16_t(rgb & 0xFF) * brightness + 127) / 255);
  led.show();
}

void edited(uint32_t now) {
  showColor();
  saveNeeded = true;
  lastEditMs = now;
  screenDirty = true;
}

void blink(uint8_t times, uint32_t now) {
  blinksLeft = times * 2 - 1;  // starts with "off", ends back "on"
  blinkPhaseMs = now;
  showColor();
}

void updateBlink(uint32_t now) {
  if (blinksLeft == 0 || now - blinkPhaseMs < BLINK_MS) return;
  --blinksLeft;
  blinkPhaseMs = now;
  showColor();
}

// ---------- Inputs ----------

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

void wheelTask(void *) {
  uint8_t raw = wheelStable;
  uint32_t changedMs = millis();
  uint32_t lastNotchMs = 0;
  for (;;) {
    const uint32_t now = millis();
    const uint8_t sample = readWheel();
    if (sample != raw) {
      raw = sample;
      changedMs = now;
    }
    if (raw != wheelStable && raw != 0 && now - changedMs >= WHEEL_DEBOUNCE_MS) {
      const int step = wheelSteps[(wheelStable << 2) | raw];
      wheelStable = raw;
      if (step != 0) {
        const bool fast = now - lastNotchMs < FAST_SCROLL_MS;
        lastNotchMs = now;
        portENTER_CRITICAL(&wheelMux);
        pendingSteps += step;
        pendingHueSteps += fast ? step * 3 : step;
        wheelPosition += step;
        portEXIT_CRITICAL(&wheelMux);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

void applyWheel(uint32_t now) {
  portENTER_CRITICAL(&wheelMux);
  int steps = pendingSteps;
  const int hueSteps = pendingHueSteps;
  pendingSteps = pendingHueSteps = 0;
  portEXIT_CRITICAL(&wheelMux);
  if (steps == 0) return;
  lastInputMs = now;
  switch (page) {
    case Page::Color:
      // uint16_t wraps around, so the circle has no ends.
      hue += uint16_t(hueSteps * HUE_STEP);
      white = false;
      break;
    case Page::Brightness: {
      // One notch changes brightness by about a fifth: ~25 notches from dim to full.
      int level = brightness;
      for (; steps > 0; --steps) level = min(255, level + max(1, level / 5));
      for (; steps < 0; ++steps) level = max(int(MIN_BRIGHTNESS), level - max(1, level / 6));
      brightness = uint8_t(level);
      break;
    }
    case Page::LightMode:
      lightMode = LightMode(((int(lightMode) + steps) % LIGHT_MODE_COUNT + LIGHT_MODE_COUNT) % LIGHT_MODE_COUNT);
      break;
    case Page::TimeZone:
      utcOffsetHours = int8_t(constrain(utcOffsetHours + steps, -12, 14));
      break;
  }
  edited(now);
}

void pollButton(int reading, uint32_t now) {
  if (reading != lastReading) {
    lastReading = reading;
    lastChangeMs = now;
  }
  if (now - lastChangeMs >= DEBOUNCE_MS && (reading == LOW) != pressed) {
    pressed = (reading == LOW);
    lastInputMs = now;
    if (pressed) {
      pressStartMs = now;
      holdHandled = false;
    } else if (!holdHandled) {
      page = Page((uint8_t(page) + 1) % PAGE_COUNT);
      screenDirty = true;
      if (!screenAddress) blink(uint8_t(page) + 1, now);
      else showColor();  // the preview may end when leaving the color pages
    }
  }
  // Act as soon as the hold is recognized, without waiting for release.
  if (pressed && !holdHandled && now - pressStartMs >= HOLD_MS) {
    holdHandled = true;
    white = !white;
    edited(now);
  }
}

// ---------- Screen ----------

const char *colorName() {
  if (white) return "White";
  const uint32_t angle = uint32_t(hue) * 360 / 65536;
  static const struct { uint16_t below; const char *name; } names[] = {
    {15, "Red"}, {40, "Orange"}, {70, "Yellow"}, {100, "Lime"},
    {150, "Green"}, {175, "Teal"}, {205, "Cyan"}, {225, "Azure"},
    {260, "Blue"}, {290, "Violet"}, {320, "Magenta"}, {345, "Pink"},
  };
  for (const auto &entry : names) if (angle < entry.below) return entry.name;
  return "Red";
}

const char *lightModeName(LightMode mode) {
  switch (mode) {
    case LightMode::AlwaysOn: return "Always on";
    case LightMode::Auto: return "Auto (night)";
    case LightMode::Off: return "Off";
  }
  return "";
}

void formatMinutes(char *text, size_t size, int minutes) {
  if (minutes < 0) snprintf(text, size, "--:--");
  else snprintf(text, size, "%02d:%02d", minutes / 60, minutes % 60);
}

// Top band (yellow rows on two-color modules): page title and local time.
void drawHeader(const char *title) {
  screen.setFont(u8g2_font_6x12_tf);
  screen.drawBox(0, 0, screen.getStrWidth(title) + 6, 14);
  screen.setDrawColor(0);
  screen.drawStr(3, 11, title);
  screen.setDrawColor(1);
  char clock[8];
  formatMinutes(clock, sizeof(clock), timeKnown ? int(localNow() % 86400 / 60) : -1);
  screen.drawStr(127 - screen.getStrWidth(clock), 11, clock);
}

void drawColorPage() {
  drawHeader(page == Page::Color ? "COLOR" : "BRIGHTNESS");

  // Color circle with a marker at the current hue, red at the top, clockwise.
  constexpr int cx = 24, cy = 40;
  screen.drawCircle(cx, cy, 21);
  screen.drawCircle(cx, cy, 15);
  if (white) {
    screen.drawDisc(cx, cy, 10);
  } else {
    const float angle = hue * (2.0f * PI / 65536.0f);
    screen.drawDisc(cx + int(lroundf(18 * sinf(angle))), cy - int(lroundf(18 * cosf(angle))), 4);
  }

  // Name, hex code of the full-strength color, brightness bar and status.
  char hex[8];
  snprintf(hex, sizeof(hex), "#%06lX", (unsigned long)baseColor());
  screen.drawStr(53, 28, colorName());
  screen.drawStr(53, 41, hex);
  screen.drawFrame(53, 46, 74, 8);
  screen.drawBox(55, 48, (brightness * 70 + 127) / 255, 4);
  screen.setFont(u8g2_font_5x8_tf);
  char percent[8];
  snprintf(percent, sizeof(percent), "%d%%", (brightness * 100 + 127) / 255);
  screen.drawStr(53, 63, percent);
  const char *state = lightMode == LightMode::AlwaysOn ? "ON" : lightMode == LightMode::Off ? "OFF"
                      : (lampLit(millis()) ? "AUTO: on" : "AUTO: off");
  screen.drawStr(127 - screen.getStrWidth(state), 63, state);
  // Points at the setting the wheel changes.
  if (page == Page::Brightness) screen.drawTriangle(47, 46, 47, 54, 51, 50);
  else screen.drawTriangle(47, 20, 47, 28, 51, 24);
}

void drawLightModePage() {
  drawHeader("LIGHT MODE");
  for (uint8_t i = 0; i < LIGHT_MODE_COUNT; ++i) {
    const int y = 27 + i * 12;
    const bool selected = uint8_t(lightMode) == i;
    if (selected) screen.drawBox(0, y - 10, 128, 12);
    screen.setDrawColor(selected ? 0 : 1);
    screen.drawStr(4, y, lightModeName(LightMode(i)));
    screen.setDrawColor(1);
  }
  screen.setFont(u8g2_font_5x8_tf);
  char line[32];
  if (!timeKnown || !locationKnown) {
    snprintf(line, sizeof(line), "%s", "Waiting for GPS position");
  } else {
    const int sunset = sunEventLocalMinutes(false), sunrise = sunEventLocalMinutes(true);
    if (sunset < 0 || sunrise < 0) {
      snprintf(line, sizeof(line), "%s", sunUp ? "Polar day" : "Polar night");
    } else {
      char set[8], rise[8];
      formatMinutes(set, sizeof(set), sunset);
      formatMinutes(rise, sizeof(rise), sunrise);
      snprintf(line, sizeof(line), "Sunset %s  Sunrise %s", set, rise);
    }
  }
  screen.drawStr(0, 63, line);
}

void drawTimeZonePage() {
  drawHeader("TIME ZONE");
  char zone[12];
  snprintf(zone, sizeof(zone), "UTC%+d", utcOffsetHours);
  screen.setFont(u8g2_font_10x20_tf);
  screen.drawStr(4, 36, zone);
  screen.setFont(u8g2_font_6x12_tf);
  screen.drawStr(4, 50, gpsStatus());
  screen.setFont(u8g2_font_5x8_tf);
  char line[32];
  if (locationKnown) snprintf(line, sizeof(line), "%.3f, %.3f", latitude, longitude);
  else snprintf(line, sizeof(line), "%s", "Position unknown");
  screen.drawStr(4, 63, line);
}

void updateScreen(uint32_t now) {
  if (!screenAddress) return;
  if (screenOn && now - lastInputMs >= SCREEN_OFF_MS) {
    screenOn = false;
    screen.setPowerSave(1);
  } else if (!screenOn && now - lastInputMs < SCREEN_OFF_MS) {
    screenOn = true;
    screenDirty = true;
    screen.setPowerSave(0);
  }
  // Redraw when the clock's minute changes, and once a second on the GPS page.
  const int minute = timeKnown ? int(localNow() / 60 % 1440) : -1;
  if (minute != lastShownMinute) { lastShownMinute = minute; screenDirty = true; }
  if (page == Page::TimeZone && now - lastDrawMs >= 1000) screenDirty = true;
  if (!screenOn || !screenDirty || now - lastDrawMs < DRAW_INTERVAL_MS) return;
  screenDirty = false;
  lastDrawMs = now;
  screen.clearBuffer();
  switch (page) {
    case Page::Color:
    case Page::Brightness: drawColorPage(); break;
    case Page::LightMode: drawLightModePage(); break;
    case Page::TimeZone: drawTimeZonePage(); break;
  }
  screen.sendBuffer();
}

void setupScreen() {
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  Wire.setClock(400000);
  for (uint8_t address : {0x3C, 0x3D}) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0) { screenAddress = address; break; }
  }
  if (!screenAddress) return;
  screen.setI2CAddress(screenAddress * 2);  // u8g2 expects the 8-bit form
  screen.begin();
}

#if LED_DIAGNOSTICS
// Send 'r' over USB serial for the current state, 'n' for 3 s of raw GPS output.
void diagnosticsLoop() {
  if (!Serial.available()) return;
  const int command = Serial.read();
  if (command == 'n') {
    const uint32_t end = millis() + 3000;
    while (millis() < end) {
      while (Serial1.available()) Serial.write(Serial1.read());
      delay(1);
    }
    return;
  }
  if (command != 'r') return;
  Serial.printf("DIAG screen=0x%02X wheel_ab=%u%u position=%d page=%u hue=%u white=%d brightness=%u mode=%u\n",
    screenAddress, (wheelStable >> 1) & 1, wheelStable & 1, wheelPosition,
    unsigned(page), hue, int(white), brightness, unsigned(lightMode));
  Serial.printf("DIAG gps_chars=%lu sentences_ok=%lu failed=%lu sats=%lu fix=%d time=%d utc=%lld loc=%d lat=%.4f lon=%.4f sun_up=%d lit=%d\n",
    (unsigned long)gps.charsProcessed(), (unsigned long)gps.passedChecksum(), (unsigned long)gps.failedChecksum(),
    (unsigned long)(gps.satellites.isValid() ? gps.satellites.value() : 0), int(gps.location.isValid()),
    int(timeKnown), (long long)(timeKnown ? utcNow() : 0), int(locationKnown), latitude, longitude,
    int(sunUp), int(lampLit(millis())));
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
  wheelStable = readWheel();
  Serial1.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  settings.begin("led", false);
  hue = settings.getUShort("hue", 0);
  white = settings.getBool("white", false);
  brightness = max(MIN_BRIGHTNESS, settings.getUChar("bright", 60));
  lightMode = LightMode(settings.getUChar("mode", 0) % LIGHT_MODE_COUNT);
  utcOffsetHours = int8_t(constrain(int(settings.getChar("utc", 3)), -12, 14));
  locationKnown = settings.getBool("hasloc", false);
  latitude = settings.getFloat("lat", 0);
  longitude = settings.getFloat("lon", 0);

  led.begin();
  showColor();
  setupScreen();

  lastReading = digitalRead(BUTTON_PIN);
  // A button held during power-up counts as pressed from this moment.
  pressed = (lastReading == LOW);
  lastChangeMs = pressStartMs = lastInputMs = millis();
  // Above the Arduino loop's priority, so it preempts screen updates.
  xTaskCreate(wheelTask, "wheel", 2048, nullptr, 5, nullptr);
}

void loop() {
  const uint32_t now = millis();
  readGps(now);
  pollButton(digitalRead(BUTTON_PIN), now);
  applyWheel(now);
  updateSchedule(now);
  updateBlink(now);
  updateScreen(now);
#if LED_DIAGNOSTICS
  diagnosticsLoop();
#endif

  if (saveNeeded && now - lastEditMs >= SAVE_DELAY_MS) {
    settings.putUShort("hue", hue);
    settings.putBool("white", white);
    settings.putUChar("bright", brightness);
    settings.putUChar("mode", uint8_t(lightMode));
    settings.putChar("utc", utcOffsetHours);
    saveNeeded = false;
  }

  // Also applies schedule and preview changes within half a second.
  static uint32_t lastRefreshMs = 0;
  if (now - lastRefreshMs >= 500) {
    lastRefreshMs = now;
    showColor();
  }

  delay(1);
}
