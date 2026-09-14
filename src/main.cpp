// =============================================================================
// LED Clock Display — firmware for an ESP32 driving a 241-LED WS2812B
// concentric-ring panel as an analog-style clock.
//
// WHAT THIS PROGRAM DOES, IN PLAIN TERMS
// ---------------------------------------------------------------------------
// The LED panel is arranged as 8 rings, one inside the other (like a
// dartboard), plus a single LED right in the center. Every ~10 milliseconds
// this program:
//   1. Asks the ESP32's clock what time it is.
//   2. Works out where the hour/minute/second "hands" should point, and
//      lights exactly one LED on each of three rings to represent them.
//   3. Lights one LED on another ring to show the current month, and one on
//      another ring to show the day of the month.
//   4. Lights up to 24 LEDs on one ring to show the next 24 hours of
//      weather forecast (fetched from the internet every 15 minutes).
//   5. Pulses the single center LED once a second, like a heartbeat.
//   6. Sends all 241 colors down one wire to the LED panel.
//
// It also runs a tiny web server so you can open a page in your browser and
// change the colors/brightness without re-uploading the firmware, and it
// uses a library called WiFiManager to make first-time WiFi setup possible
// from a phone, with no WiFi password ever hard-coded into the source.
//
// HOW THE FILE IS ORGANIZED (top to bottom)
// ---------------------------------------------------------------------------
//   - Hardware constants & pin numbers
//   - The LED ring layout (which physical LEDs belong to which ring)
//   - User settings (colors/brightness/location) + saving/loading them
//   - The web configuration page (HTML) and its request handlers
//   - Small helper functions for turning on individual LEDs
//   - WiFi setup (the captive portal)
//   - Weather fetching (talks to a weather website over the internet)
//   - renderClock() -- the function that decides what every LED should look
//     like right now, called continuously from loop()
//   - setup() -- runs once at power-on
//   - loop() -- runs forever, over and over, after setup() finishes
//
// If you're new to Arduino/ESP32 programming: every Arduino-style program
// has exactly two functions the hardware calls for you -- setup() runs once
// when the board powers on, and loop() then runs repeatedly, forever, many
// times per second. Everything else in this file is just regular functions
// that setup() and loop() (directly or indirectly) call.
// =============================================================================

#include <Arduino.h>       // Core Arduino functions (pinMode, delay, Serial, etc.)
#include <WiFi.h>          // ESP32's built-in WiFi radio
#include <WiFiManager.h>   // Lets the board offer a "connect to my WiFi" web page
                           // of its own, instead of hard-coding a WiFi password
#include <WebServer.h>     // A basic web server, so this board can host web pages
#include <ESPmDNS.h>       // Lets the board be reached as "ledclock.local"
                           // instead of typing its numeric IP address
#include <Preferences.h>   // Saves small settings to flash memory that survive
                           // a power cut (this is often called "NVS" on ESP32)
#include <time.h>          // Standard C library for working with dates/times
#include <sys/time.h>      // gettimeofday() -- reads the clock with fractions
                           // of a second, not just whole seconds
#include <FastLED.h>       // The library that actually talks to the WS2812B
                           // LEDs and lets us set each one's color
#include <HTTPClient.h>    // Lets the board make web requests (like a tiny browser)
#include <WiFiClientSecure.h>  // The "https://" (encrypted) version of a
                               // network connection, needed for the weather API
#include <ArduinoJson.h>   // Reads/writes JSON, the text format the weather
                           // API sends its data back in

// -----------------------------------------------------------------------
// Hardware constants
// -----------------------------------------------------------------------
#define DATA_PIN 16     // The ESP32 pin wired to the LED panel's data input
#define NUM_LEDS 241    // Total LEDs on the panel (8 rings + 1 center dot)
#define LED_TYPE WS2812B    // The exact chip inside each LED, so FastLED knows
                            // the timing/protocol to use
#define COLOR_ORDER GRB     // Some LED strips expect colors in the order
                            // Green-Red-Blue rather than Red-Green-Blue; this
                            // panel is wired for GRB
