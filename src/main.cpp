#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>
#include <FastLED.h>

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
//   Center (1 LED):          index 240 -- treated as a 9th "ring" of size 1,
//                            the final stop of the spinner sweep.
#define CENTER_INDEX 240

struct RingDef {
  int start;
  int size;
};
// Outer (index 0) to inner (index 7), plus the center dot (index 8).
const RingDef ALL_RINGS[9] = {
    {0, 60}, {60, 48}, {108, 40}, {148, 32}, {180, 24},
    {204, 16}, {220, 12}, {232, 8}, {CENTER_INDEX, 1},
};

// Each hand owns a single dedicated ring. Every other ring (plus the center
// dot) is a "spinner" track: a decorative dot that sweeps outer -> inner
// through all of them in turn, so the display stays lively between the
// (slow) hand movements.
#define SECOND_HAND_RING 0   // ring1, 60 LEDs
#define SPINNER1_RING 1      // ring2, 48 LEDs -- between second & minute
#define MINUTE_HAND_RING 2   // ring3, 40 LEDs
#define SPINNER2_RING 3      // ring4, 32 LEDs -- between minute & hour
#define HOUR_HAND_RING 4     // ring5, 24 LEDs
#define SPINNER3_RING 5      // ring6, 16 LEDs -- between hour & center
#define SPINNER4_RING 6      // ring7, 12 LEDs -- inner, was unused
#define SPINNER5_RING 7      // ring8,  8 LEDs -- innermost, was unused
#define CENTER_RING 8        // center dot, 1 LED -- final spinner stop

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
CRGB spinnerColorStart(90, 65, 15);
CRGB spinnerColorMid(75, 42, 67);
CRGB spinnerColorEnd(60, 20, 120);
uint8_t handBrightness = 255;
uint8_t spinnerBrightnessStart = 255;
uint8_t spinnerBrightnessEnd = 255;

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
  spinnerColorStart =
      unpackColor(prefs.getUInt("spinRGB", packColor(spinnerColorStart)));
  spinnerColorMid =
      unpackColor(prefs.getUInt("spinRGBm", packColor(spinnerColorMid)));
  spinnerColorEnd =
      unpackColor(prefs.getUInt("spinRGB2", packColor(spinnerColorEnd)));
  handBrightness = prefs.getUChar("handBri", handBrightness);
  spinnerBrightnessStart =
      prefs.getUChar("spinBriS", spinnerBrightnessStart);
  spinnerBrightnessEnd = prefs.getUChar("spinBriE", spinnerBrightnessEnd);
  prefs.end();
}

void saveColorSettings() {
  prefs.begin("clock", false);
  prefs.putUInt("hourRGB", packColor(hourColor));
  prefs.putUInt("minRGB", packColor(minuteColor));
  prefs.putUInt("secRGB", packColor(secondColor));
  prefs.putUInt("spinRGB", packColor(spinnerColorStart));
  prefs.putUInt("spinRGBm", packColor(spinnerColorMid));
  prefs.putUInt("spinRGB2", packColor(spinnerColorEnd));
  prefs.putUChar("handBri", handBrightness);
  prefs.putUChar("spinBriS", spinnerBrightnessStart);
  prefs.putUChar("spinBriE", spinnerBrightnessEnd);
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
<label>Spinner color (start, outer ring)
<input type="color" name="spinnerColor" value="%SPINNER_COLOR%"></label>
<label>Spinner color (midpoint)
<input type="color" name="spinnerColorMid" value="%SPINNER_COLOR_MID%"></label>
<label>Spinner color (end, inner ring)
<input type="color" name="spinnerColor2" value="%SPINNER_COLOR2%"></label>
<label>Hand brightness
<input type="range" name="handBrightness" min="0" max="255" value="%HAND_BRI%"></label>
<label>Spinner brightness (start, outer ring)
<input type="range" name="spinnerBrightnessStart" min="0" max="255" value="%SPIN_BRI_START%"></label>
<label>Spinner brightness (end, inner ring)
<input type="range" name="spinnerBrightnessEnd" min="0" max="255" value="%SPIN_BRI_END%"></label>
<button type="submit">Save</button>
</form>
</body></html>
)rawliteral";

