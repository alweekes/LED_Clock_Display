#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>
#include <FastLED.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

#define DATA_PIN 16
#define NUM_LEDS 241
#define LED_TYPE WS2812B
#define COLOR_ORDER GRB
#define BRIGHTNESS 80

// POSIX TZ string, editable later via the WiFi setup portal.
// Handles Europe/London GMT/BST DST switch automatically.
#define DEFAULT_TZ "GMT0BST,M3.5.0/1,M10.5.0"

// Physical layout: 8 concentric rings + a single center dot, wired outer to
// inner, each ring traversed fully before moving inward.
//   Ring 1 (outer, 60 LEDs): indices   0..59
//   Ring 2 (48 LEDs):        indices  60..107
//   Ring 3 (40 LEDs):        indices 108..147
//   Ring 4 (32 LEDs):        indices 148..179
//   Ring 5 (24 LEDs):        indices 180..203
//   Ring 6 (16 LEDs):        indices 204..219
//   Ring 7 (12 LEDs):        indices 220..231
//   Ring 8 (8 LEDs):         indices 232..239
//   Center (1 LED):          index 240
#define CENTER_INDEX 240

struct RingDef {
  int start;
  int size;
};
// Outer (index 0) to inner (index 7).
const RingDef ALL_RINGS[8] = {
    {0, 60}, {60, 48}, {108, 40}, {148, 32}, {180, 24}, {204, 16}, {220, 12}, {232, 8},
};

// Hour/minute/second are continuous sweeps (fine on an unmarked ring).
// Month/day/forecast are discrete step indicators instead: each lights
// exactly one LED per integer value, meant to line up with fixed printed
// numbers/hour-marks on a 3D-printed bezel, so they must NOT be a
// continuous/anti-aliased sweep.
//   Ring 5 (24 LEDs) is an exact 1:1 fit for a 24-hour weather forecast, so
//   the hour hand (a continuous sweep, fine on any ring size) moved to
//   ring 2 (48 LEDs) to free it up.
//   Ring 7 (12 LEDs) is an exact 1:1 fit for month (Jan..Dec).
//   Ring 4 (32 LEDs) is the closest fit for day-of-month (1..31); index 31
//   (the 32nd LED) is simply never lit.
// Rings 6, 8 (16, 8 LEDs) are currently unused.
#define SECOND_HAND_RING 0  // ring1, 60 LEDs
#define HOUR_HAND_RING 1    // ring2, 48 LEDs
#define MINUTE_HAND_RING 2  // ring3, 40 LEDs
#define DAY_RING 3          // ring4, 32 LEDs -- day of month, 31 positions used
#define FORECAST_RING 4     // ring5, 24 LEDs -- next 24 hours' weather
#define MONTH_RING 6        // ring7, 12 LEDs -- month, all 12 positions used

CRGB leds[NUM_LEDS];
Preferences prefs;
String tzString;
WiFiManagerParameter* tzParam;
WebServer webServer(80);

// User-configurable via the web UI at http://<device-ip>/ (or
// http://ledclock.local/), persisted in NVS. Brightness is a 0-255 scale
// applied on top of the picked color, so it can be dimmed without having to
// re-pick a darker color.
CRGB hourColor(150, 0, 0);
CRGB minuteColor(0, 150, 0);
CRGB secondColor(0, 60, 255);
CRGB monthColor(200, 120, 0);
CRGB dayColor(0, 150, 150);
uint8_t handBrightness = 255;
float weatherLat = 51.5074;   // default: London
float weatherLon = -0.1278;

uint32_t packColor(CRGB c) {
  return ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | c.b;
}

CRGB unpackColor(uint32_t v) {
  return CRGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}

String colorToHex(CRGB c) {
  char buf[8];
  snprintf(buf, sizeof(buf), "#%02X%02X%02X", c.r, c.g, c.b);
  return String(buf);
}

CRGB hexToColor(const String& hex, CRGB fallback) {
  if (hex.length() != 7 || hex[0] != '#') return fallback;
  long v = strtol(hex.c_str() + 1, NULL, 16);
  return CRGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}

void loadColorSettings() {
  prefs.begin("clock", true);
  hourColor = unpackColor(prefs.getUInt("hourRGB", packColor(hourColor)));
  minuteColor = unpackColor(prefs.getUInt("minRGB", packColor(minuteColor)));
  secondColor = unpackColor(prefs.getUInt("secRGB", packColor(secondColor)));
  monthColor = unpackColor(prefs.getUInt("monRGB", packColor(monthColor)));
  dayColor = unpackColor(prefs.getUInt("dayRGB", packColor(dayColor)));
  handBrightness = prefs.getUChar("handBri", handBrightness);
  weatherLat = prefs.getFloat("wLat", weatherLat);
  weatherLon = prefs.getFloat("wLon", weatherLon);
  prefs.end();
}

