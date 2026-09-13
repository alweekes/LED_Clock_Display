# LED Clock Display

An analog-style clock rendered on a 241-LED WS2812B concentric-ring panel,
driven by an ESP32. Hour, minute, and second "hands" are drawn as single
LEDs on dedicated rings, with a decorative gradient spinner sweeping the
remaining rings and finishing at the center dot. WiFi and timezone are
configured from a phone via a captive portal, time is kept via NTP, and
colors/brightness are adjustable live from a web page.

See [`TRANSCRIPT.md`](TRANSCRIPT.md) for the full story of how this was
built (including the hardware detective work to figure out the panel's
wiring layout).

## Hardware

- **MCU**: ESP32-WROOM-32 DevKitC-style dev board (30-pin, no PSRAM)
- **LED panel**: WS2812B (5050 RGB) addressable LED ring panel, 241 pixels
  total, arranged as **8 concentric rings + 1 center dot**, wired outer to
  inner as a single data chain (each ring fully traversed before the chain
  continues to the next ring inward):

  | Ring            | LEDs | Role                                    |
  |-----------------|-----:|------------------------------------------|
  | 1 (outermost)   | 60   | Second hand                              |
  | 2               | 48   | Spinner (gradient sweep)                 |
  | 3               | 40   | Minute hand                              |
  | 4               | 32   | Spinner                                  |
  | 5               | 24   | Hour hand                                |
  | 6               | 16   | Spinner                                  |
  | 7               | 12   | Spinner                                  |
  | 8 (innermost)   | 8    | Spinner                                  |
  | Center          | 1    | Spinner (final stop)                     |
  | **Total**       | **241** |                                       |

  Ring 1 being exactly 60 LEDs gives the second hand a satisfying 1:1
  mapping, though the firmware actually renders it (and every hand) as a
  continuous sub-pixel position, not a fixed 60-step tick.

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
Arduino framework). Key dependencies (see `platformio.ini`):

- [FastLED](https://github.com/FastLED/FastLED) — LED driving
- [WiFiManager](https://github.com/tzapu/WiFiManager) — captive-portal WiFi
  + timezone setup
- ESP32 Arduino core's built-in `WebServer` + `ESPmDNS` — the live config
  web UI

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
- Spinner color gradient (start / mid / end)
- Hand brightness
- Spinner brightness (start / end, interpolated across the sweep)

Settings are saved to flash (NVS) and persist across reboots.

## Repo layout

```
src/main.cpp       Firmware (rendering, WiFi/NTP setup, web UI)
platformio.ini     PlatformIO project/board/library configuration
docs/              Reference material collected during the build
TRANSCRIPT.md       Condensed chat transcript of the build process
```
