# LED Clock Display

An analog-style clock rendered on a 241-LED WS2812B concentric-ring panel,
driven by an ESP32. Hour, minute, and second "hands" are drawn as single
LEDs sweeping continuously around dedicated rings, while month, day-of-month,
and a 24-hour weather forecast are drawn as fixed integer steps on three more
rings — intended to line up with printed numbers on a 3D-printed bezel. WiFi
and timezone are configured from a phone via a captive portal, time is kept
via NTP, weather comes from a free API, and colors/brightness/location are
adjustable live from a web page.

See [`TRANSCRIPT.md`](TRANSCRIPT.md) for the full story of how this was
built (including the hardware detective work to figure out the panel's
wiring layout).

## Hardware

![Ring map: the panel's 8 concentric rings and center dot, showing each ring's LED count, assigned role, and an example lit state](docs/ring-map.svg)

*Fig. 1 — wiring order runs outer to inner (index 0 lands on the same spoke on every ring). Illustrated at 10:09:24, September 13 — not live data.*

- **MCU**: ESP32-WROOM-32 DevKitC-style dev board (30-pin, no PSRAM)
- **LED panel**: WS2812B (5050 RGB) addressable LED ring panel, 241 pixels
  total, arranged as **8 concentric rings + 1 center dot**, wired outer to
  inner as a single data chain (each ring fully traversed before the chain
  continues to the next ring inward):

  | Ring            | LEDs | Role                    | Mode                        |
  |-----------------|-----:|--------------------------|------------------------------|
  | 1 (outermost)   | 60   | Second hand              | Continuous sweep             |
  | 2               | 48   | Hour hand                | Continuous sweep             |
  | 3               | 40   | Minute hand              | Continuous sweep             |
  | 4               | 32   | Day of month             | Discrete step (31/32 used)   |
  | 5               | 24   | 24-hour weather forecast | Data-driven, 1 hour/LED      |
  | 6               | 16   | *unused*                 | —                             |
  | 7               | 12   | Month                    | Discrete step (12/12 used)   |
  | 8 (innermost)   | 8    | *unused*                 | —                             |
  | Center          | 1    | Heartbeat                | Pulses once per second       |
  | **Total**       | **241** |                       |                               |

  Ring 1 being exactly 60 LEDs gives the second hand a satisfying 1:1
  mapping, though the hands actually render as a continuous sub-pixel
  position, not a fixed step. Month and day, by contrast, are rendered as
  an **exact integer LED index** with no anti-aliasing — ring 7 (12 LEDs)
  is an exact fit for the months, and ring 4 (32 LEDs) is the closest fit
  for day-of-month (index 31, the 32nd LED, simply never lights). This
  matters if you're 3D-printing a bezel with fixed printed numbers: each
  step must land on the same physical LED every time.

## Wiring

| Panel wire | ESP32 pin                                   |
|------------|----------------------------------------------|
| 5V (V)     | 5V / VIN                                      |
| GND (G)    | GND                                            |
| Data (D)   | **GPIO16** (labeled `RX2` on this board's silkscreen, between `TX2` and `D4`) |

Notes:

- GPIO16/17 are only safe to use for general I/O on plain WROOM-32 modules.
  On WROVER modules (with onboard PSRAM), GPIO16/17 are wired internally to
  the PSRAM and unusable here — pick a different pin (e.g. GPIO18/19/21/23)
  if you're using a WROVER board, and update `DATA_PIN` in `src/main.cpp`
  to match.
- The panel draws significant current if many LEDs are lit at once (241 x
  up to ~60mA each at full white). This project only lights a handful of
  LEDs at a time by design, but if you increase brightness or LED counts,
  power the panel from a supply sized for worst case, not just the ESP32's
  onboard 5V regulator.
- No level shifter or data-line series resistor was used in this build; if
  you see flicker or erratic pixels, a ~330-470 ohm resistor in series on
  the data line (close to the first LED) and a level shifter (3.3V -> 5V)
  are the standard WS2812B reliability fixes.

## Firmware

Built with [PlatformIO](https://platformio.org/) (`esp32dev` board,
Arduino framework). `src/main.cpp` is commented throughout for readers new
to Arduino/ESP32 development — it explains the ring-indexing math, the
WiFiManager/captive-portal flow, NVS persistence, and the weather
HTTP/JSON fetch as it goes, not just what each line does but why. Key
dependencies (see `platformio.ini`):

- [FastLED](https://github.com/FastLED/FastLED) — LED driving
- [WiFiManager](https://github.com/tzapu/WiFiManager) — captive-portal WiFi
  + timezone setup
- [ArduinoJson](https://arduinojson.org/) — parsing the weather API response
- ESP32 Arduino core's built-in `WebServer`, `ESPmDNS`, `HTTPClient` +
  `WiFiClientSecure` — the live config web UI and weather fetch

Weather comes from [Open-Meteo](https://open-meteo.com/) (free, no API key)
based on the latitude/longitude set in the web UI, refreshed every 15
minutes or immediately when the location is changed.

### Build & flash

```
pio run --target upload
```

(`upload_port` in `platformio.ini` is hardcoded to `/dev/ttyUSB0` — adjust
if your board enumerates elsewhere.)

### First boot

On first boot (or after clearing WiFi settings), the ESP32 starts an access
point named **`LED-Clock-Setup`**. Connect to it from a phone or laptop; a
captive portal should open automatically (or browse to `192.168.4.1`) with
fields for your WiFi SSID/password and a POSIX timezone string (defaults to
`GMT0BST,M3.5.0/1,M10.5.0`, i.e. Europe/London with DST). Once saved, the
device reboots, joins your network, and syncs time via NTP.

To clear stored WiFi credentials and re-run setup, send `resetwifi` over
the serial monitor.

### Live configuration

Once on your network, browse to **`http://ledclock.local/`** (or the
device's IP, printed over serial at boot) for a page to set:

- Hour, minute, and second hand colors
- Month and day indicator colors
- Brightness (applied to all of the above)
- Weather latitude/longitude

Settings are saved to flash (NVS) and persist across reboots.

## Repo layout

```
src/main.cpp       Firmware (rendering, WiFi/NTP setup, web UI)
platformio.ini     PlatformIO project/board/library configuration
docs/              Reference material collected during the build
TRANSCRIPT.md      Condensed chat transcript of the build process
CHANGELOG.md       What changed in each release
```

See [`CHANGELOG.md`](CHANGELOG.md) for release notes.