void saveColorSettings() {
  prefs.begin("clock", false);
  prefs.putUInt("hourRGB", packColor(hourColor));
  prefs.putUInt("minRGB", packColor(minuteColor));
  prefs.putUInt("secRGB", packColor(secondColor));
  prefs.putUInt("monRGB", packColor(monthColor));
  prefs.putUInt("dayRGB", packColor(dayColor));
  prefs.putUChar("handBri", handBrightness);
  prefs.putFloat("wLat", weatherLat);
  prefs.putFloat("wLon", weatherLon);
  prefs.end();
}

const char PAGE_TEMPLATE[] = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>LED Clock</title>
<style>
body{font-family:sans-serif;background:#111;color:#eee;max-width:420px;margin:0 auto;padding:16px}
h1{font-size:1.3em}
label{display:block;margin-top:18px;font-weight:bold}
input[type=color]{width:100%;height:44px;border:none;background:none;margin-top:6px}
input[type=range]{width:100%;margin-top:6px}
input[type=number]{width:100%;padding:8px;margin-top:6px;background:#222;color:#eee;border:1px solid #444;border-radius:4px;box-sizing:border-box}
.row{display:flex;justify-content:space-between;align-items:center}
button{margin-top:24px;width:100%;padding:12px;font-size:1em;background:#3a7;
  color:#fff;border:none;border-radius:6px}
</style></head><body>
<h1>LED Clock Settings</h1>
<form method="POST" action="/save">
<label>Hour hand color
<input type="color" name="hourColor" value="%HOUR_COLOR%"></label>
<label>Minute hand color
<input type="color" name="minuteColor" value="%MINUTE_COLOR%"></label>
<label>Second hand color
<input type="color" name="secondColor" value="%SECOND_COLOR%"></label>
<label>Month indicator color
<input type="color" name="monthColor" value="%MONTH_COLOR%"></label>
<label>Day indicator color
<input type="color" name="dayColor" value="%DAY_COLOR%"></label>
<label>Brightness
<input type="range" name="handBrightness" min="0" max="255" value="%HAND_BRI%"></label>
<label>Weather latitude
<input type="number" step="0.0001" name="weatherLat" value="%WEATHER_LAT%"></label>
<label>Weather longitude
<input type="number" step="0.0001" name="weatherLon" value="%WEATHER_LON%"></label>
<button type="submit">Save</button>
</form>
</body></html>
)rawliteral";

void handleRoot() {
  String page = PAGE_TEMPLATE;
  page.replace("%HOUR_COLOR%", colorToHex(hourColor));
  page.replace("%MINUTE_COLOR%", colorToHex(minuteColor));
  page.replace("%SECOND_COLOR%", colorToHex(secondColor));
  page.replace("%MONTH_COLOR%", colorToHex(monthColor));
  page.replace("%DAY_COLOR%", colorToHex(dayColor));
  page.replace("%HAND_BRI%", String(handBrightness));
  page.replace("%WEATHER_LAT%", String(weatherLat, 4));
  page.replace("%WEATHER_LON%", String(weatherLon, 4));
  webServer.send(200, "text/html", page);
}

bool weatherNeedsRefresh = false;

void handleSave() {
  hourColor = hexToColor(webServer.arg("hourColor"), hourColor);
  minuteColor = hexToColor(webServer.arg("minuteColor"), minuteColor);
  secondColor = hexToColor(webServer.arg("secondColor"), secondColor);
  monthColor = hexToColor(webServer.arg("monthColor"), monthColor);
  dayColor = hexToColor(webServer.arg("dayColor"), dayColor);
  handBrightness = (uint8_t)webServer.arg("handBrightness").toInt();

  float newLat = webServer.arg("weatherLat").toFloat();
  float newLon = webServer.arg("weatherLon").toFloat();
  if (newLat != weatherLat || newLon != weatherLon) {
    weatherLat = newLat;
    weatherLon = newLon;
    weatherNeedsRefresh = true;  // location changed; re-fetch on next loop()
  }

  saveColorSettings();

  webServer.sendHeader("Location", "/");
  webServer.send(303);
}

void setupWebServer() {
  webServer.on("/", HTTP_GET, handleRoot);
  webServer.on("/save", HTTP_POST, handleSave);
  webServer.begin();

  if (MDNS.begin("ledclock")) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("Web UI: http://ledclock.local/");
  }
}

// Lights a single LED in one ring at the given fraction (0..1) around that
// ring's circumference. Used for the continuously-sweeping hands.
void drawDot(int ringIdx, float frac, CRGB color) {
  int start = ALL_RINGS[ringIdx].start;
  int size = ALL_RINGS[ringIdx].size;

  int idx = ((int)roundf(frac * size)) % size;
  if (idx < 0) idx += size;

  leds[start + idx] += color;
}

// Lights a single LED in one ring at an exact integer step (0-based), for
// the month/day indicators -- no rounding/anti-aliasing, since each step
// must land exactly on a fixed printed number on the bezel.
void drawStep(int ringIdx, int step, CRGB color) {
  int start = ALL_RINGS[ringIdx].start;
  int size = ALL_RINGS[ringIdx].size;
  if (step < 0 || step >= size) return;  // e.g. day 32 on the 32-LED ring
  leds[start + step] += color;
}

void saveConfigCallback() {
  tzString = tzParam->getValue();
  prefs.begin("clock", false);
  prefs.putString("tz", tzString);
  prefs.end();
  configTzTime(tzString.c_str(), "pool.ntp.org", "time.nist.gov");
}

void setupWiFi() {
  prefs.begin("clock", true);
  tzString = prefs.getString("tz", DEFAULT_TZ);
  prefs.end();

  WiFiManager wm;
  static WiFiManagerParameter tzp(
      "tz", "POSIX TZ string (see bit.ly/posixtz)", tzString.c_str(), 64);
  tzParam = &tzp;
  wm.addParameter(&tzp);
  wm.setSaveConfigCallback(saveConfigCallback);

  // On first boot (or after resetwifi), opens AP "LED-Clock-Setup" with a
  // captive portal so WiFi + timezone can be entered from a phone.
  if (!wm.autoConnect("LED-Clock-Setup")) {
    Serial.println("WiFi connect failed, restarting...");
    delay(1000);
    ESP.restart();
  }

  configTzTime(tzString.c_str(), "pool.ntp.org", "time.nist.gov");
}

// 24-hour forecast, indexed 0 = the current hour. weatherCode[i] == -1 means
// "no data yet" (before the first successful fetch, or a fetch failed and
// we're still showing the last known-good data).
#define FORECAST_HOURS 24
int forecastWeatherCode[FORECAST_HOURS];
int forecastPrecipProb[FORECAST_HOURS];
unsigned long lastWeatherFetch = 0;
#define WEATHER_FETCH_INTERVAL_MS (15UL * 60UL * 1000UL)  // 15 minutes

// Maps an Open-Meteo/WMO weather code to a color for the forecast ring.
// https://open-meteo.com/en/docs -- WMO Weather interpretation codes.
CRGB weatherCodeColor(int code) {
  if (code == 0) return CRGB(255, 170, 0);          // clear sky
  if (code <= 3) return CRGB(140, 140, 150);        // partly cloudy/overcast
  if (code == 45 || code == 48) return CRGB(170, 170, 170);  // fog
  if (code >= 51 && code <= 57) return CRGB(80, 160, 255);   // drizzle
  if (code >= 61 && code <= 67) return CRGB(30, 90, 255);    // rain
  if (code >= 71 && code <= 77) return CRGB(220, 220, 255);  // snow
  if (code >= 80 && code <= 82) return CRGB(30, 90, 255);    // rain showers
  if (code >= 85 && code <= 86) return CRGB(220, 220, 255);  // snow showers
  if (code >= 95) return CRGB(160, 0, 220);                  // thunderstorm
  return CRGB(80, 80, 80);                                    // unknown code
}

// Fetches the next 24 hours of forecast from Open-Meteo (no API key
// required) for the configured lat/lon. On any failure, leaves the
// previous forecast data in place rather than blanking the ring.
void fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();  // no cert bundle on-device; acceptable for a hobby project

  HTTPClient http;
  char url[256];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&hourly=weathercode,precipitation_probability&forecast_days=2&timezone=auto",
           weatherLat, weatherLon);

  if (!http.begin(client, url)) {
    Serial.println("Weather: http.begin failed");
    return;
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("Weather: HTTP GET failed, code=%d\n", code);
    http.end();
    return;
  }

  // Buffer the full body first rather than parsing from http.getStream()
  // directly -- streaming parse was unreliable here (likely a chunked
  // transfer-encoding/TLS interaction), returning InvalidInput.
  String body = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.printf("Weather: JSON parse failed: %s (body length %d)\n",
                  err.c_str(), body.length());
    Serial.println(body.substring(0, 200));
    return;
  }

  JsonArray codes = doc["hourly"]["weathercode"];
  JsonArray precip = doc["hourly"]["precipitation_probability"];
  if (codes.isNull() || precip.isNull()) {
    Serial.println("Weather: unexpected response shape");
    return;
  }

  // The API (with timezone=auto) returns hourly data starting at local
  // midnight of today; the current hour is simply that offset into it.
  time_t nowSec = time(NULL);
  struct tm timeinfo;
  localtime_r(&nowSec, &timeinfo);
  int startIdx = timeinfo.tm_hour;

  for (int i = 0; i < FORECAST_HOURS; i++) {
    int srcIdx = startIdx + i;
    if (srcIdx < (int)codes.size()) {
      forecastWeatherCode[i] = codes[srcIdx].as<int>();
      forecastPrecipProb[i] = precip[srcIdx].as<int>();
    }
  }

  lastWeatherFetch = millis();
  Serial.println("Weather: forecast updated");
}

