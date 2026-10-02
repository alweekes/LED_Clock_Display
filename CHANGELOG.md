# Changelog

All notable changes to this project are documented here. Format loosely
follows [Keep a Changelog](https://keepachangelog.com/).

## [Unreleased]

### Added
- Auto-dim overnight: an on/off toggle in the web UI that, when enabled,
  scales brightness down to ~25% between 10pm and 7am on top of whatever
  brightness is configured. The schedule and dim level are fixed constants
  in `src/main.cpp` (`AUTO_DIM_START_HOUR`/`AUTO_DIM_END_HOUR`/
  `AUTO_DIM_SCALE`), not exposed in the web page.

### Changed
- NTP time sync now tries the local GPS-disciplined time server
  (`192.168.1.40`) first, falling back to `pool.ntp.org` then
  `time.nist.gov` if it's unreachable -- previously went straight to the
  internet servers.

### Fixed
- `drawDot()` (the hour/minute/second hands and the hour-of-day pointer)
  switched from rounding to the nearest LED to flooring. Rounding made each
  hand light its next LED once it was more than halfway through that LED's
  time step rather than when it actually arrived -- most visible on the
  hour hand, where each of its 48 LEDs spans 15 real minutes: it was
  jumping to the "3" position at 2:52:30 instead of 3:00. Affected the
  minute/second hands and hour-of-day pointer too, just less noticeably,
  since their steps are smaller slices of time.

### Added
- [`docs/flashing-troubleshooting.md`](docs/flashing-troubleshooting.md):
  a full write-up of why automated flashing is unreliable on this
  specific board (a marginal auto-reset circuit, diagnosed down to an
  oscilloscope-confirmed ambiguous "half-rail" EN release), and why
  manual BOOT+EN flashing is the practical answer rather than a
  software/timing workaround.

## [1.2.0] - 2026-09-20

### Fixed
- Center heartbeat LED now respects the brightness slider (`handBrightness`)
  like every other ring. It was previously always drawn at raw full
  strength (60-255), ignoring the web UI's brightness setting.

### Changed
- Documented the as-built bezel labeling: the weather color legend went on
  ring 5 instead of hour numbers, so ring 6 carries the printed 24-hour
  timing markers instead (both rings still cover the same fixed 0-23
  layout underneath — only the printed labels moved).
- Temperature gauge (ring 8) fill math switched from rounding to
  truncation, so each LED gets an exact whole-number "on" threshold
  instead of the old half-degree offsets (2.5, 7.5, ...) — cheaper to
  label on a printed bezel.
- Temperature gauge range settled at **-5&deg;C to 35&deg;C** (5&deg;C per
  LED, thresholds 0, 5, 10, ... 35), after briefly trying 0&deg;C&ndash;40&deg;C
  and shifting back down to restore headroom below freezing. Readings
  below 0&deg;C still all show an empty gauge (no sub-zero resolution),
  but the gauge at least registers "below freezing" starting from 0
  rather than 5.

## [1.1.0] - 2026-09-19

### Changed
- Weather forecast ring (ring 5) reverts from an hour-hand-aligned rotating
  layout back to a **fixed 24-hour-of-day dial**: LED 0 is always midnight,
  LED 23 always 11pm, regardless of the current time. This makes it
  bezel-printable ("12am...10pm" at fixed positions), which the rotating
  version couldn't be, since its LED-to-hour mapping moved continuously.
- Weather condition colors replaced with a smaller set of maximally
  distinct hues (yellow/green/blue/white/magenta) rather than one shade per
  WMO sub-category. The previous palette used three different greys and
  three different blues; on an RGB LED, "grey" (equal R/G/B) just reads as
  dim white, which was easy to confuse with the snow color.

### Added
- Hour-of-day pointer (ring 6, previously unused): a continuous sweep over
  the full 24-hour day, showing where "now" falls on the fixed forecast
  dial above. Deliberately separate from the (12-hour) hour hand, so 2am
  and 2pm read differently.
- Temperature gauge (ring 8, previously unused): a coarse 8-LED bar-fill
  gauge (-5C to 35C, ~5C per LED), each LED colored by its fixed position
  in a blue-to-red gradient; the fill count (not the color) shows the
  reading. Uses the `temperature_2m` field from the same Open-Meteo
  request.
- Hour-of-day pointer color field in the web UI.

## [1.0.0] - 2026-09-14

First tagged release. Summary of the feature set as of this point:

### Added
- ESP32 + FastLED firmware driving a 241-LED WS2812B concentric-ring panel
  (8 rings + center dot) as an analog-style clock.
- Hour, minute, and second hands rendered as continuous sub-pixel sweeps on
  three dedicated rings.
- Month and day-of-month indicators rendered as exact, fixed-position LED
  steps (rather than a sweep), intended to align with printed numbers on a
  3D-printed bezel.
- 24-hour weather forecast ring: color encodes condition (clear / cloudy /
  rain / snow / thunderstorm), brightness encodes chance of rain, fetched
  from the free [Open-Meteo](https://open-meteo.com/) API (no key required).
- Center dot pulses once per second as a heartbeat.
- WiFiManager-based captive portal (`LED-Clock-Setup`) for first-time WiFi
  and POSIX timezone setup from a phone, with no credentials hard-coded.
- NTP time sync.
- Live web configuration page (`http://ledclock.local/`) for hand/indicator
  colors, brightness, and weather location, persisted to flash (NVS).
- `resetwifi` serial command to clear saved WiFi credentials.
- Reference ring-layout diagram (`docs/ring-map.svg`), embedded in the
  README.
- Beginner-friendly inline documentation throughout `src/main.cpp`.

### Removed
- An earlier decorative "spinner" effect (a gradient sweep across the
  otherwise-unused rings) was built, iterated on extensively, and ultimately
  removed in favor of the month/day/forecast indicators above. See
  [`TRANSCRIPT.md`](TRANSCRIPT.md) for that history.