#define BRIGHTNESS 80       // A global brightness cap (0-255) applied to every
                            // LED, mostly to keep power draw sane

// A POSIX "TZ string" tells the C time library both the UTC offset *and* the
// daylight-saving rules for a region, so clocks/dates stay correct across
// DST changes automatically. This default is Europe/London (GMT in winter,
// British Summer Time in summer); it's editable later from a phone during
// WiFi setup, so nothing about your location needs to be hard-coded here.
#define DEFAULT_TZ "GMT0BST,M3.5.0/1,M10.5.0"

// -----------------------------------------------------------------------
// LED ring layout
// -----------------------------------------------------------------------
// FastLED sees the whole panel as one long strip of 241 LEDs, numbered 0 to
// 240 in the order they're wired. Physically, though, they're arranged as 8
// concentric rings (like a dartboard) plus one LED dead center, wired so
// that ring 1 (the outermost) is numbered first from LED 0, then ring 2
// continues where ring 1 left off, and so on inward:
//   Ring 1 (outer, 60 LEDs): indices   0..59
//   Ring 2 (48 LEDs):        indices  60..107
//   Ring 3 (40 LEDs):        indices 108..147
//   Ring 4 (32 LEDs):        indices 148..179
//   Ring 5 (24 LEDs):        indices 180..203
//   Ring 6 (16 LEDs):        indices 204..219
//   Ring 7 (12 LEDs):        indices 220..231
//   Ring 8 (8 LEDs):         indices 232..239
//   Center (1 LED):          index 240
//
// Working out which of the 241 numbers corresponds to "the 5th LED of ring
// 4" by hand every time would be error-prone, so instead we store, for each
// ring, where it *starts* in that long strip and how many LEDs it has. Then
// "LED number N of ring R" is simply `ring[R].start + N`.
#define CENTER_INDEX 240

struct RingDef {
  int start;  // index of this ring's first LED in the `leds[]` array
  int size;   // how many LEDs are in this ring
};
// Outer (array index 0) to inner (array index 7). This does NOT include the
// center LED -- that's handled separately via CENTER_INDEX, since it isn't
// really a "ring" (it's a single point with no direction/angle).
const RingDef ALL_RINGS[8] = {
    {0, 60}, {60, 48}, {108, 40}, {148, 32}, {180, 24}, {204, 16}, {220, 12}, {232, 8},
};

// Each of the 8 rings has been given a job. The hour/minute/second hands are
// *continuous sweeps* -- their LED position is calculated as a fraction (for
// example, "37% of the way around this ring") and rounded to the nearest
// LED, which looks fine on a plain, unmarked ring since there's nothing for
// a slightly-off position to visibly misalign with.
//
// Month/day/forecast are different: they're *discrete step indicators*,
// meant to eventually sit behind a 3D-printed bezel with actual numbers
// printed on it (e.g. "1" through "31" around the day ring). For that to
// work, LED number 5 must ALWAYS be exactly where "day 6" is printed --
// there's no room for the continuous-sweep rounding-to-nearest-LED approach,
// so these use a plain integer index instead (see drawStep() below).
//
//   Ring 5 (24 LEDs) is an exact 1-LED-per-hour fit for a 24-hour weather
//   forecast, so the hour hand (happy on any ring size) moved to ring 2
//   (48 LEDs) to free ring 5 up for that.
//   Ring 7 (12 LEDs) is an exact fit for the 12 months of the year.
//   Ring 4 (32 LEDs) is the closest fit for day-of-month (which needs 31
//   positions); the 32nd LED position is simply never lit, since there's
//   never a "day 32".
// Rings 6 and 8 (16 and 8 LEDs) aren't assigned to anything yet.
#define SECOND_HAND_RING 0  // ring1, 60 LEDs
#define HOUR_HAND_RING 1    // ring2, 48 LEDs
#define MINUTE_HAND_RING 2  // ring3, 40 LEDs
#define DAY_RING 3          // ring4, 32 LEDs -- day of month, 31 positions used
#define FORECAST_RING 4     // ring5, 24 LEDs -- next 24 hours' weather
#define MONTH_RING 6        // ring7, 12 LEDs -- month, all 12 positions used

