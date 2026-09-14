# Changelog

All notable changes to this project are documented here. Format loosely
follows [Keep a Changelog](https://keepachangelog.com/).

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
