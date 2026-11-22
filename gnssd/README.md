# linht-gnssd

GNSS daemon for LinHT Rev B and Rev C. It owns the Quectel LG77L receiver on
`/dev/ttyLP2` (LPUART3, 9600 8N1) and is the only program that writes to it.

What it does:

- parses the NMEA output (RMC, GGA, GSA, GSV, TXT, ZDA when present) and
  offers position, time, satellites, DOP and C/N0 on the system D-Bus,
- puts the receiver in standby and wakes it, restarts it (hot, warm, cold,
  full cold, or a RESET_N pulse) and sets the fix interval,
- watches the NMEA stream. Without valid NMEA for 10 s it opens the tty
  again, and after three such reopens in a row it pulses RESET_N,
- passes every valid sentence on to other readers through a pty and a unix
  socket, so gpsd or the FART field logger still get raw NMEA,
- sets the system clock and the battery-backed RTC (`/dev/rtc1`) from GNSS,
  two separate choices, see Time sync.

## Build and test

```sh
make            # the daemon, needs libsystemd (sd-bus)
make test       # unit tests on any Linux host with a fake receiver
make asan       # the same tests with AddressSanitizer and UBSan
make install DESTDIR=... PREFIX=/usr
```

The LinHT image has GCC and the libsystemd headers, so `make` and
`make test` also run on the unit itself. No other library is needed.

The tests feed recorded sentences: an epoch without a fix from Rev C and
Rev B (indoors, 2026-11-20), a 3D fix with GSV and C/N0 from the examples of
the Quectel protocol specification, the replies of the receiver and broken
input. The core tests drive the watchdog, the commands and the signals with
a fake receiver and a fake clock. The time sync tests use a fake system
clock and a fake RTC.

## Files

| File | Installed as |
|---|---|
| `linht-gnssd` | `/usr/bin/linht-gnssd` |
| `linht-gnssd.service` | `/lib/systemd/system/linht-gnssd.service` |
| `org.m17project.linht.Gnss1.conf` | `/usr/share/dbus-1/system.d/` |
| `gnssd.conf` | `/etc/linht/gnssd.conf` |
| NMEA pass-through | `/run/linht-gnssd/nmea` (pty), `/run/linht-gnssd/nmea.sock` |

## Configuration

`/etc/linht/gnssd.conf`, `key = value`, `#` starts a comment. A missing
file or key keeps the default.

| Key | Default | Meaning |
|---|---|---|
| `device`, `baud` | `/dev/ttyLP2`, 9600 | receiver UART |
| `stale_s` | 5 | s, older sentences are not used (at least 2.5 fix intervals) |
| `watchdog_s` | 10 | s without valid NMEA before the tty is opened again (at least 2.5 fix intervals) |
| `watchdog_reset` | `on` | RESET_N pulse after `reset_after_reopens` reopens in a row |
| `reset_after_reopens` | 3 | |
| `reset_min_interval_s` | 600 | at most one watchdog reset in this time |
| `reset_chip`, `reset_line` | `/dev/gpiochip2`, 3 | RESET_N, GPIO4_IO03. `reset_line = -1` turns it off |
| `reset_ms` | 150 | low time, the module needs at least 100 ms |
| `position_interval_s` | 5 | `PositionChanged` at most this often |
| `pty_link`, `socket` | see Files | pass-through, empty turns a part off |
| `feed_group` | `dialout` | group of the pty and the socket, mode 0660, like `/dev/ttyLP2` |
| `send_command` | `off` | `SendCommand` allowed (root only) |
| `system_clock` | `auto` | `auto`, `off` or `always`: whether the daemon steps the system clock, see Time sync |
| `rtc` | `on` | `on` or `off`: whether the daemon writes `rtc_device`, see Time sync |
| `time_sync_threshold_s` | 2 | s, a larger offset steps the system clock |
| `time_sync_delay_ms` | 125 | ms from the start of the second to the first byte of RMC, see Time sync |
| `rtc_device` | `/dev/rtc1` | battery-backed RTC, empty also turns the RTC writes off |

## D-Bus API

Bus name, interface and object path on the system bus:

```
org.m17project.linht.Gnss1
/org/m17project/linht/Gnss1
```

### Methods