// `leds[]` is the array FastLED actually reads from to know what color to
// send to each physical LED. Every function in this file that "turns on an
// LED" is really just writing a color into one slot of this array; nothing
// visibly changes on the panel until FastLED.show() is called, which sends
// the whole array down the wire in one go (see the end of renderClock()).
CRGB leds[NUM_LEDS];

Preferences prefs;             // Handle used to read/write saved settings (NVS)
String tzString;                // The currently active POSIX timezone string
WiFiManagerParameter* tzParam;  // A custom field WiFiManager adds to its setup
                                // page, so the timezone can be entered
                                // alongside the WiFi password
WebServer webServer(80);        // Our own web server, listening on port 80
                                // (the standard port for http:// addresses)

// -----------------------------------------------------------------------
// User settings
// -----------------------------------------------------------------------
// Everything below is editable from the web page at http://<device-ip>/ (or
// http://ledclock.local/) and is saved to flash memory (NVS) so it survives
// a reboot or power cut. `handBrightness` is a single 0-255 dimmer applied
// on top of whatever color is picked, so you can dim the whole display
// without having to re-pick every color as a darker shade.
CRGB hourColor(150, 0, 0);      // dim red
CRGB minuteColor(0, 150, 0);    // dim green
CRGB secondColor(0, 60, 255);   // blue
CRGB monthColor(200, 120, 0);   // amber
CRGB dayColor(0, 150, 150);     // teal
uint8_t handBrightness = 255;
float weatherLat = 51.5074;   // default: London
float weatherLon = -0.1278;

// ---- Small helpers for converting colors to/from the forms we need to
// ---- store them (a single number) or show them in a web page ("#rrggbb").

// Squashes a color's red/green/blue components into one 32-bit number, so
// Preferences (which only knows how to save simple number/string types) can
// store a whole color under a single key. This is a common trick: shifting
// red left by 16 bits and green left by 8 bits lines them up like
// 0xRRGGBB, then bitwise-OR-ing combines them into one value.
uint32_t packColor(CRGB c) {
  return ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | c.b;
}

// The reverse of packColor(): pulls the red/green/blue bytes back out of a
// packed 0xRRGGBB-style number.
CRGB unpackColor(uint32_t v) {
  return CRGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}

// Formats a color as the "#rrggbb" hex string that HTML's
// <input type="color"> expects, e.g. CRGB(255, 0, 0) -> "#FF0000".
String colorToHex(CRGB c) {
  char buf[8];
  snprintf(buf, sizeof(buf), "#%02X%02X%02X", c.r, c.g, c.b);
  return String(buf);
}

// The reverse of colorToHex(): parses a "#rrggbb" string (as submitted by
// the web page's color picker) back into a CRGB color. If the text doesn't
// look like a valid hex color, it just returns whatever color was already
// in use, so a malformed request can't corrupt the current settings.
CRGB hexToColor(const String& hex, CRGB fallback) {
  if (hex.length() != 7 || hex[0] != '#') return fallback;
  long v = strtol(hex.c_str() + 1, NULL, 16);  // parse the hex digits after '#'
  return CRGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}

