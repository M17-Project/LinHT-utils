# LinHT Rev C ATtiny826 power manager

Firmware for the current LinHT mainboard, ATtiny826-MU (`U14`).
`main.c` owns the AVR GPIO, interrupts, clock and timer; `pmu.c` contains the
power state machine, which also runs in the host regression tests.

## Pins

| ATtiny pin | Board signal | Behavior |
| --- | --- | --- |
| PA0 / 19 | UPDI, TP7; TP10 through R14 1 kΩ | Programming; UPDI retained by fuses |
| PA1 / 20 | `~5V_OFF_REQ`, SoM pin 16 `PMIC_STBY_REQ` | Input, no pull-up |
| PA2 / 1 | `SWITCH_ON_OFF` | LOW = switch ON; internal pull-up |
| PA3 / 2 | `USB_BOOT`, SoM pin 5 | HIGH = USB boot; otherwise Hi-Z, no pull-up |
| PA4 / 5 | `SOM_RST`, SoM pin 14 | Reserved, Hi-Z, no pull-up |
| PA5 / 6 | `~5V_ON_OUT`, SoM pin 3 `ONOFF` | LOW pulse / Hi-Z; never drive HIGH |
| PA6 / 7 | `5V0_ON_REQ` | Active-high enable of 5 V regulator |
| PA7 / 8 | `3V3_ON_REQ` | Active-high enable of 3.3 V regulator |
| PB5 / 9 | `SIDE_BTN`, J9 | Physical active-low side button |
| PC0 / 15 | `SIDE_BTN_OUT`, SoM pin 114 `GPIO3_IO22` | Open-drain LOW / Hi-Z, no pull-up |
| PB0, PB1 / 14, 13 | Shared I2C SCL, SDA | Not used by this firmware |
| PB2, PB3 / 12, 11 | TP8 TX, TP9 RX | UART not used by this firmware |

PC0 must never be driven HIGH or pulled up to the ATtiny's 3.3 V supply.
Configure the SoM pin as an input with a pull-up in its own 1.8 V domain.

## Startup and side button

1. In `PMU_OFF`, both regulator enables are LOW and the USB strap and PC0
   are Hi-Z. PB5 pull-up and digital input buffer are disabled. The switch
   must first be OFF, then ON continuously for 40 ms. Applying power or resetting
   the ATtiny with the switch already ON does not start the radio.
2. Enable PB5 pull-up, allow 1 ms to settle, and require 40 ms of consecutive
   stable button samples before powering the SoM. If the button keeps bouncing
   for 500 ms, choose normal boot. Turning the switch OFF cancels startup.
3. A stable held button selects USB boot: drive PA3 HIGH **before** enabling 5 V.
   An unpressed button selects normal boot with PA3 Hi-Z. Latch this choice;
   pressing or releasing the key later cannot change it.
4. Enable 5 V, then enable 3.3 V after 3000 ms. Release the USB strap to Hi-Z
   5000 ms after the 5 V enable, including when shutdown has started meanwhile.
5. From 5 V enable until power-off, PB5 BOTHEDGES interrupts immediately forward
   the raw button state to PC0 (LOW when pressed, Hi-Z when released). There is
   no runtime debounce or main-loop polling delay in this path. Mechanical bounce
   is passed to the SoM for its input driver to handle.

PB5 pull-up stays enabled during startup, operation and shutdown: without it the
isolated released button would float. It is disabled when the radio is OFF.
An open button draws only leakage current through the pull-up; a held button
draws pull-up current while the radio is powered.

## Shutdown

In `PMU_RUNNING`, a debounced switch-OFF requests Linux shutdown with a 200 ms
LOW pulse on PA5. Keep both rails enabled while waiting, with a 20 s hard timeout.
A qualified active-low PA1 acknowledgement cuts power in `PMU_RUNNING` or
`PMU_SHUTDOWN`: the firmware must first have observed a debounced HIGH before
accepting LOW. PA1 must be stable for 40 ms, so short glitches cannot cut power.
During the initial 3 s startup, PA1 is sampled but cannot cut power.

On power-off, disable 3.3 V before 5 V, release the strap and PC0, and disable PB5
pull-up/input sensing. Turning the switch back ON during shutdown does not
cancel it; after power-off another complete OFF -> ON action is required.

The SoM signal is `PMIC_STBY_REQ`, not an application GPIO. Its actual voltage,
polarity, boot behavior and use during suspend must be measured on the new board.
The active-low acknowledgement is the current firmware contract, not proof of a
validated Linux shutdown integration. Do not enable system suspend until it has
been distinguished from completed power-off. Tune the ONOFF pulse and 20 s timeout
only after observing Linux input events and shutdown logs.