| Method | What it does |
|---|---|
| `Standby()` | Sends `$PMTK161,0` and returns after its ACK `$PMTK001,161,3`. The module stops the RF and the NMEA output, the UART stays on. |
| `Wake()` | Sends one command (the version query), which ends the standby, and returns at the first RMC or GGA. |
| `Restart(s mode)` | `hot`, `warm`, `cold`, `full-cold` (`$PMTK101` to `$PMTK104`, no ACK by design) or `reset` (RESET_N pulse). `full-cold` also resets the configuration to factory values. |
| `SetUpdateRate(u milliseconds)` | `$PMTK220`, 1000 to 10000 ms in whole seconds, returns after the ACK. Faster rates do not fit through 9600 baud and are refused. |
| `SyncTimeNow() -> (d offset, b stepped, b rtc_written)` | Does now what `system_clock` and `rtc` allow, also below the threshold and before the hourly RTC write is due. Root only. See SyncTimeNow below. `offset` is the system clock minus GNSS before the call. |
| `SendCommand(s command) -> (as replies)` | Raw command for experiments, for example `PMTK414`. `$` and the checksum are added when missing. Returns the `$P...` sentences of the next 1.5 s. Root only, and only with `send_command = on`. The daemon does not track what the command changes. |

Errors: `org.m17project.linht.Gnss1.Error.Busy` (another command waits),
`.InvalidArgument`, `.NotSupported` (no reset line), `.NotOpen` (tty not
open), `.Timeout` (no ACK or no NMEA), `.Rejected` (PMTK001 flag 0 or 1),
`.Failed` (PMTK001 flag 2 or a failed clock step), `.Disabled`
(`send_command = off`, or `SyncTimeNow` with `system_clock = off` and
`rtc = off`), `.NoFix` (no stable GNSS time for `SyncTimeNow`),
`.NtpSynchronized` (NTP keeps the clock and it differs from GNSS by more
than the threshold).

The standby does not survive a restart of the daemon: every open of the tty
sends the version queries, which wake the module.

### Properties

All are read only. The first table sends `PropertiesChanged`.

| Property | Type | Meaning |
|---|---|---|
| `Fix` | s | `none`, `2D`, `3D`. `none` also when the data is stale |
| `Valid` | b | a fix with fresh data and a position |
| `Health` | s | `starting`, `ok`, `no-data` (watchdog), `tty-busy` (another reader holds the flock), `tty-error`, `standby` |
| `PowerState` | s | `on`, `standby` |
| `AntennaStatus` | s | text after `ANTSTATUS=` in `$GPTXT`, see Antenna status |
| `ModuleVersion` | s | `$PQVERNO` and `$PMTK705` replies |
| `TimeValid` | b | RMC status A |
| `LastTimeSync` | s | UTC of the last clock step or RTC write, empty = never |

Values, poll them with `GetAll` every 1 to 5 s:

| Property | Type | Meaning |
|---|---|---|
| `Latitude`, `Longitude` | d | degrees WGS84, NaN without a fix |
| `Altitude` | d | m above mean sea level (GGA), NaN without a 3D fix |
| `Speed`, `Course` | d | m/s and degrees true (RMC), NaN without a fix |
| `UtcTime` | s | ISO 8601 from RMC (or ZDA), also without a fix. Before the receiver knows the time it counts from 1980-01-06 |
| `SatellitesUsed` | u | GGA |
| `SatellitesUsedPerSystem` | a{su} | GSA, keys `GPS`, `GLONASS`, `Galileo`, `BeiDou`, `QZSS`, `SBAS` |
| `SatellitesInView` | u | GSV, every satellite the receiver lists |
| `SatellitesInViewPerSystem` | a{su} | same keys |
| `SatellitesTracked` | u | in view with a C/N0 |
| `Satellites` | a(sqnnd) | system, ID, elevation, azimuth (-1 = empty), C/N0 in dB-Hz (NaN = not tracked) |
| `Hdop`, `Pdop`, `Vdop` | d | GSA, NaN when empty |
| `CnoBest`, `CnoMean` | d | dB-Hz over the tracked satellites, NaN with none |
| `DataAgeSec` | d | since the last valid sentence |
| `UpdateIntervalMs` | u | measured from the RMC times, 0 = unknown |
| `Sentences`, `ChecksumErrors` | t, u | counters |
| `Reopens`, `Resets` | u | watchdog reopens and RESET_N pulses since the start |
| `Device`, `NmeaPty`, `NmeaSocket` | s | paths |
| `NmeaDropped` | u | lines a slow socket reader did not get |
| `SystemClockMode` | s | `system_clock`: `auto`, `off`, `always` |
| `RtcSync` | b | `rtc = on` and `rtc_device` not empty |
| `TimeSyncOffsetSec` | d | last measured system clock minus GNSS time (positive = system clock ahead), NaN before the first stable fix. Not reset by a step |