void handleRoot() {
  String page = PAGE_TEMPLATE;
  page.replace("%HOUR_COLOR%", colorToHex(hourColor));
  page.replace("%MINUTE_COLOR%", colorToHex(minuteColor));
  page.replace("%SECOND_COLOR%", colorToHex(secondColor));
  page.replace("%SPINNER_COLOR%", colorToHex(spinnerColorStart));
  page.replace("%SPINNER_COLOR_MID%", colorToHex(spinnerColorMid));
  page.replace("%SPINNER_COLOR2%", colorToHex(spinnerColorEnd));
  page.replace("%HAND_BRI%", String(handBrightness));
  page.replace("%SPIN_BRI_START%", String(spinnerBrightnessStart));
  page.replace("%SPIN_BRI_END%", String(spinnerBrightnessEnd));
  webServer.send(200, "text/html", page);
}

void handleSave() {
  hourColor = hexToColor(webServer.arg("hourColor"), hourColor);
  minuteColor = hexToColor(webServer.arg("minuteColor"), minuteColor);
  secondColor = hexToColor(webServer.arg("secondColor"), secondColor);
  spinnerColorStart =
      hexToColor(webServer.arg("spinnerColor"), spinnerColorStart);
  spinnerColorMid =
      hexToColor(webServer.arg("spinnerColorMid"), spinnerColorMid);
  spinnerColorEnd =
      hexToColor(webServer.arg("spinnerColor2"), spinnerColorEnd);
  handBrightness = (uint8_t)webServer.arg("handBrightness").toInt();
  spinnerBrightnessStart =
      (uint8_t)webServer.arg("spinnerBrightnessStart").toInt();
  spinnerBrightnessEnd =
      (uint8_t)webServer.arg("spinnerBrightnessEnd").toInt();

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
// ring's circumference. Used for both the hands and the spinner dots.
void drawDot(int ringIdx, float frac, CRGB color) {
  int start = ALL_RINGS[ringIdx].start;
  int size = ALL_RINGS[ringIdx].size;

  int idx = ((int)roundf(frac * size)) % size;
  if (idx < 0) idx += size;

  leds[start + idx] += color;
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

// First half of the cycle: a traveling dot sweeps outer -> inner across all
// 5 spinner rings in turn, and each ring STAYS fully lit once its turn ends
// (instead of blanking) -- so by the midpoint of the cycle, every spinner
// ring is completely illuminated. Second half: all 5 rings fade out
// together, reaching fully dark by the end of the cycle, then it repeats.
//
// Each ring's sweep gets a duration sized to its own LED count at a fixed,
// safe per-LED time -- rather than forcing every ring into a shared budget.
// A full 241-LED FastLED.show() takes ~7.2ms; cramming a 48-LED ring into a
// rushed time slice pushed us right past that floor and skipped LEDs.
//
// The resulting minimum sweep length is rounded UP to the next whole second
// (extra time redistributed proportionally across rings, so per-LED time
// only ever increases), and the full cycle (sweep + equal-length decay) is
// anchored to the RTC-derived seconds count (not millis(), which free-runs
// from boot and would drift/not line up with real seconds) -- so the sweep
// always restarts from the outer ring exactly on a whole-second boundary.
#define SPINNER_MS_PER_LED 15UL  // ~2x the ~7.2ms hardware floor, safe margin

const int SPINNER_RINGS[6] = {SPINNER1_RING, SPINNER2_RING, SPINNER3_RING,
                               SPINNER4_RING, SPINNER5_RING, CENTER_RING};
#define NUM_SPINNER_RINGS 6

unsigned long spinnerRingMs[NUM_SPINNER_RINGS];  // sweep duration per ring
int spinnerRingOffset[NUM_SPINNER_RINGS];        // this ring's start, in the
                                                  // overall spinner LED order
int spinnerTotalLeds;
unsigned long spinnerSweepMs;                    // total sweep (first half)
unsigned long spinnerFullCycleMs;                // sweep + decay
unsigned long spinnerCycleSec;                   // full cycle, in seconds

// Color for one LED at its position in the overall spinner order (0 ..
// spinnerTotalLeds-1, outer ring first): a 3-stop gradient (start -> mid ->
// end) across the whole 5-ring path, with brightness also interpolated
// between a start and end value over the same span.
CRGB spinnerColorAt(int globalIdx) {
  uint8_t ratio = (spinnerTotalLeds > 1)
      ? (uint8_t)((long)globalIdx * 255 / (spinnerTotalLeds - 1))
      : 0;

  CRGB c;
  if (ratio < 128) {
    c = blend(spinnerColorStart, spinnerColorMid, (uint8_t)(ratio * 2));
  } else {
    c = blend(spinnerColorMid, spinnerColorEnd, (uint8_t)((ratio - 128) * 2));
  }

  uint8_t brightness =
      lerp8by8(spinnerBrightnessStart, spinnerBrightnessEnd, ratio);
  c.nscale8_video(brightness);
  return c;
}

void initSpinnerTiming() {
  int totalLeds = 0;
  for (int i = 0; i < NUM_SPINNER_RINGS; i++) {
    spinnerRingOffset[i] = totalLeds;
    totalLeds += ALL_RINGS[SPINNER_RINGS[i]].size;
  }
  spinnerTotalLeds = totalLeds;

  unsigned long minSweepMs = (unsigned long)totalLeds * SPINNER_MS_PER_LED;
  unsigned long sweepSec = (minSweepMs + 999UL) / 1000UL;  // round up
  if (sweepSec < 1) sweepSec = 1;
  spinnerSweepMs = sweepSec * 1000UL;
  spinnerFullCycleMs = spinnerSweepMs * 2UL;
  spinnerCycleSec = spinnerFullCycleMs / 1000UL;

  unsigned long assigned = 0;
  for (int i = 0; i < NUM_SPINNER_RINGS; i++) {
    if (i == NUM_SPINNER_RINGS - 1) {
      spinnerRingMs[i] = spinnerSweepMs - assigned;  // absorb rounding
    } else {
      int size = ALL_RINGS[SPINNER_RINGS[i]].size;
      spinnerRingMs[i] =
          (unsigned long)((float)size / totalLeds * spinnerSweepMs);
      assigned += spinnerRingMs[i];
    }
  }
}

// t: milliseconds within the full spinner cycle (0..spinnerFullCycleMs-1),
// derived from RTC wall-clock time so it lines up with real seconds. Each
// LED's color comes from spinnerColorAt(), a gradient across the whole
// 5-ring spinner path (outer ring first), rather than one flat color.
void drawSpinner(unsigned long t) {
  if (t >= spinnerSweepMs) {
    // Decay phase: mirrors the sweep in the same outer -> inner order, using
    // the same per-ring time slices, so the ring that lit up first (and has
    // been lit longest) also darkens first -- a concentric wave of darkness
    // moving inward, rather than every ring dimming together.
    unsigned long td = t - spinnerSweepMs;
    unsigned long segStart = 0;
    for (int i = 0; i < NUM_SPINNER_RINGS; i++) {
      unsigned long segEnd = segStart + spinnerRingMs[i];
      const RingDef& rd = ALL_RINGS[SPINNER_RINGS[i]];

      if (td >= segEnd) {
        // Already fully decayed -- stays off.
      } else if (td >= segStart) {
        // Extinguish LED-by-LED from the first (index 0) to the last, the
        // same direction the sweep lit them up in -- not a ring-wide fade.
        float localT = (float)(td - segStart) / (float)spinnerRingMs[i];
        int cutoff = (int)roundf(localT * rd.size);  // LEDs already off
        for (int j = cutoff; j < rd.size; j++) {
          leds[rd.start + j] += spinnerColorAt(spinnerRingOffset[i] + j);
        }
      } else {
        // Not yet reached by the decay wave -- still fully lit.
        for (int j = 0; j < rd.size; j++) {
          leds[rd.start + j] += spinnerColorAt(spinnerRingOffset[i] + j);
        }
      }
      segStart = segEnd;
    }
    return;
  }

  // Sweep phase: rings already completed stay fully lit; the current ring
  // grows a trail from its start up to however far the head has traveled;
  // rings not yet reached stay dark (already cleared this frame).
  unsigned long segStart = 0;
  for (int i = 0; i < NUM_SPINNER_RINGS; i++) {
    unsigned long segEnd = segStart + spinnerRingMs[i];
    int ringIdx = SPINNER_RINGS[i];
    int start = ALL_RINGS[ringIdx].start;
    int size = ALL_RINGS[ringIdx].size;

    if (t >= segEnd) {
      for (int j = 0; j < size; j++) {
        leds[start + j] += spinnerColorAt(spinnerRingOffset[i] + j);
      }
    } else if (t >= segStart) {
      float localT = (float)(t - segStart) / (float)spinnerRingMs[i];  // 0..1
      float headPos = localT * size;
      int headIdx = ((int)roundf(headPos)) % size;
      int steps = (int)roundf(headPos);

      for (int k = 0; k <= steps; k++) {
        int idx = ((headIdx - k) % size + size) % size;
        float brightness = (steps == 0) ? 1.0f : 1.0f - (float)k / (float)steps;
        CRGB c = spinnerColorAt(spinnerRingOffset[i] + idx);
        c.nscale8_video((uint8_t)roundf(brightness * 255));
        leds[start + idx] += c;
      }
      break;  // remaining rings not yet reached; stay dark
    } else {
      break;
    }
    segStart = segEnd;
  }
}

// subSec (0..1) must come from the same clock read as timeinfo.tm_sec and
// epochSec -- mixing RTC-derived values with millis() (a separate
// free-running timer) lets the two drift apart and makes things step
// backward or drift out of sync over time.
void renderClock(const struct tm& timeinfo, float subSec, time_t epochSec) {
  // Hand rings redraw clean each frame (no trail).
  fill_solid(&leds[ALL_RINGS[HOUR_HAND_RING].start], ALL_RINGS[HOUR_HAND_RING].size, CRGB::Black);
  fill_solid(&leds[ALL_RINGS[MINUTE_HAND_RING].start], ALL_RINGS[MINUTE_HAND_RING].size, CRGB::Black);
  fill_solid(&leds[ALL_RINGS[SECOND_HAND_RING].start], ALL_RINGS[SECOND_HAND_RING].size, CRGB::Black);

  // Spinner rings (including the center dot, the final stop) redraw clean
  // each frame -- drawSpinner() below fills in the active ring's full
  // geometric trail on top, so an inactive ring is fully off and the active
  // one is off/growing/fully lit depending on how far through its turn it
  // is.
  for (int i = 0; i < NUM_SPINNER_RINGS; i++) {
    const RingDef& rd = ALL_RINGS[SPINNER_RINGS[i]];
    fill_solid(&leds[rd.start], rd.size, CRGB::Black);
  }

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

  // Traveling filler spinner: sweeps outer -> inner across all 5 spinner
  // rings and finishes at the center dot, then repeats every
  // spinnerCycleSec seconds, anchored to the RTC so it always restarts from
  // the outer ring exactly on a real second boundary. Color is a gradient
  // (see spinnerColorAt), brightness applied there too.
  unsigned long secInCycle = (unsigned long)(epochSec % spinnerCycleSec);
  unsigned long tInCycle = secInCycle * 1000UL + (unsigned long)(subSec * 1000.0f);
  drawSpinner(tInCycle);

  FastLED.show();
}

void setup() {
  Serial.begin(115200);

  FastLED.addLeds<LED_TYPE, DATA_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.setBrightness(BRIGHTNESS);
  FastLED.clear();
  FastLED.show();

  initSpinnerTiming();
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
}

void loop() {
  webServer.handleClient();

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

    renderClock(timeinfo, subSec, tv.tv_sec);
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
