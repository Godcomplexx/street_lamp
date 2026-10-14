// Offline LED controller: no Wi-Fi or Bluetooth. For boards whose radio
// does not work (see VALIDATION.md).
//   short press  -> next field / next page: HOME, COLOR, BRIGHTNESS,
//                   LIGHT MODE, CLOCK (hour, minute, day, month, year,
//                   UTC offset), LOCATION (latitude, longitude)
//   wheel        -> changes the selected setting
//   hold         -> white light on / off on COLOR and BRIGHTNESS,
//                   back to HOME on the other pages
// While the OLED is off, the first wheel notch or press only wakes it on HOME.
// LIGHT MODE: "Adaptive" follows the sun with color temperature and level
// (neutral white by day, warm in the evening, dim amber late at night);
// "Night only" lights the lamp between sunset and sunrise. Both use the clock
// and position, which can be set by hand or come from an optional NEO-6M GPS.
// Without a display, HOME is skipped and a short press blinks the page number.
// All settings and the position survive power loss. The clock survives it
// only with an optional DS3231 RTC on the OLED's I2C bus; without one, the
// last known time is kept as the starting point for setting the clock again.
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
// Optional DS3231 RTC on the same I2C bus as the OLED, powered from 3V3.
constexpr uint8_t RTC_ADDRESS = 0x68;
// Optional TP4056 status outputs, each through a 10 kOhm resistor: CHRG pulls
// low while charging, STDBY when charged. Unconnected pins read "not charging".
constexpr uint8_t CHARGING_PIN = 1;
constexpr uint8_t CHARGED_PIN = 0;

constexpr uint32_t WHEEL_DEBOUNCE_MS = 12;
constexpr uint16_t HUE_STEP = 65536 / 48;   // 48 notches per full circle
constexpr uint32_t FAST_SCROLL_MS = 40;     // notches closer than this count as fast
constexpr float COORDINATE_STEP = 0.1f;     // degrees per slow notch; fast notches move 1 degree
constexpr uint32_t DEBOUNCE_MS = 20;
constexpr uint32_t HOLD_MS = 600;           // longer than this is a hold, not a press
constexpr uint8_t MIN_BRIGHTNESS = 3;
constexpr uint32_t SAVE_DELAY_MS = 2000;    // limits flash writes while adjusting
constexpr uint32_t BLINK_MS = 120;
constexpr uint32_t DRAW_INTERVAL_MS = 30;
constexpr uint32_t IDLE_MS = 60000;         // then HOME, and the OLED sleeps unless charging
constexpr uint8_t IDLE_CONTRAST = 8;        // always-on display while charging
constexpr uint32_t PREVIEW_MS = 10000;      // a lamp switched off by schedule lights up while adjusted
constexpr uint32_t SCHEDULE_MS = 10000;
constexpr uint32_t LAST_TIME_SAVE_MS = 10 * 60000;  // flash writes stay rare
constexpr uint32_t RTC_GPS_SYNC_MS = 3600000;       // GPS corrects the RTC hourly
constexpr double SUNSET_ALTITUDE = -0.833;  // degrees: refraction plus the sun's radius
// Starting point when a position is entered by hand for the first time.
constexpr float DEFAULT_LATITUDE = 55.75f, DEFAULT_LONGITUDE = 37.62f;  // Moscow

Adafruit_NeoPixel led(1, LED_PIN, NEO_GRB + NEO_KHZ800);
Preferences settings;
U8G2_SSD1306_128X64_NONAME_F_HW_I2C screen(U8G2_R0, U8X8_PIN_NONE, OLED_SCL_PIN, OLED_SDA_PIN);
TinyGPSPlus gps;

enum class Page : uint8_t { Home, Color, Brightness, LightMode, Clock, Location };
constexpr uint8_t PAGE_COUNT = 6;
// Stored values; NightOnly keeps the value of the former "Auto".
enum class LightMode : uint8_t { AlwaysOn = 0, NightOnly = 1, Off = 2, Adaptive = 3 };
// Order on the LIGHT MODE page and when scrolling through it.
const LightMode LIGHT_MODES[] = {LightMode::AlwaysOn, LightMode::Adaptive, LightMode::NightOnly, LightMode::Off};
constexpr uint8_t LIGHT_MODE_COUNT = 4;
enum ClockField : uint8_t { HOUR, MINUTE, DAY, MONTH, YEAR, ZONE, CLOCK_FIELDS };
enum LocationField : uint8_t { LATITUDE, LONGITUDE, LOCATION_FIELDS };
enum class Source : uint8_t { None, Saved, Manual, Gps, Rtc };