// Reads every saved setting from flash (NVS) into the variables above. If a
// setting has never been saved before (e.g. the very first boot), the
// second argument to each getXxx() call is used as the default instead.
void loadColorSettings() {
  prefs.begin("clock", true);  // true = open read-only
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

// Writes every current setting out to flash (NVS), so it's remembered next
// time the board boots. Called whenever the web page's Save button is used.
void saveColorSettings() {
  prefs.begin("clock", false);  // false = open read-write
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

// -----------------------------------------------------------------------
// The web configuration page
// -----------------------------------------------------------------------
// This is a complete (if small) web page written as one big piece of text.
// The %PLACEHOLDER% markers get swapped out for real values in handleRoot()
// below, just before the page is sent to whoever's browser asked for it.
// R"rawliteral( ... )rawliteral" is C++'s way of writing a multi-line string
// literally, without needing to escape every quote mark inside the HTML.
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

// Called by the web server whenever a browser requests the page "/" (i.e.
// whoever just typed the device's address into their browser). Builds the
// page by filling in the current settings, then sends it back.
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
  webServer.send(200, "text/html", page);  // 200 = the standard "OK" HTTP status
}

// Set to true when the saved location changes, so loop() knows to fetch a
// fresh weather forecast immediately instead of waiting for the next
// scheduled 15-minute refresh.
bool weatherNeedsRefresh = false;

// Called by the web server when the form on the settings page is submitted
// (a browser sends this as an HTTP POST request to "/save", carrying the
// new values typed into every field). Reads each submitted value, updates
// our in-memory settings, saves them to flash, then sends the browser back
// to "/" so it sees the page again with the new values already filled in.
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

  // HTTP status 303 tells the browser "the thing you want is over here" --
  // this is the standard way to redirect after a form submission, so
  // reloading the settings page doesn't accidentally resubmit the form.
  webServer.sendHeader("Location", "/");
  webServer.send(303);
}

// Registers our two page handlers with the web server and starts it
// listening, then tries to register the friendly "ledclock.local" name so
// you don't have to remember/type the device's numeric IP address.
void setupWebServer() {
  webServer.on("/", HTTP_GET, handleRoot);
  webServer.on("/save", HTTP_POST, handleSave);
  webServer.begin();

  if (MDNS.begin("ledclock")) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("Web UI: http://ledclock.local/");
  }
}

// -----------------------------------------------------------------------
// LED-lighting helpers
// -----------------------------------------------------------------------

// Lights a single LED in one ring at a given fraction of the way around it.
// `frac` is a number from 0.0 to 1.0 -- for example 0.25 means "a quarter
// of the way around this ring". This is how the continuously-sweeping
// hour/minute/second hands are drawn: as time passes, `frac` increases
// smoothly, and this function works out the nearest whole LED to light.
//
// `+=` (rather than `=`) is used so that if two things try to light the
// same LED in the same frame, their colors add together instead of one
// overwriting the other -- e.g. red + green light would blend toward
// yellow, rather than only the second one drawn being visible.
void drawDot(int ringIdx, float frac, CRGB color) {
  int start = ALL_RINGS[ringIdx].start;
  int size = ALL_RINGS[ringIdx].size;

  int idx = ((int)roundf(frac * size)) % size;
  if (idx < 0) idx += size;  // keep the index positive even if frac was negative

  leds[start + idx] += color;
}

// Lights a single LED in one ring at an exact whole-number position (0-based
// counting: the first LED in the ring is step 0, not step 1). Used for the
// month/day indicators, which must always land on the exact same physical
// LED for a given value -- unlike drawDot() above, there's no rounding to
// "the nearest" LED, because there's only ever one correct LED to light.
void drawStep(int ringIdx, int step, CRGB color) {
  int start = ALL_RINGS[ringIdx].start;
  int size = ALL_RINGS[ringIdx].size;
  if (step < 0 || step >= size) return;  // e.g. day 32 on the 32-LED ring
  leds[start + step] += color;
}

// -----------------------------------------------------------------------
// WiFi setup
// -----------------------------------------------------------------------

// Called by WiFiManager the moment someone finishes filling in its setup
// page (WiFi network + our custom timezone field) and taps Save. We only
// need to handle the timezone field ourselves -- WiFiManager saves the WiFi
// password on its own, internally.
void saveConfigCallback() {
  tzString = tzParam->getValue();
  prefs.begin("clock", false);
  prefs.putString("tz", tzString);
  prefs.end();
  // configTzTime() tells the ESP32's internal clock which timezone rules to
  // use once it syncs with an internet time server (NTP), further down.
  configTzTime(tzString.c_str(), "pool.ntp.org", "time.nist.gov");
}