// subSec (0..1) must come from the same clock read as timeinfo.tm_sec --
// mixing tm_sec (RTC/NTP-corrected) with millis() (a separate free-running
// timer) lets the two drift apart and makes the sub-second phase
// occasionally step backward.
void renderClock(const struct tm& timeinfo, float subSec) {
  fill_solid(leds, NUM_LEDS, CRGB::Black);

  float secWithinMin = timeinfo.tm_sec + subSec;
  float minWithinHour = timeinfo.tm_min + secWithinMin / 60.0f;

  float secFrac = secWithinMin / 60.0f;
  float minFrac = minWithinHour / 60.0f;
  float hourFrac = ((timeinfo.tm_hour % 12) + minWithinHour / 60.0f) / 12.0f;

  CRGB hourC = hourColor;
  hourC.nscale8_video(handBrightness);
  CRGB minuteC = minuteColor;
  minuteC.nscale8_video(handBrightness);
  CRGB secondC = secondColor;
  secondC.nscale8_video(handBrightness);

  drawDot(HOUR_HAND_RING, hourFrac, hourC);
  drawDot(MINUTE_HAND_RING, minFrac, minuteC);
  drawDot(SECOND_HAND_RING, secFrac, secondC);

  CRGB monthC = monthColor;
  monthC.nscale8_video(handBrightness);
  CRGB dayC = dayColor;
  dayC.nscale8_video(handBrightness);

  drawStep(MONTH_RING, timeinfo.tm_mon, monthC);      // tm_mon: 0=Jan..11=Dec
  drawStep(DAY_RING, timeinfo.tm_mday - 1, dayC);     // tm_mday: 1..31

  // 24-hour forecast: color = condition, brightness = chance of rain.
  for (int i = 0; i < FORECAST_HOURS; i++) {
    if (forecastWeatherCode[i] < 0) continue;  // no data yet for this hour
    CRGB c = weatherCodeColor(forecastWeatherCode[i]);
    uint8_t precipBrightness =
        map(forecastPrecipProb[i], 0, 100, 150, 255);
    c.nscale8_video(precipBrightness);
    c.nscale8_video(handBrightness);
    drawStep(FORECAST_RING, i, c);
  }

  // Center hub: always dimly lit, pulsing brighter once per second.
  uint8_t pulse = (uint8_t)(60 + 195 * (1.0f - subSec));
  leds[CENTER_INDEX] += CRGB(pulse, pulse, pulse);

  FastLED.show();
}