uint16_t hue = 0;            // 0..65535 around the circle, 0 = red
bool white = false;
uint8_t brightness = 60;
LightMode lightMode = LightMode::AlwaysOn;
int8_t utcOffsetHours = 3;
Page page = Page::Home;
uint8_t field = 0;           // selected field on CLOCK and LOCATION

bool pressed = false;        // debounced state
int lastReading = HIGH;
uint32_t lastChangeMs = 0;
uint32_t pressStartMs = 0;
bool holdHandled = false;
bool wakePress = false;      // this press only woke the display
bool saveNeeded = false;
uint32_t lastEditMs = 0;
uint32_t lastInputMs = 0;

uint8_t blinksLeft = 0;      // off/on phases still to show
uint32_t blinkPhaseMs = 0;

uint8_t screenAddress = 0;   // 0 when no display answers
bool screenDirty = true;
bool screenOn = true;
bool screenDimmed = false;
bool idle = false;           // no input for IDLE_MS
uint32_t lastDrawMs = 0;

// Time: set by hand or by the GPS; between updates the board's own clock counts.
bool timeKnown = false;
Source timeSource = Source::None;
int64_t utcAtSync = 0;       // Unix seconds at syncMs
uint32_t syncMs = 0;
bool locationKnown = false;
Source locationSource = Source::None;
float latitude = 0, longitude = 0;
bool locationSavedThisBoot = false;
bool rtcPresent = false;
bool rtcWriteNeeded = false;  // the clock was set by hand
uint32_t lastRtcGpsSyncMs = 0;
uint32_t lastTimeSaveMs = 0;
bool sunUp = false;          // last schedule result, valid when timeKnown && locationKnown
uint32_t adaptiveRgb = 0;    // current adaptive color, valid when timeKnown && locationKnown
uint16_t adaptiveKelvin = 0;
float adaptiveLevel = 1.0f;  // multiplies the brightness setting
uint32_t lastScheduleMs = 0;
int lastShownMinute = -1;

// The wheel is polled by its own task so that a screen redraw (~25 ms)
// cannot swallow notches. The loop picks up the accumulated steps.
portMUX_TYPE wheelMux = portMUX_INITIALIZER_UNLOCKED;
int pendingSteps = 0;        // one per notch
int pendingFastSteps = 0;    // the part of pendingSteps made while scrolling fast
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

int daysInMonth(int y, int m) {
  static const uint8_t days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
  return m == 2 && leap ? 29 : days[m - 1];
}

int wrap(int value, int count) { return (value % count + count) % count; }

int64_t utcNow() {
  return utcAtSync + int64_t(millis() - syncMs) / 1000;
}

int64_t localNow() {
  return utcNow() + int64_t(utcOffsetHours) * 3600;
}

int localMinutes() {
  return int(localNow() % 86400 / 60);
}

// Firmware build time as local seconds: a close starting point for manual entry.
int64_t buildTimeLocal() {
  static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char name[4] = {};
  int day = 1, year = 2026, hour = 0, minute = 0, second = 0;
  sscanf(__DATE__, "%3s %d %d", name, &day, &year);
  sscanf(__TIME__, "%d:%d:%d", &hour, &minute, &second);
  const char *found = strstr(months, name);
  const int month = found ? int(found - months) / 3 + 1 : 1;
  return int64_t(daysFromCivil(year, month, day)) * 86400 + hour * 3600 + minute * 60 + second;
}

// ---------- DS3231 real-time clock (keeps UTC) ----------

uint8_t toBcd(int value) { return uint8_t((value / 10) << 4 | value % 10); }
int fromBcd(uint8_t value) { return (value >> 4) * 10 + (value & 0x0F); }

bool rtcRead(int64_t &utc) {
  Wire.beginTransmission(RTC_ADDRESS);
  Wire.write(0x00);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom(RTC_ADDRESS, uint8_t(7)) != 7) return false;
  uint8_t r[7];
  for (uint8_t &value : r) value = Wire.read();
  Wire.beginTransmission(RTC_ADDRESS);
  Wire.write(0x0F);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom(RTC_ADDRESS, uint8_t(1)) != 1) return false;
  if (Wire.read() & 0x80) return false;  // oscillator stopped: battery was out, time invalid
  const int year = 2000 + fromBcd(r[6]);
  if (year < 2024) return false;
  utc = int64_t(daysFromCivil(year, fromBcd(r[5] & 0x1F), fromBcd(r[4]))) * 86400 +
        fromBcd(r[2] & 0x3F) * 3600 + fromBcd(r[1]) * 60 + fromBcd(r[0] & 0x7F);
  return true;
}