C/N0 is the carrier to noise density of each satellite in dB-Hz, not a
signal level in dBm. Outdoors with a working antenna the best satellites
show about 40 to 50 dB-Hz. `CnoBest` and `SatellitesTracked` are the first
values to look at when the receiver gets no fix.

### Signals

- `PropertiesChanged` for the first table.
- `FixChanged(s fix, b valid)` when `Fix` or `Valid` changes.
- `PositionChanged(d latitude, d longitude, d altitude)` while there is a
  fix, at most every `position_interval_s`.

### Access

Only root owns the name. Everyone reads the properties. Root and the group
`plugdev` (the `linht` user is in it) call `Standby`, `Wake`, `Restart` and
`SetUpdateRate`. `SendCommand` and `SyncTimeNow` are for root only, in the
D-Bus policy and again in the daemon.

### busctl examples

```sh
B="org.m17project.linht.Gnss1 /org/m17project/linht/Gnss1 org.m17project.linht.Gnss1"
busctl --system get-property $B Fix
busctl --system get-property $B CnoBest
busctl --system get-property $B SatellitesInViewPerSystem
busctl --system --json=pretty call org.m17project.linht.Gnss1 /org/m17project/linht/Gnss1 \
    org.freedesktop.DBus.Properties GetAll s org.m17project.linht.Gnss1
busctl --system call $B Standby
busctl --system call $B Wake
busctl --system call $B Restart s cold
busctl --system call $B SetUpdateRate u 2000
busctl --system call $B SendCommand s PMTK414
busctl --system call $B SyncTimeNow
busctl --system monitor org.m17project.linht.Gnss1
```

## Client modes

The same binary is also a small client:

```sh
linht-gnssd status            # all properties, one per line
linht-gnssd status --json     # one JSON line, NaN is null
linht-gnssd standby
linht-gnssd wake
linht-gnssd restart cold      # hot, warm, cold, full-cold, reset
linht-gnssd rate 1000
linht-gnssd send PMTK605      # prints the replies, root only
linht-gnssd sync-time         # SyncTimeNow, root only
```

## NMEA pass-through

The daemon holds `flock` on `/dev/ttyLP2`, the same lock as
`gnss_status.py` and the FART field logger, so they refuse to open the tty
while it runs. Every valid sentence goes on to:

- the pty `/run/linht-gnssd/nmea` for one tty reader, for example
  `gpsd -n -b /run/linht-gnssd/nmea` (read only mode, gpsd must not
  configure the receiver) or `field-log.py run --gnss /run/linht-gnssd/nmea`.
  The field logger works with it unchanged (tested on Rev C). When nobody
  reads the pty, the backlog is dropped above 1 KiB, so a new reader gets
  at most about two seconds of old sentences first.
- the socket `/run/linht-gnssd/nmea.sock` for any number of readers, for
  example `socat - UNIX-CONNECT:/run/linht-gnssd/nmea.sock`.

Both are read only. What a reader writes is dropped and never reaches the
receiver.

The field logger should later read the D-Bus properties instead of the tty
or the pty. That gives it the C/N0, the satellites in view and the health of
the receiver without its own NMEA parser.

## Time sync

LinHT has two RTCs. The kernel uses `rtc0`, the BBNSM of the i.MX93, which
has no backup supply. `rtc1` is the AB1805 on the SoM. Its backup supply
is `VCC_RTC` (SoM pin 127) on `+3.3VP`, the MCP1703A that runs from the
battery pack (`+BATT`) and is always on. There is no coin cell, so the
AB1805 keeps the time while a charged pack is in the unit and loses it when
the pack is out or empty. Its driver then logs "Oscillator failure, data
is invalid" and refuses every read until the next write.
`rtc1-hctosys.service` sets the system clock from `rtc1` at boot. Nothing
else sets `rtc1`, so the daemon keeps it right from GNSS. When the AB1805
has lost its time, the first stable fix sets the system clock and `rtc1`.

The receiver time counts only with a stable fix: RMC status A, a 3D fix
with at least 4 satellites, a year not before the build year of the daemon,
and 5 epochs in a row in which the receiver time runs with the monotonic
clock (within 0.2 s). A receiver without time reports 1980-01-06 and a GPS
week rollover goes back 19.6 years, both are below the build year. The
offset is the median of the last 5 epochs.

Two keys decide what the daemon does with a stable fix. Each one is a
separate choice of the operator:

| `system_clock` | The system clock |
|---|---|
| `auto` | stepped (`clock_settime`) when it is not valid (before the build year of the daemon) or the offset is above `time_sync_threshold_s`, unless NTP keeps the clock |
| `always` | as `auto`, but NTP is ignored |
| `off` | never stepped |

