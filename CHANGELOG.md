# Changelog

All notable changes to this project are documented here. Format loosely
follows [Keep a Changelog](https://keepachangelog.com/).

## [Unreleased]

### Changed
- Temperature gauge (ring 8) range changed from -5&deg;C&ndash;35&deg;C to
  **0&deg;C&ndash;40&deg;C**, and the fill math switched from rounding to
  truncation. Together these give each LED an exact whole-number "on"
  threshold (5, 10, 15, ... 40&deg;C) instead of the old half-degree
  offsets (2.5, 7.5, ...) — cheaper to label on a printed bezel, at the
  cost of no longer distinguishing sub-zero readings (anything at or
  below 5&deg;C now shows an empty gauge).

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