void rtcWrite(int64_t utc) {
  const int32_t days = int32_t(utc / 86400);
  const int seconds = int(utc % 86400);
  int y, m, d;
  civilFromDays(days, y, m, d);
  Wire.beginTransmission(RTC_ADDRESS);
  Wire.write(0x00);
  Wire.write(toBcd(seconds % 60));
  Wire.write(toBcd(seconds / 60 % 60));
  Wire.write(toBcd(seconds / 3600));        // 24-hour mode
  Wire.write(uint8_t((days + 3) % 7 + 1));  // weekday 1-7, Monday = 1
  Wire.write(toBcd(d));
  Wire.write(toBcd(m));
  Wire.write(toBcd(y - 2000));
  Wire.endTransmission();
  // Clear the oscillator-stop flag so the time counts as valid.
  Wire.beginTransmission(RTC_ADDRESS);
  Wire.write(0x0F);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom(RTC_ADDRESS, uint8_t(1)) != 1) return;
  const uint8_t status = Wire.read();
  Wire.beginTransmission(RTC_ADDRESS);
  Wire.write(0x0F);
  Wire.write(uint8_t(status & 0x7F));
  Wire.endTransmission();
}

void setupClock(uint32_t now) {
  Wire.beginTransmission(RTC_ADDRESS);
  rtcPresent = Wire.endTransmission() == 0;
  int64_t utc;
  if (rtcPresent && rtcRead(utc)) {
    utcAtSync = utc;
    syncMs = now;
    timeKnown = true;
    timeSource = Source::Rtc;
  }
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
    timeSource = Source::Gps;
    if (rtcPresent && (lastRtcGpsSyncMs == 0 || now - lastRtcGpsSyncMs >= RTC_GPS_SYNC_MS)) {
      rtcWrite(utcAtSync + int64_t(now - syncMs) / 1000);
      lastRtcGpsSyncMs = now;
    }
  }

  if (gps.location.isUpdated() && gps.location.isValid()) {
    const float lat = gps.location.lat(), lon = gps.location.lng();
    const bool moved = !locationKnown || fabsf(lat - latitude) > 0.05f || fabsf(lon - longitude) > 0.05f;
    latitude = lat;
    longitude = lon;
    locationKnown = true;
    locationSource = Source::Gps;
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

// ---------- Charger ----------

bool isCharging() { return digitalRead(CHARGING_PIN) == LOW; }
bool isCharged() { return digitalRead(CHARGED_PIN) == LOW; }
bool onCharger() { return isCharging() || isCharged(); }

// ---------- Light output ----------

float mix(float from, float to, float t) { return from + (to - from) * constrain(t, 0.0f, 1.0f); }

// Color of a black body at the given temperature (T. Helland's approximation).
uint32_t kelvinToRgb(float kelvin) {
  const float t = kelvin / 100.0f;
  const float r = t <= 66 ? 255 : 329.698727446f * powf(t - 60, -0.1332047592f);
  const float g = t <= 66 ? 99.4708025861f * logf(t) - 161.1195681661f
                          : 288.1221695283f * powf(t - 60, -0.0755148492f);
  const float b = t >= 66 ? 255 : t <= 19 ? 0 : 138.5177312231f * logf(t - 10) - 305.0447927307f;
  auto channel = [](float v) { return uint32_t(constrain(v, 0.0f, 255.0f)); };
  return channel(r) << 16 | channel(g) << 8 | channel(b);
}

// Adaptive light from the sun's altitude, plus a late-night dim amber phase
// by the clock. Returns the color temperature and sets the level factor.
float adaptiveProfile(double altitude, int minutes, float &level) {
  float kelvin;
  if (altitude >= 20) {                       // day
    kelvin = 5500; level = 1.0f;
  } else if (altitude >= 0) {                 // sun low: warming towards sunset
    const float t = float(altitude / 20);
    kelvin = mix(3500, 5500, t); level = mix(0.85f, 1.0f, t);
  } else if (altitude >= -6) {                // civil twilight
    const float t = float((altitude + 6) / 6);
    kelvin = mix(2700, 3500, t); level = mix(0.65f, 0.85f, t);
  } else {                                    // evening and night
    kelvin = 2700; level = 0.65f;
  }
  // 22:00-23:00 fade into a night light, held until 05:00, gone by 05:30.
  float night = 0;
  if (minutes >= 22 * 60) night = (minutes - 22 * 60) / 60.0f;
  else if (minutes < 5 * 60) night = 1;
  else if (minutes < 5 * 60 + 30) night = 1 - (minutes - 5 * 60) / 30.0f;
  if (altitude > -6) night *= constrain(float(-altitude / 6), 0.0f, 1.0f);  // not while it is light
  kelvin = mix(kelvin, 2000, night);
  level = mix(level, 0.25f, night);
  return kelvin;
}

void updateSchedule(uint32_t now) {
  if (now - lastScheduleMs < SCHEDULE_MS && lastScheduleMs != 0) return;
  lastScheduleMs = now;
  if (!timeKnown || !locationKnown) return;
  const double altitude = sunAltitude(utcNow(), latitude, longitude);
  const bool up = altitude > SUNSET_ALTITUDE;
  float level = 1.0f;
  const float kelvin = adaptiveProfile(altitude, localMinutes(), level);
  const uint16_t roundedKelvin = uint16_t(lroundf(kelvin / 50) * 50);
  if (up != sunUp || roundedKelvin != adaptiveKelvin) screenDirty = true;
  sunUp = up;
  adaptiveKelvin = roundedKelvin;
  adaptiveRgb = kelvinToRgb(kelvin);
  adaptiveLevel = level;
}

bool adaptiveActive() {
  return lightMode == LightMode::Adaptive && timeKnown && locationKnown;
}

bool lampLit(uint32_t now) {
  // While the user is adjusting, show the light even if the schedule says off.
  const bool adjusting = now - lastInputMs < PREVIEW_MS && (page == Page::Color || page == Page::Brightness);
  switch (lightMode) {
    case LightMode::AlwaysOn:
    case LightMode::Adaptive: return true;
    case LightMode::Off: return adjusting;
    case LightMode::NightOnly:
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
  // Circle colors are gamma-corrected so the circle looks even; black-body
  // colors are already perceptual. Then scale to the brightness setting.
  const bool adaptive = adaptiveActive();
  const uint32_t rgb = adaptive ? adaptiveRgb : Adafruit_NeoPixel::gamma32(baseColor());
  const uint16_t level = adaptive ? uint16_t(max(1.0f, brightness * adaptiveLevel)) : brightness;
  led.setPixelColor(0, (uint16_t((rgb >> 16) & 0xFF) * level + 127) / 255,
                      (uint16_t((rgb >> 8) & 0xFF) * level + 127) / 255,
                      (uint16_t(rgb & 0xFF) * level + 127) / 255);
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

// ---------- Manual clock and position ----------

// Changes one field of the local date and time; the others stay as they are.
void editClock(int steps, uint32_t now) {
  if (field == ZONE) {
    utcOffsetHours = int8_t(constrain(utcOffsetHours + steps, -12, 14));
    return;
  }
  if (!timeKnown) {
    // Start from the last known time if it is newer than the firmware build.
    const int64_t built = buildTimeLocal() - int64_t(utcOffsetHours) * 3600;
    utcAtSync = max(built, int64_t(settings.getLong64("lasttime", 0)));
    syncMs = now;
  }
  const int64_t local = localNow();
  const int32_t days = int32_t(local / 86400);
  int seconds = int(local % 86400);
  int y, m, d;
  civilFromDays(days, y, m, d);
  int hour = seconds / 3600, minute = seconds / 60 % 60;
  seconds %= 60;
  switch (field) {
    case HOUR: hour = wrap(hour + steps, 24); break;
    case MINUTE: minute = wrap(minute + steps, 60); seconds = 0; break;  // start the minute now
    case DAY: d = wrap(d - 1 + steps, daysInMonth(y, m)) + 1; break;
    case MONTH: m = wrap(m - 1 + steps, 12) + 1; break;
    case YEAR: y = constrain(y + steps, 2024, 2099); break;
  }
  d = min(d, daysInMonth(y, m));
  const int64_t updated = int64_t(daysFromCivil(y, m, d)) * 86400 + hour * 3600 + minute * 60 + seconds;
  utcAtSync = updated - int64_t(utcOffsetHours) * 3600;
  syncMs = now;
  timeKnown = true;
  timeSource = Source::Manual;
  rtcWriteNeeded = true;
}

void editLocation(int steps, int fastSteps) {
  if (!locationKnown) {
    latitude = DEFAULT_LATITUDE;
    longitude = DEFAULT_LONGITUDE;
  }
  const float delta = (steps + 9 * fastSteps) * COORDINATE_STEP;
  if (field == LATITUDE) {
    latitude = constrain(latitude + delta, -89.9f, 89.9f);
  } else {
    longitude += delta;
    if (longitude > 180.0f) longitude -= 360.0f;
    if (longitude < -180.0f) longitude += 360.0f;
  }
  locationKnown = true;
  locationSource = Source::Manual;
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
        if (fast) pendingFastSteps += step;
        wheelPosition += step;
        portEXIT_CRITICAL(&wheelMux);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

// True while the display is asleep or dimmed: the next input only wakes it.
bool displayWaiting() {
  return screenAddress && (!screenOn || screenDimmed);
}

void wake(uint32_t now) {
  lastInputMs = now;
  page = Page::Home;
  field = 0;
  screenDirty = true;
}

Page homePage() { return screenAddress ? Page::Home : Page::Color; }

void applyWheel(uint32_t now) {
  portENTER_CRITICAL(&wheelMux);
  int steps = pendingSteps;
  const int fastSteps = pendingFastSteps;
  pendingSteps = pendingFastSteps = 0;
  portEXIT_CRITICAL(&wheelMux);
  if (steps == 0) return;
  if (displayWaiting()) { wake(now); return; }
  lastInputMs = now;
  switch (page) {
    case Page::Home:
      return;  // only shows the time
    case Page::Color:
      // Choosing a color by hand leaves the adaptive light.
      if (lightMode == LightMode::Adaptive) lightMode = LightMode::AlwaysOn;
      // Fast notches move 3x. uint16_t wraps around, so the circle has no ends.
      hue += uint16_t((steps + 2 * fastSteps) * HUE_STEP);
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
    case Page::LightMode: {
      int index = 0;
      while (LIGHT_MODES[index] != lightMode) ++index;
      lightMode = LIGHT_MODES[wrap(index + steps, LIGHT_MODE_COUNT)];
      break;
    }
    case Page::Clock:
      editClock(steps, now);
      lastScheduleMs = 0;  // re-evaluate the schedule right away
      break;
    case Page::Location:
      editLocation(steps, fastSteps);
      lastScheduleMs = 0;
      break;
  }
  edited(now);
}

uint8_t fieldCount(Page p) {
  return p == Page::Clock ? CLOCK_FIELDS : p == Page::Location ? LOCATION_FIELDS : 1;
}

void pollButton(int reading, uint32_t now) {
  if (reading != lastReading) {
    lastReading = reading;
    lastChangeMs = now;
  }
  if (now - lastChangeMs >= DEBOUNCE_MS && (reading == LOW) != pressed) {
    pressed = (reading == LOW);
    if (pressed) {
      pressStartMs = now;
      holdHandled = false;
      wakePress = displayWaiting();
      if (wakePress) wake(now);
    } else if (!holdHandled && !wakePress) {
      lastInputMs = now;
      if (field + 1 < fieldCount(page)) {
        ++field;
      } else {
        field = 0;
        page = Page((uint8_t(page) + 1) % PAGE_COUNT);
        if (page == Page::Home && !screenAddress) page = Page::Color;
        if (!screenAddress) blink(uint8_t(page), now);  // COLOR blinks once
      }
      screenDirty = true;
      showColor();  // the preview may end when leaving the color pages
    }
  }
  // Act as soon as the hold is recognized, without waiting for release.
  if (pressed && !holdHandled && !wakePress && now - pressStartMs >= HOLD_MS) {
    holdHandled = true;
    lastInputMs = now;
    if (page == Page::Color || page == Page::Brightness) {
      if (lightMode == LightMode::Adaptive) lightMode = LightMode::AlwaysOn;
      white = !white;
      edited(now);
    } else if (page != Page::Home) {
      page = homePage();
      field = 0;
      screenDirty = true;
      if (!screenAddress) blink(1, now);
    }
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
    case LightMode::Adaptive: return "Adaptive";
    case LightMode::NightOnly: return "Night only";
    case LightMode::Off: return "Off";
  }
  return "";
}

const char *sourceName(Source source) {
  switch (source) {
    case Source::None: return "Not set";
    case Source::Saved: return "Saved";
    case Source::Manual: return "Set by hand";
    case Source::Gps: return "From GPS";
    case Source::Rtc: return "From RTC";
  }
  return "";
}

void formatMinutes(char *text, size_t size, int minutes) {
  if (minutes < 0) snprintf(text, size, "--:--");
  else snprintf(text, size, "%02d:%02d", minutes / 60, minutes % 60);
}

const char *chargerText() {
  return isCharging() ? "Charging" : isCharged() ? "Charged" : "";
}

// Text with an inverted box behind it when selected; returns the x after it.
int drawField(int x, int y, int height, const char *text, bool selected) {
  const int width = screen.getStrWidth(text);
  if (selected) {
    screen.drawBox(x - 1, y - height + 1, width + 2, height + 1);
    screen.setDrawColor(0);
  }
  screen.drawStr(x, y, text);
  screen.setDrawColor(1);
  return x + width;
}

// Top band (yellow rows on two-color modules): page title and local time.
void drawHeader(const char *title) {
  screen.setFont(u8g2_font_6x12_tf);
  screen.drawBox(0, 0, screen.getStrWidth(title) + 6, 14);
  screen.setDrawColor(0);
  screen.drawStr(3, 11, title);
  screen.setDrawColor(1);
  char clock[8];
  formatMinutes(clock, sizeof(clock), timeKnown ? localMinutes() : -1);
  screen.drawStr(127 - screen.getStrWidth(clock), 11, clock);
}

// Large clock. While the display stays on during charging, the layout moves
// by a few pixels every minute so no pixel is lit all the time.
void drawHomePage() {
  const int shift = idle ? (timeKnown ? localMinutes() : int(millis() / 60000)) % 4 : 0;
  screen.setFont(u8g2_font_6x12_tf);
  static const char *weekdays[] = {"Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed"};  // 1970-01-01 was a Thursday
  static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  char line[32] = "Clock not set";
  if (timeKnown) {
    const int32_t days = int32_t(localNow() / 86400);
    int y, m, d;
    civilFromDays(days, y, m, d);
    snprintf(line, sizeof(line), "%s %d %s", weekdays[days % 7], d, months[m - 1]);
  }
  screen.drawStr(shift, 11, line);
  const char *charger = chargerText();
  screen.drawStr(127 - shift - screen.getStrWidth(charger), 11, charger);

  char clock[8];
  formatMinutes(clock, sizeof(clock), timeKnown ? localMinutes() : -1);
  screen.setFont(u8g2_font_logisoso28_tn);
  screen.drawStr((128 - screen.getStrWidth(clock)) / 2 + shift - 2, 47, clock);

  screen.setFont(u8g2_font_5x8_tf);
  switch (lightMode) {
    case LightMode::Adaptive:
      if (adaptiveActive()) snprintf(line, sizeof(line), "Adaptive %uK", adaptiveKelvin);
      else snprintf(line, sizeof(line), "%s", "Adaptive: set clock");
      break;
    case LightMode::NightOnly: {
      if (!timeKnown || !locationKnown) { snprintf(line, sizeof(line), "%s", "Night only: set clock"); break; }
      char at[8];
      formatMinutes(at, sizeof(at), sunEventLocalMinutes(!sunUp));
      snprintf(line, sizeof(line), sunUp ? "Night only, on at %s" : "Night only, off at %s", at);
      break;
    }
    default:
      snprintf(line, sizeof(line), "%s", lightModeName(lightMode));
  }
  screen.drawStr((128 - screen.getStrWidth(line)) / 2 + shift - 2, 62, line);
}

void drawColorPage() {
  drawHeader(page == Page::Color ? "COLOR" : "BRIGHTNESS");

  // Color circle with a marker at the current hue, red at the top, clockwise.
  const bool adaptive = adaptiveActive();
  constexpr int cx = 24, cy = 40;
  screen.drawCircle(cx, cy, 21);
  screen.drawCircle(cx, cy, 15);
  if (white || adaptive) {
    screen.drawDisc(cx, cy, 10);
  } else {
    const float angle = hue * (2.0f * PI / 65536.0f);
    screen.drawDisc(cx + int(lroundf(18 * sinf(angle))), cy - int(lroundf(18 * cosf(angle))), 4);
  }

  // Name, hex code or color temperature, brightness bar and status.
  char hex[8];
  if (adaptive) snprintf(hex, sizeof(hex), "%uK", adaptiveKelvin);
  else snprintf(hex, sizeof(hex), "#%06lX", (unsigned long)baseColor());
  screen.drawStr(53, 28, adaptive ? "Adaptive" : colorName());
  screen.drawStr(53, 41, hex);
  screen.drawFrame(53, 46, 74, 8);
  screen.drawBox(55, 48, (brightness * 70 + 127) / 255, 4);
  screen.setFont(u8g2_font_5x8_tf);
  char percent[8];
  snprintf(percent, sizeof(percent), "%d%%", (brightness * 100 + 127) / 255);
  screen.drawStr(53, 63, percent);
  const char *state = "ON";
  switch (lightMode) {
    case LightMode::AlwaysOn: state = "ON"; break;
    case LightMode::Off: state = "OFF"; break;
    case LightMode::Adaptive: state = adaptive ? "ADAPTIVE" : "SET CLOCK"; break;
    case LightMode::NightOnly: state = lampLit(millis()) ? "NIGHT: on" : "NIGHT: off"; break;
  }
  screen.drawStr(127 - screen.getStrWidth(state), 63, state);
  // Points at the setting the wheel changes.
  if (page == Page::Brightness) screen.drawTriangle(47, 46, 47, 54, 51, 50);
  else screen.drawTriangle(47, 20, 47, 28, 51, 24);
}

void drawLightModePage() {
  drawHeader("LIGHT MODE");
  for (uint8_t i = 0; i < LIGHT_MODE_COUNT; ++i) {
    const int y = 24 + i * 10;
    const bool selected = LIGHT_MODES[i] == lightMode;
    if (selected) screen.drawBox(0, y - 9, 128, 10);
    screen.setDrawColor(selected ? 0 : 1);
    screen.drawStr(4, y, lightModeName(LIGHT_MODES[i]));
    screen.setDrawColor(1);
  }
  screen.setFont(u8g2_font_5x8_tf);
  char line[32];
  if (!timeKnown || !locationKnown) {
    snprintf(line, sizeof(line), "%s", !timeKnown ? "Set CLOCK for sun modes" : "Set LOCATION for sun modes");
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

void drawClockPage() {
  drawHeader("CLOCK");
  int y = 0, m = 0, d = 0, hour = -1, minute = -1;
  if (timeKnown) {
    const int64_t local = localNow();
    civilFromDays(int32_t(local / 86400), y, m, d);
    hour = int(local % 86400 / 3600);
    minute = int(local % 3600 / 60);
  }
  char text[8];
  // Large time, then the date and the UTC offset; the selected field is inverted.
  screen.setFont(u8g2_font_10x20_tf);
  int x = 4;
  if (hour < 0) snprintf(text, sizeof(text), "--"); else snprintf(text, sizeof(text), "%02d", hour);
  x = drawField(x, 37, 16, text, field == HOUR);
  x = drawField(x, 37, 16, ":", false);
  if (minute < 0) snprintf(text, sizeof(text), "--"); else snprintf(text, sizeof(text), "%02d", minute);
  drawField(x, 37, 16, text, field == MINUTE);

  screen.setFont(u8g2_font_6x12_tf);
  x = 4;
  if (d) snprintf(text, sizeof(text), "%02d", d); else snprintf(text, sizeof(text), "--");
  x = drawField(x, 52, 10, text, field == DAY);
  x = drawField(x, 52, 10, ".", false);
  if (m) snprintf(text, sizeof(text), "%02d", m); else snprintf(text, sizeof(text), "--");
  x = drawField(x, 52, 10, text, field == MONTH);
  x = drawField(x, 52, 10, ".", false);
  if (y) snprintf(text, sizeof(text), "%04d", y); else snprintf(text, sizeof(text), "----");
  drawField(x, 52, 10, text, field == YEAR);
  snprintf(text, sizeof(text), "UTC%+d", utcOffsetHours);
  drawField(84, 52, 10, text, field == ZONE);

  screen.setFont(u8g2_font_5x8_tf);
  screen.drawStr(4, 63, sourceName(timeSource));
}

void drawLocationPage() {
  drawHeader("LOCATION");
  char text[16];
  screen.drawStr(4, 30, "Lat");
  if (locationKnown) snprintf(text, sizeof(text), "%6.2f %c", fabsf(latitude), latitude < 0 ? 'S' : 'N');
  else snprintf(text, sizeof(text), "%s", "  --.--");
  drawField(34, 30, 10, text, field == LATITUDE);
  screen.drawStr(4, 44, "Lon");
  if (locationKnown) snprintf(text, sizeof(text), "%6.2f %c", fabsf(longitude), longitude < 0 ? 'W' : 'E');
  else snprintf(text, sizeof(text), "%s", "  --.--");
  drawField(34, 44, 10, text, field == LONGITUDE);

  screen.setFont(u8g2_font_5x8_tf);
  screen.drawStr(4, 54, sourceName(locationSource));
  screen.drawStr(4, 63, gpsStatus());
}

void updateScreen(uint32_t now) {
  if (!screenAddress) return;
  // After a minute without input: back to HOME, then sleep, or stay on
  // dimmed while the lamp is on the charger.
  const bool wasIdle = idle;
  idle = now - lastInputMs >= IDLE_MS;
  if (idle && !wasIdle) { page = Page::Home; field = 0; screenDirty = true; }
  const bool wantOn = !idle || onCharger();
  const bool wantDim = idle && wantOn;
  if (wantOn != screenOn) {
    screenOn = wantOn;
    screen.setPowerSave(screenOn ? 0 : 1);
    screenDirty = true;
  }
  if (wantDim != screenDimmed) {
    screenDimmed = wantDim;
    screen.setContrast(screenDimmed ? IDLE_CONTRAST : 255);
    screenDirty = true;
  }
  // Redraw when the clock's minute changes, and once a second on the GPS
  // page and the charger state on HOME.
  const int minute = timeKnown ? int(localNow() / 60 % 1440) : -1;
  if (minute != lastShownMinute) { lastShownMinute = minute; screenDirty = true; }
  if ((page == Page::Location || page == Page::Home) && now - lastDrawMs >= 1000) screenDirty = true;
  if (!screenOn || !screenDirty || now - lastDrawMs < DRAW_INTERVAL_MS) return;
  screenDirty = false;
  lastDrawMs = now;
  screen.clearBuffer();
  switch (page) {
    case Page::Home: drawHomePage(); break;
    case Page::Color:
    case Page::Brightness: drawColorPage(); break;
    case Page::LightMode: drawLightModePage(); break;
    case Page::Clock: drawClockPage(); break;
    case Page::Location: drawLocationPage(); break;
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
// USB serial: 'r' state report, 'n' 3 s of raw GPS output,
// 'm' GPS radio status (UBX-MON-HW reply as hex).
void diagnosticsLoop() {
  if (!Serial.available()) return;
  const int command = Serial.read();
  if (command == 'm') {
    static const uint8_t poll[] = {0xB5, 0x62, 0x0A, 0x09, 0x00, 0x00, 0x13, 0x43};
    Serial1.write(poll, sizeof(poll));
    const uint32_t end = millis() + 1500;
    while (millis() < end) {
      while (Serial1.available()) Serial.printf("%02X ", Serial1.read());
      delay(1);
    }
    Serial.println();
    return;
  }
  if (command == 'n') {
    const uint32_t end = millis() + 3000;
    while (millis() < end) {
      while (Serial1.available()) Serial.write(Serial1.read());
      delay(1);
    }
    return;
  }
  if (command != 'r') return;
  Serial.printf("DIAG screen=0x%02X on=%d dim=%d wheel_ab=%u%u position=%d page=%u field=%u hue=%u white=%d brightness=%u mode=%u\n",
    screenAddress, int(screenOn), int(screenDimmed), (wheelStable >> 1) & 1, wheelStable & 1, wheelPosition,
    unsigned(page), unsigned(field), hue, int(white), brightness, unsigned(lightMode));
  Serial.printf("DIAG gps_chars=%lu sentences_ok=%lu failed=%lu sats=%lu fix=%d charging=%d charged=%d\n",
    (unsigned long)gps.charsProcessed(), (unsigned long)gps.passedChecksum(), (unsigned long)gps.failedChecksum(),
    (unsigned long)(gps.satellites.isValid() ? gps.satellites.value() : 0), int(gps.location.isValid()),
    int(isCharging()), int(isCharged()));
  Serial.printf("DIAG time=%d src=%u local=%lld utc_offset=%d loc=%d src=%u lat=%.4f lon=%.4f sun_up=%d lit=%d\n",
    int(timeKnown), unsigned(timeSource), (long long)(timeKnown ? localNow() : 0), utcOffsetHours,
    int(locationKnown), unsigned(locationSource), latitude, longitude, int(sunUp), int(lampLit(millis())));
  Serial.printf("DIAG rtc=%d last_saved=%lld\n", int(rtcPresent), (long long)settings.getLong64("lasttime", 0));
  Serial.printf("DIAG adaptive=%d kelvin=%u level=%.2f rgb=%06lX\n",
    int(adaptiveActive()), adaptiveKelvin, adaptiveLevel, (unsigned long)adaptiveRgb);
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
  pinMode(CHARGING_PIN, INPUT_PULLUP);
  pinMode(CHARGED_PIN, INPUT_PULLUP);
  wheelStable = readWheel();
  Serial1.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  settings.begin("led", false);
  hue = settings.getUShort("hue", 0);
  white = settings.getBool("white", false);
  brightness = max(MIN_BRIGHTNESS, settings.getUChar("bright", 60));
  lightMode = LightMode(settings.getUChar("mode", 0) % LIGHT_MODE_COUNT);
  utcOffsetHours = int8_t(constrain(int(settings.getChar("utc", 3)), -12, 14));
  locationKnown = settings.getBool("hasloc", false);
  if (locationKnown) locationSource = Source::Saved;
  latitude = settings.getFloat("lat", 0);
  longitude = settings.getFloat("lon", 0);

  led.begin();
  showColor();
  setupScreen();  // also starts the I2C bus
  setupClock(millis());
  page = homePage();

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
    if (locationSource == Source::Manual) {
      settings.putFloat("lat", latitude);
      settings.putFloat("lon", longitude);
      settings.putBool("hasloc", true);
    }
    if (rtcWriteNeeded) {
      if (rtcPresent) rtcWrite(utcNow());
      rtcWriteNeeded = false;
      lastTimeSaveMs = 0;  // store the new time below right away
    }
    saveNeeded = false;
  }

  if (timeKnown && (lastTimeSaveMs == 0 || now - lastTimeSaveMs >= LAST_TIME_SAVE_MS)) {
    settings.putLong64("lasttime", utcNow());
    lastTimeSaveMs = now;
  }

  // Also applies schedule and preview changes within half a second.
  static uint32_t lastRefreshMs = 0;
  if (now - lastRefreshMs >= 500) {
    lastRefreshMs = now;
    showColor();
  }

  delay(1);
}