// Connects to WiFi, or -- if no WiFi details have been saved yet -- puts the
// board into "setup mode" instead: it creates its own temporary WiFi
// network (an "access point") named LED-Clock-Setup, and anyone who
// connects to it from a phone sees a captive portal page (the same kind of
// page you get when joining hotel or airport WiFi) asking for the WiFi
// network to actually use, plus our custom timezone field.
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

  // autoConnect() does the heavy lifting: try the last-saved WiFi network
  // first, and only fall back to the "LED-Clock-Setup" portal if that
  // fails (including the very first boot, when nothing's saved yet). It
  // doesn't return until the board is connected, or setup has failed.
  if (!wm.autoConnect("LED-Clock-Setup")) {
    Serial.println("WiFi connect failed, restarting...");
    delay(1000);
    ESP.restart();
  }

  configTzTime(tzString.c_str(), "pool.ntp.org", "time.nist.gov");
}

// -----------------------------------------------------------------------
// Weather forecast
// -----------------------------------------------------------------------
// These two arrays hold the next 24 hours of forecast, one entry per
// upcoming hour (index 0 = the current hour, index 23 = 23 hours from now).
// A weather code of -1 means "we don't have real data for this hour yet" --
// either the board has only just booted and hasn't fetched anything, or the
// last fetch attempt failed and we're still showing whatever was fetched
// before that (better to show slightly-stale data than blank the ring).
#define FORECAST_HOURS 24
int forecastWeatherCode[FORECAST_HOURS];
int forecastPrecipProb[FORECAST_HOURS];  // % chance of rain, 0-100
unsigned long lastWeatherFetch = 0;
#define WEATHER_FETCH_INTERVAL_MS (15UL * 60UL * 1000UL)  // 15 minutes, in milliseconds

// Turns a weather condition code from the forecast into a color for the
// forecast ring. The codes themselves come from the WMO ("World
// Meteorological Organization") weather code standard, which the Open-Meteo
// API we use also follows -- see https://open-meteo.com/en/docs for the
// full table. We only care about a handful of broad categories here.
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
  return CRGB(80, 80, 80);                                    // unrecognized code
}

// Fetches the next 24 hours of forecast from Open-Meteo (a free weather API
// that, unusually, doesn't require signing up for an API key) for whatever
// latitude/longitude is currently configured, and fills in the two arrays
// above. If anything goes wrong along the way (no WiFi, the website doesn't
// respond, the reply isn't valid data, etc.) the function just gives up and
// returns early -- crucially, it does NOT clear the existing forecast data,
// so the ring keeps showing the last successful forecast rather than going
// blank because of a single failed request.
void fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) return;

  // WiFiClientSecure handles the "https://" (encrypted) side of the
  // connection. setInsecure() skips verifying the website's security
  // certificate -- normally you'd want that check, but it needs a list of
  // trusted certificate authorities stored on the device, which is more
  // setup than a hobby project like this needs.
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;  // Builds and sends the actual web request for us
  char url[256];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&hourly=weathercode,precipitation_probability&forecast_days=2&timezone=auto",
           weatherLat, weatherLon);

  if (!http.begin(client, url)) {
    Serial.println("Weather: http.begin failed");
    return;
  }

  int code = http.GET();  // Actually sends the request and waits for a reply
  if (code != HTTP_CODE_OK) {  // HTTP_CODE_OK is 200, meaning "success"
    Serial.printf("Weather: HTTP GET failed, code=%d\n", code);
    http.end();
    return;
  }

  // Read the whole reply into a String first, then parse it, rather than
  // parsing directly from the network stream as it arrives -- streaming
  // parse turned out to be unreliable here (likely a chunked
  // transfer-encoding/TLS interaction), returning InvalidInput.
  String body = http.getString();
  http.end();

  // The weather website replies with JSON, a common text format for
  // structured data (it looks like nested {"key": value} blocks). JsonDocument
  // is ArduinoJson's in-memory representation of that data once parsed, and
  // deserializeJson() is the function that does the actual parsing.
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.printf("Weather: JSON parse failed: %s (body length %d)\n",
                  err.c_str(), body.length());
    Serial.println(body.substring(0, 200));
    return;
  }

  // Reach into the parsed JSON for the two lists of numbers we actually
  // need: one weather code and one rain-chance percentage per hour.
  JsonArray codes = doc["hourly"]["weathercode"];
  JsonArray precip = doc["hourly"]["precipitation_probability"];
  if (codes.isNull() || precip.isNull()) {
    Serial.println("Weather: unexpected response shape");
    return;
  }

  // We asked for "timezone=auto", so the API replies with hourly data
  // starting at local midnight of today, in the same timezone this clock is
  // set to. That means "the current hour's forecast" is simply entry number
  // `timeinfo.tm_hour` in that list (e.g. at 2pm, that's entry 14).
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