| `rtc` | `rtc_device` |
|---|---|
| `on` | written with the GNSS time after each step, at the first stable fix, and then at most once per hour |
| `off` | never written |

"NTP keeps the clock" is the test of `timedatectl` NTPSynchronized, an
`adjtimex` maximum error below 16 s, with systemd-timesyncd or chrony.
When NTP keeps the clock and GNSS differs from it by more than the
threshold, the daemon cannot tell which one is wrong. It then steps nothing
and writes nothing, and logs a warning once per hour. With
`system_clock = always` this case does not exist, the clock is stepped.

A clock before the build year is never taken as kept by NTP. systemd
starts the clock at its own build time (February 2024 on the current
image) when `rtc1` has nothing, and an `ntpd` with only the local clock
`127.127.1.0` (older Rev B images) reports sync without a source. Such a
clock is stepped in `auto` mode at the first stable fix.

`rtc = on` with `system_clock = off` writes the GNSS time to the RTC, not
the system time, so the RTC is right also when the system clock is not.
`TimeSyncOffsetSec` is measured in every mode.

The defaults are `system_clock = auto` and `rtc = on`, so a fresh unit gets
a valid clock at its first fix and keeps it over a power cycle. The LinHT
image has no NTP client running and the unit is often without a network,
so GNSS is the only time source. `auto` does not fight an NTP client on a
unit that has one.

A failed step (no `CAP_SYS_TIME`) is tried again after 10 minutes. Each
step and each RTC write goes to the journal:

```
system clock stepped by -2.540 s from GNSS, 2026-11-21T08:56:01Z -> 2026-11-21T08:55:59Z
/dev/rtc1 set to 2026-11-21T08:56:00Z from GNSS (after the step)
```

`rtc1` counts whole seconds and its driver clears the hundredths when it
is written, so the daemon waits for the next whole second before the write
(up to 1 s, once per hour). The daemon runs as root, which gives it
`CAP_SYS_TIME` for both. At start the journal shows both choices:

```
running, bus name org.m17project.linht.Gnss1, /dev/ttyLP2 at 9600 baud, system clock auto, RTC /dev/rtc1
```

### SyncTimeNow

`SyncTimeNow` (`linht-gnssd sync-time`) does at once what the two keys
allow, without the threshold and without waiting for the hourly RTC
write. It needs a stable fix like the automatic sync.

| `system_clock` | `rtc` | `SyncTimeNow` |
|---|---|---|
| `auto` | `on` | steps the clock (offset of 1 ms or more), writes the RTC. A clock kept by NTP is not stepped, only the RTC is written |
| `auto` | `off` | steps the clock as above, no RTC write |
| `always` | `on` | steps the clock also against NTP, writes the RTC |
| `always` | `off` | steps the clock also against NTP, no RTC write |
| `off` | `on` | writes the GNSS time to the RTC, no step |
| `off` | `off` | refused with `.Disabled` |

In `auto` and `off` mode, a clock kept by NTP that differs from GNSS by
more than the threshold gives `.NtpSynchronized` and nothing changes.
`stepped` and `rtc_written` tell what was done. `rtc_written` is false
with an empty `rtc_device`.

### Accuracy without 1PPS

The 1PPS line of the module is not used, so the time comes from the NMEA
second boundary. The RMC time is the start of the epoch, and RMC is the
first sentence of the epoch. The daemon takes the system time when the
RMC line is complete and subtracts the time of the line on the wire (76
characters at 9600 baud are 79 ms) and `time_sync_delay_ms`, the delay of
the receiver and the UART driver from the start of the second to the first
byte.

The default of 125 ms comes from one measurement on Rev C (2026-11-21,
indoors near a window, a 3D fix with 4 to 5 satellites). With
`time_sync_delay_ms = 0` the daemon stepped the clock, and the clock was
then 127 ms behind an NTP synchronised PC on the LAN (offset measured over
ssh with a round trip below 1 ms). The RMC arrival jitters by about 30 ms
from epoch to epoch, and the median of 5 epochs takes most of that out.
Without a fix the receiver epochs drift against the true second, which is
one more reason to use only a stable fix.

With the default the error should be a few tens of milliseconds (to be
confirmed outdoors with a better fix), and below 0.5 s in any case. To measure the delay again, run the unit with NTP
synchronised and `time_sync_delay_ms = 0`: `TimeSyncOffsetSec` is then the
delay in seconds. For better than a few milliseconds the 1PPS on
GPIO4_IO08 would be needed.

## Receiver commands

