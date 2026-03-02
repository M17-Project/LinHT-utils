# LinHT ATtiny826 Power Manager Firmware

## Overview

This firmware runs on the ATtiny826 (`U14`) on LinHT hardware revision B.
It is the always-on power supervisor for the handheld SDR platform.

Its primary role is to control safe power sequencing between:

- user hardware actions (`ON/OFF` switch),
- Linux/SoM power state requests,
- temporary USB boot strap behavior.

This MCU is part of the core reliability path of LinHT. If this firmware is
missing or incorrect, normal system start/shutdown is not guaranteed.

## Hardware Context

LinHT Rev.B power architecture uses:

- `BQ25792` charger / power-path,
- 2S battery system,
- controlled buck enables for SoM and peripherals.

The ATtiny826 controls enable lines and handshake lines connected to the i.MX93 SoM.

## Signal Map (ATtiny826)

- `PA2` -> `/Connectors/SWITCH_ON_OFF` (input, active low from user switch)
- `PA6` -> `/Power/5V0_ON_REQ` (output, buck enable for 5V rail)
- `PA7` -> `/Power/3V3_ON_REQ` (output, buck enable for 3V3 rail)
- `PA1` -> `/Power/~{5V_OFF_REQ}` (input from SoM/Linux, active low)
- `PA5` -> `/Power/~{5V_ON_OUT}` (output to SoM ON/OFF input, asserted low pulse)
- `PA3` -> `/Power/USB_BOOT` (output, tri-stated when inactive)
- `PB5` -> `/SIDE_BTN` (input, active low when side key pressed)
- `PA4` -> `/Power/SOM_RST` (reserved, not actively used in current logic)

## Functional Goals

- Power-on only on valid user action (ON/OFF switch falling edge).
- Always enable 5V first, then 3V3 after delay.
- Support USB boot by pressing side button during power-on.
- Perform graceful Linux shutdown handshake when user turns switch off.
- Force hard power cut if Linux does not respond in time.

## State Machine

The firmware uses four explicit states:

- `DEV_OFF`
- `DEV_STARTUP_WAIT_3V3`
- `DEV_RUNNING`
- `DEV_SHUTDOWN_WAIT`

### `DEV_OFF`

- Rails are off (`5V0_ON_REQ=0`, `3V3_ON_REQ=0`).
- `USB_BOOT` is high-impedance (floating).
- `~5V_ON_OUT` is high-impedance (inactive).
- Waits for `ON/OFF` switch falling edge.
- `SIDE_BTN` pull-up is enabled only in this state for alterative USB boot detection.

### Startup Sequence

Trigger: `ON/OFF` switch falling edge (user turns device on).

Actions:

- Enable `5V0_ON_REQ` immediately.
- Keep `3V3_ON_REQ` off.
- Sample USB boot request:
  - if side button is held during this event, assert `USB_BOOT`,
  - otherwise keep `USB_BOOT` floating.
- Start timer for delayed 3V3 enable.

### `DEV_STARTUP_WAIT_3V3`

- Waits `STARTUP_3V3_DELAY_MS` (default 3000 ms).
- Then enables `3V3_ON_REQ`.
- `USB_BOOT` is released (floating) after a short hold window (`USB_BOOT_HOLD_MS`, default 5000 ms from start event).
- If user reverts switch during startup, rails are shut off and state returns to `DEV_OFF`.

### `DEV_RUNNING`

- `5V` and `3V3` rails enabled.
- Normal operation.
- Watches for two shutdown signals:
  - software request: `~5V_OFF_REQ` asserted by Linux,
  - hardware request: user turns switch off (rising edge).

### Hardware-Off Path (`DEV_RUNNING` -> `DEV_SHUTDOWN_WAIT`)

Trigger: `ON/OFF` switch rising edge (user turns switch off).

Actions:

- Send active-low pulse on `~5V_ON_OUT` to request graceful Linux shutdown.
- Start hard timeout (`SHUTDOWN_TIMEOUT_MS`, default 20000 ms).
- Wait for Linux `~5V_OFF_REQ`.

### Software-Off Path (Any active state)

Trigger: `~5V_OFF_REQ` asserted low by SoM/Linux.

Actions:

- Immediate power cut:
  - `3V3_ON_REQ=0`,
  - `5V0_ON_REQ=0`.
- Return to `DEV_OFF`.

### `DEV_SHUTDOWN_WAIT`

- Continues waiting for Linux-off request.
- If timeout expires before `~5V_OFF_REQ`, performs hard power cut and returns to `DEV_OFF`.

## Timing Parameters

Configured in `main.c`:

- `LOOP_TICK_MS = 10`
- `DEBOUNCE_TICKS = 4`
- `STARTUP_3V3_DELAY_MS = 3000`
- `USB_BOOT_HOLD_MS = 5000`
- `ON_OUT_PULSE_MS = 200`
- `SHUTDOWN_TIMEOUT_MS = 20000`

## Clocking and Power Consumption

CPU is configured for low-power stable operation:

- `F_CPU = 1 MHz`
- Internal oscillator source configured at 16 MHz in fuses.
- Runtime prescaler set to `/16` in firmware.

## Fuses

Fuse image is stored in:

- `attiny/fuses.hex`

Current profile:

- Watchdog enabled after reset
- BOD at 2.6V
- 16 MHz oscillator source
- UPDI kept enabled on PA0
- 64 ms startup delay

## Build and Flash

```bash
make
```

Docker build (if avr-gcc with ATTiny826 support is unavailable on your system):

```bash
docker run --rm -v .:/workspace ghcr.io/slintak/avr-gcc:latest make -C /workspace/pmu clean all
```

Program sequence:

```bash
make -C attiny ping COM_PORT=/dev/ttyUSB0
make -C attiny fuses COM_PORT=/dev/ttyUSB0
make -C attiny program COM_PORT=/dev/ttyUSB0
make -C attiny read_fuses COM_PORT=/dev/ttyUSB0
```