// -----------------------------------------------------------------------
// Rendering -- deciding what every LED should look like right now
// -----------------------------------------------------------------------

// renderClock() is called continuously (see loop(), further down) and does
// all the actual "drawing": it works out where the hands should point, what
// the month/day/forecast indicators should show, and writes all of that
// into the `leds[]` array, before finally sending it to the physical panel.
//
// `timeinfo` is the current date/time, already broken down into separate
// hour/minute/second/day/month fields by the C time library.
//
// `subSec` is a number from 0.0 (the instant a new second began) up to just
// under 1.0 (the instant before the next second begins) -- the "fractional"
// part of the current second, used to make the second hand (and the center
// pulse) move smoothly instead of visibly jumping once per second.
//
// IMPORTANT: `subSec` must be read from the exact same clock query as
// `timeinfo.tm_sec`. It's tempting to instead use millis() (a separate
// free-running counter of milliseconds since the board booted) for the
// fractional part, but millis() and the real clock (which gets corrected by
// NTP) drift apart from each other over time -- mixing the two occasionally
// makes the fractional part step backward for an instant, which is visible
// as a brief stutter. See loop() below for how both values are obtained
// together from a single call to gettimeofday().
void renderClock(const struct tm& timeinfo, float subSec) {
  // Start every frame from a blank panel, then draw everything back on top.
  // This is simpler and less error-prone than trying to individually erase
  // only the LEDs that changed since last time.
  fill_solid(leds, NUM_LEDS, CRGB::Black);

  // Work out how far around each hand's ring it should be, as a fraction
  // from 0.0 to 1.0 (see drawDot() above for what that fraction means).
  float secWithinMin = timeinfo.tm_sec + subSec;            // 0.0 to 60.0
  float minWithinHour = timeinfo.tm_min + secWithinMin / 60.0f;  // 0.0 to 60.0

  float secFrac = secWithinMin / 60.0f;   // 0.0 to 1.0 around the second ring
  float minFrac = minWithinHour / 60.0f;  // 0.0 to 1.0 around the minute ring
  // tm_hour counts 0-23; "% 12" converts that to a 12-hour clock face, and
  // adding the minutes-so-far makes the hour hand creep forward smoothly
  // through the hour instead of jumping on the hour, like a real clock.
  float hourFrac = ((timeinfo.tm_hour % 12) + minWithinHour / 60.0f) / 12.0f;

  // nscale8_video() scales a color's brightness down by a 0-255 amount
  // (255 = unchanged, 0 = off) without ever quite rounding a dim-but-lit
  // color down to fully off -- useful so a low brightness setting still
  // shows *something* rather than disappearing entirely.
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

  // tm_mon already counts 0=January..11=December, which conveniently
  // matches drawStep()'s 0-based LED numbering directly. tm_mday counts
  // 1..31, so we subtract 1 to line day 1 up with LED position 0.
  drawStep(MONTH_RING, timeinfo.tm_mon, monthC);
  drawStep(DAY_RING, timeinfo.tm_mday - 1, dayC);

  // 24-hour forecast ring: each of the 24 LEDs represents one upcoming
  // hour. The LED's color shows the weather condition; how bright that
  // color is shows how likely rain is (map() rescales the 0-100% chance
  // onto a 150-255 brightness range, so even a 0% chance still shows some
  // color, rather than fading all the way to black).
  for (int i = 0; i < FORECAST_HOURS; i++) {
    if (forecastWeatherCode[i] < 0) continue;  // no data yet for this hour
    CRGB c = weatherCodeColor(forecastWeatherCode[i]);
    uint8_t precipBrightness =
        map(forecastPrecipProb[i], 0, 100, 150, 255);
    c.nscale8_video(precipBrightness);
    c.nscale8_video(handBrightness);
    drawStep(FORECAST_RING, i, c);
  }

  // The center LED is always at least dimly lit, and pulses brighter once
  // per second: brightest right as a new second begins (subSec near 0),
  // fading back down as the second progresses (subSec approaching 1).
  uint8_t pulse = (uint8_t)(60 + 195 * (1.0f - subSec));
  leds[CENTER_INDEX] += CRGB(pulse, pulse, pulse);

  // Everything above only changed the `leds[]` array in the ESP32's memory
  // -- nothing on the physical panel updates until FastLED.show() sends
  // that whole array down the data wire.
  FastLED.show();
}

