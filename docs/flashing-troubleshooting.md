# Flashing Troubleshooting: Marginal Auto-Reset Circuit

## Symptom

Automated flashing (`pio run --target upload`, or `esptool` run directly from
a script) has reliably failed throughout this project with:

```
A fatal error occurred: Failed to connect to ESP32: Invalid head of packet (0x2A): Possible serial noise or corruption.
```

— regardless of esptool version (PlatformIO's bundled 4.11.0 or a newer
system 5.3.1), baud rate, cable, USB port, or retry count. Manually holding
the board's **BOOT** button while tapping **EN** has always worked.

## Diagnosis (2026-10-02)

Methodically ruled out, in order, using an oscilloscope and raw serial
capture (bypassing esptool's own protocol logic with plain `pyserial`):

1. **GPIO0/DTR wiring.** Drove DTR directly while reading raw serial bytes.
   GPIO0 (probed at the BOOT button pad) measured a clean 0V when DTR was
   asserted, in both polarities tested. The signal path from the USB-serial
   chip's DTR line to GPIO0 is intact.
2. **Reset-sequence timing.** Replicated esptool's exact `ClassicReset` /
   `UnixTightReset` sequences, then deliberately stretched the gap between
   releasing EN and releasing GPIO0 up to 3 seconds (via `esptool.cfg`'s
   `reset_delay`). No improvement at any timing tried, which rules out a
   simple RC-settling-time mismatch as the sole cause.
3. **CP2102C hardware-flow-control quirk.** esptool specifically flags the
   `CP2102C` (USB ID `10c4:ea64`) for an RTS-stealing flow-control bug. This
   board's chip reports plain `10c4:ea60` (standard CP2102) — not affected.
4. **Spontaneous reset loop.** Captured raw serial output immediately after
   a single, deliberate reset pulse (no further commands issued) and found
   the chip repeatedly printing a *truncated* ROM boot banner
   (`ets Jul 29 2019 12:21:46` / `rst:`) over and over, dozens of times in
   under a second, before eventually settling into a full boot. This pointed
   at genuine electrical instability around reset, not a GPIO/control-line
   logic issue.
5. **EN voltage level (the actual root cause).** Put an oscilloscope
   directly on EN:
   - Holding GPIO0 low continuously via a software sequence that mimicked a
     manual "hold BOOT, tap EN several times" (DTR held asserted, RTS
     pulsed repeatedly) only pulled EN down to **~2.5V** — nowhere near the
     well-under-1V level the ESP32 needs for a clean reset.
   - Pulsing RTS alone, with DTR left idle, dropped EN to a clean **0V**.
   - Under esptool's actual stock reset sequence (a brief, correctly-ordered
     DTR/RTS overlap, not a sustained hold), EN also reached 0V — but then
     rose in **two visible stages**: 0V &rarr; an intermediate "half-rail"
     plateau &rarr; full 3.3V, rather than one clean edge.

## Root cause

The onboard two-transistor auto-reset circuit (the standard ESP32 DevKitC
reference design, where the DTR and RTS lines are cross-coupled between a
pair of transistors to drive EN and GPIO0) resets EN to a true 0V, but its
*release* passes through an ambiguous half-rail plateau before settling
high, rather than one clean edge. Exactly when — or whether — the ESP32's
internal logic decides "reset is over, sample GPIO0 now" during that
plateau varies from attempt to attempt. That explains why the chip
sometimes lands in the ROM bootloader (download mode) and sometimes just
boots the normal application: it's not a fixed wiring fault, a software
timing bug, or an esptool version issue, so no amount of retrying, baud
rate changes, or reset-sequence tuning fixes it — the electrical transition
itself is marginal.

A physical BOOT-button press works every time because it shorts GPIO0
directly to ground through a mechanical switch, bypassing the transistor
circuit's ambiguous electrical state entirely.

## Practical takeaway

Flash this board manually, holding BOOT and tapping EN as the upload
begins:

```
pio run --target upload
```

or directly:

```
python3 -m esptool --chip esp32 --port /dev/ttyUSB0 --baud 460800 write-flash 0x10000 .pio/build/esp32dev/firmware.bin
```

Automated/remote flashing of this specific board is not expected to become
reliable without a hardware revision to the auto-reset circuit (for
example, tuning or replacing the capacitor/resistor values around the two
transistors to produce a single clean EN edge instead of the half-rail
plateau).