void setup() {
  Serial.begin(115200);

  FastLED.addLeds<LED_TYPE, DATA_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.setBrightness(BRIGHTNESS);
  FastLED.clear();
  FastLED.show();

  for (int i = 0; i < FORECAST_HOURS; i++) forecastWeatherCode[i] = -1;

  loadColorSettings();
  setupWiFi();
  setupWebServer();

  Serial.println("Waiting for NTP time sync...");
  struct tm timeinfo;
  while (!getLocalTime(&timeinfo)) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nTime synced.");

  fetchWeather();  // NTP must be synced first, so the current-hour offset
                    // into the forecast response is correct.
}

void loop() {
  webServer.handleClient();

  if (weatherNeedsRefresh ||
      millis() - lastWeatherFetch >= WEATHER_FETCH_INTERVAL_MS) {
    weatherNeedsRefresh = false;
    fetchWeather();
  }

  static unsigned long lastUpdate = 0;
  unsigned long now = millis();
  if (now - lastUpdate >= 10) {
    lastUpdate = now;

    struct timeval tv;
    gettimeofday(&tv, NULL);
    time_t nowSec = tv.tv_sec;
    struct tm timeinfo;
    localtime_r(&nowSec, &timeinfo);
    float subSec = tv.tv_usec / 1000000.0f;

    renderClock(timeinfo, subSec);
  }

  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd == "resetwifi") {
      WiFiManager wm;
      wm.resetSettings();
      Serial.println("WiFi settings cleared. Restarting...");
      delay(500);
      ESP.restart();
    }
  }
}