// -----------------------------------------------------------------------
// setup() -- runs once, when the board first powers on (or resets)
// -----------------------------------------------------------------------
void setup() {
  Serial.begin(115200);  // Start the USB/serial connection used for debug logging

  // Tell FastLED which pin the LEDs are wired to, how many there are, and
  // what protocol/color order they expect, then blank the panel.
  FastLED.addLeds<LED_TYPE, DATA_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.setBrightness(BRIGHTNESS);
  FastLED.clear();
  FastLED.show();

  // Mark every forecast hour as "no data yet" until the first successful
  // fetch further down fills in real numbers.
  for (int i = 0; i < FORECAST_HOURS; i++) forecastWeatherCode[i] = -1;

  loadColorSettings();
  setupWiFi();       // Blocks here until WiFi is connected (or setup fails)
  setupWebServer();

  // getLocalTime() only succeeds once the board has heard back from an NTP
  // (internet time) server, so we sit in this loop trying every half a
  // second until it does. Until this succeeds, the ESP32 doesn't actually
  // know what time it is (its internal clock starts from zero at boot).
  Serial.println("Waiting for NTP time sync...");
  struct tm timeinfo;
  while (!getLocalTime(&timeinfo)) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nTime synced.");

  // Fetch the first forecast now that we know the correct current time --
  // fetchWeather() needs an accurate "current hour" to know which part of
  // the API's response is relevant, so this must happen after NTP sync
  // above, not before.
  fetchWeather();
}

// -----------------------------------------------------------------------
// loop() -- runs over and over, forever, after setup() finishes
// -----------------------------------------------------------------------
void loop() {
  // Let the web server handle any pending page request or form submission.
  // If this isn't called regularly, the web page would never load.
  webServer.handleClient();

  // Refresh the weather forecast either on the usual 15-minute schedule, or
  // immediately if the saved location just changed (see handleSave() above).
  if (weatherNeedsRefresh ||
      millis() - lastWeatherFetch >= WEATHER_FETCH_INTERVAL_MS) {
    weatherNeedsRefresh = false;
    fetchWeather();
  }

  // Re-draw the LEDs at most every 10 milliseconds (about 100 times a
  // second) rather than on every single pass through loop() -- loop()
  // itself runs far more often than that, and redrawing needlessly often
  // wastes time without making anything look smoother.
  static unsigned long lastUpdate = 0;
  unsigned long now = millis();
  if (now - lastUpdate >= 10) {
    lastUpdate = now;

    // Read the current date/time and the fractional part of the current
    // second from a single call to gettimeofday(), so both values come
    // from exactly the same instant (see the big comment on renderClock()
    // above for why that matters).
    struct timeval tv;
    gettimeofday(&tv, NULL);
    time_t nowSec = tv.tv_sec;
    struct tm timeinfo;
    localtime_r(&nowSec, &timeinfo);  // splits nowSec into hour/min/sec/etc, in local time
    float subSec = tv.tv_usec / 1000000.0f;  // microseconds -> a 0.0-1.0 fraction

    renderClock(timeinfo, subSec);
  }

  // A tiny debug feature: typing "resetwifi" into the serial monitor and
  // pressing enter wipes the saved WiFi network and restarts the board, so
  // it falls back into the "LED-Clock-Setup" portal again. Handy if you
  // need to reconnect it to a different WiFi network later.
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