From the Quectel Lx0&Lx6&LC86L&LG77L GNSS Protocol Specification V2.0 and
the LG77L(C) Hardware Design V1.3. "Rev C" means checked on the Rev C unit on
2026-11-20 (firmware `LG77LICNR11A01S_GLN`, chipset
`MT3333_AXN5.1.9_MODULE_STD_F9_P1`).

The LG77L uses MediaTek PMTK and Quectel PQ commands. It does not use the
Airoha PAIR or PQTM command sets of newer Quectel modules.

| Command | Use | Status |
|---|---|---|
| `$PMTK161,0` | standby, any byte wakes | spec, Rev C (ACK, NMEA stops, wakes) |
| `$PMTK101` to `$PMTK104` | hot, warm, cold, full cold restart, no ACK | spec, Rev C hot, Rev B cold (time back to 1980) |
| `$PMTK220,ms` | fix interval 100 to 10000 ms | spec, Rev C 2000 and 1000 ms (ACK, interval follows) |
| `$PMTK314,...` / `$PMTK414` | NMEA sentence rates / query | spec, Rev C query: GLL RMC VTG GGA GSA GSV every fix |
| `$PMTK353,gps,glo,gal,0,bds` | constellation selection | spec only, not tried. GLONASS and BeiDou cannot be on together |
| `$PMTK225,...` | periodic and backup modes | spec only, not used, see WAKEUP below |
| `$PMTK605` | firmware release (`$PMTK705`) | spec, Rev C |
| `$PMTK869,0` | EASY (self generated orbit prediction) query | spec, Rev C: enabled, 0 days |
| `$PMTK413`, `$PMTK401` | SBAS and DGPS query | spec, Rev C: SBAS on, DGPS mode 2 (SBAS) |
| `$PQVERNO,R` | Quectel firmware version | Quectel forum answer only, not in spec V2.0, Rev C |

The daemon uses `PMTK161`, `PMTK101` to `PMTK104`, `PMTK220`, `PMTK605`
and `PQVERNO`. Everything else is left to `SendCommand`. AGNSS with host
supplied EPO data is not supported. The EASY function in the module is on
by default.

## Hardware notes

From the LinHT netlist and the device tree (`sbc-mcm-imx93.dtsi`):

- VCC on +3V3, VCC_IO on +1V8, V_BCKP on +3.3VP (the SoM RTC supply). There
  is no power switch for the module, so the deepest software controlled
  state is standby (about 1 mA by the specification, against about 25 mA
  in acquisition).
- RESET_N on SoM pin 83, GPIO4_IO03, `gpiochip2` line 3. Active low, pulled
  up inside the module. The daemon drives it low for 150 ms and then makes
  the line an input again. Tested on Rev C: the module restarts and sends
  its start-up message.
- WAKEUP on SoM pin 85, GPIO4_IO02, `gpiochip2` line 2. The daemon never
  drives it. WAKEUP is in the backup domain of the module, its high level
  is 0.825 to 1.4 V (Hardware Design, Table 11) and it must be low before
  the module can enter backup mode. A 1.8 V output is above that range. The
  pad setting 0x31e in the device tree enables the i.MX93 pull-up on this
  pin, and the line reads high on both units. Whether that keeps the pin
  above 1.4 V against the internal pull-down of the module was not
  measured.
- 1PPS on SoM pin 87, GPIO4_IO08. Not used by the daemon, see Time sync.
- ANT_DET, ANT_SHORT and ANT_OFF are not connected.

### Antenna status

`$GPTXT,01,01,02,ANTSTATUS=...` comes from the active antenna detection
pins. They are not connected on LinHT, so `OPEN` is what the module always
reports and it says nothing about the antenna. Rev C uses a passive antenna
(u.FL, pigtail and a tuned wire), and there is no DC bias for an active one.

## Long tests

Not part of `make test`:

- Outdoors with a clear sky: `linht-gnssd status` every minute for 30 min
  after a cold restart. Compare `CnoBest`, `SatellitesTracked` and the time
  to the first fix between Rev B and Rev C and with a known good antenna on
  the u.FL.
- Standby current: battery current from `linht-chargerd` with and without
  `linht-gnssd standby`, on battery.
- gpsd on the pty: `gpsd -N -n -b /run/linht-gnssd/nmea` and `cgps`.
- Time sync outdoors: set the system clock 1 h wrong (`date -s`), wait for
  the fix, check the journal for one step and one `rtc1` write, then
  `hwclock -r -f /dev/rtc1` and `TimeSyncOffsetSec` near 0. Measure
  `time_sync_delay_ms` as in Accuracy without 1PPS.