## Clock and watchdog

`fuses.hex` configures:

- `WDTCFG=0x0B`: 8K watchdog cycles (about 8 s), locked after reset;
- `BODCFG=0x46`: 2.6 V brownout level;
- `OSCCFG=0x01`: 16 MHz oscillator;
- `SYSCFG0=0xC4`: PA0 remains UPDI;
- `SYSCFG1=0x07`: 64 ms startup.

Firmware selects a /16 prescaler for a 1 MHz CPU. TCB0 provides a 1 ms timebase;
all debounces require consecutive stable elapsed time. The CPU sleeps in idle
between interrupts. The watchdog is fed from the main loop, so a stalled main
loop still resets the MCU. A reset drops the rails and requires OFF -> ON.
No charger control, I2C protocol or UART service is implemented.

## Build and test

From the `LinHT-utils` repository root:

```sh
make -C pmu test
make -C pmu
```

If the host AVR compiler lacks ATtiny826 support:

```sh
docker run --rm -v "$PWD:/workspace" ghcr.io/slintak/avr-gcc:latest make -C /workspace/pmu
```

The output is `pmu/build/pmu-revC.hex`. Host tests cover normal/USB boot,
switch/button bounce, unstable-button fallback, cancelled startup, shutdown
acknowledgement/glitches, hard timeout, reset with switch ON, re-arming,
USB strap release during shutdown, and 32-bit timer wraparound.

## Programming power and wiring

**TP16 is the shared +3.3VP rail, not an isolated programmer supply input.**
It connects to U14 VCC, SoM RTC backup, GNSS backup and **U5 MCP1703A output**.
U5 input is connected to +BATT. Do not apply standalone 3.3 V to TP16 with U5
input unpowered: the MCP1703A absolute maximum for its output is VIN + 0.3 V;
reverse-current isolation is not provided by this board.
See [MCP1703A datasheet, DS20005122C, page 3](https://ww1.microchip.com/downloads/aemDocuments/documents/APID/ProductDocuments/DataSheets/MCP1703-Data-Sheet-DS20005122.pdf).

For initial programming:

1. Leave the radio switch OFF and disconnect USB-C. Supply the board through its
   battery connector using the battery or a current-limited lab supply matching
   the 2S battery voltage. Check approximately 3.3 V at TP16 relative to TP11.
2. Connect programmer GND to TP11, UART RX to TP7, UART TX to TP10 (the board
   includes the 1 kΩ resistor R14 between TX and UPDI). Use **3.3 V logic**.
   For a dedicated UPDI probe, use its documented UPDI connection to TP7.
3. Leave the USB/UART adapter's 3.3 V/5 V power output **disconnected**. Do not
   parallel it with U5. A dedicated probe's target-voltage sense is not a supply.
4. Program and verify, then disconnect programmer signal wires before normal
   battery operation. If changing power connections, disconnect the board supply
   first. Programming/reset may remove SoM power, so do not program a running Linux.

```sh
make -C pmu ping COM_PORT=/dev/ttyUSB0
make -C pmu fuses COM_PORT=/dev/ttyUSB0
make -C pmu program COM_PORT=/dev/ttyUSB0
make -C pmu read_fuses COM_PORT=/dev/ttyUSB0
```

Programming solely through TP16 requires an electrical isolation change (for
example isolating U5's output) and is not the procedure for an unmodified board.

## First hardware checks

- With the switch OFF, check +3.3VP and verify both regulator enables stay LOW;
  pressing the side button must not start the SoM.
- Measure PA3 and PA6 together: held button -> PA3 HIGH before PA6 rises;
  released button -> PA3 Hi-Z and normal boot. Confirm the roughly 40 ms sampling
  delay, 3 s rail delay and 5 s strap release on a scope/logic analyzer.
- Confirm USB download enumerates with `uuu -ls`; repeat with the key released
  immediately after power-on and with mechanical button bounce.
- Check PC0 is LOW for a press and rises only to the SoM's 1.8 V pull-up when
  released. Check both edges at runtime, including a held key across startup.
- Check knob-OFF produces the 200 ms ONOFF pulse, then an acknowledgement or the
  20 s hard cutoff. Check Linux poweroff and reboot separately.
- Verify PB5 pull-up is removed after every OFF path, and measure OFF current.
- Verify fuse readback, watchdog recovery, and that programmer-only signal wiring
  does not back-power a disconnected target.

Host tests and a successful AVR build do not validate voltage domains, rail ramps,
boot-strap capture, Linux shutdown or current consumption on physical hardware.
