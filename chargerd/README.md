# linht-chargerd

Charger daemon for LinHT Rev B and Rev C. It owns the TI BQ25792 charger on
I2C bus 2, address 0x6B, and is the only program that writes to it.

What it does:

- keeps the charger in host mode and kicks its 40 s watchdog every 10 s,
- sets the charge voltage (VREG) from the config, 8.30 V by default,
- reads the charger ADC once per second (continuous conversion, 15 bit),
  and sets the charger up again when the ADC stops on USB,
- offers the readings, averages and battery estimates on the system D-Bus,
- gives quiet leases. A lease puts the charger in HIZ, the only state in
  which the charger does not raise the receiver noise floor,
- learns the battery capacity and the state of health from discharges,
  from full ones (reliable) and from partial ones (low precision), in
  quiet on USB and on battery in the field.

It does not handle the i.MX93 SoC ADC (`iio:device0`, its calibration failure
on Rev C and the volume knob). The battery voltage comes only from the
BQ25792.

## Build and test

```sh
make            # the daemon, needs libsystemd (sd-bus)
make test       # unit tests on any Linux host with a fake chip
make asan       # the same tests with AddressSanitizer and UBSan
make install DESTDIR=... PREFIX=/usr
```

The LinHT image has GCC and the libsystemd headers, so `make` and
`make test` also run on the unit itself. No other library is needed.

## Files

| File | Installed as |
|---|---|
| `linht-chargerd` | `/usr/bin/linht-chargerd` |
| `linht-chargerd.service` | `/lib/systemd/system/linht-chargerd.service` |
| `org.m17project.linht.Charger1.conf` | `/usr/share/dbus-1/system.d/` |
| `chargerd.conf` | `/etc/linht/chargerd.conf` |
| state | `/var/lib/linht-chargerd/state.json`, `cycles.jsonl` |

## Configuration

`/etc/linht/chargerd.conf`, `key = value`, `#` starts a comment. A missing
file or key keeps the default.

| Key | Default | Meaning |
|---|---|---|
| `i2c_dev`, `i2c_addr` | `/dev/i2c-2`, `0x6b` | charger on I2C |
| `pfm` | `off` | PFM in forward mode. `off` is PFM_FWD_DIS = 1, use `on` on Rev B |
| `full_hold` | `off` | HIZ with a full battery on USB until the SoC falls to `full_hold_resume_soc` |
| `full_hold_resume_soc` | 90 | % |
| `quiet_max_s` | 300 | longest lease in seconds |
| `quiet_min_soc` | 25 | % on USB, new leases are refused below |
| `quiet_end_soc` | 20 | % on USB, all leases end and charging resumes |
| `quiet_resume_soc` | 40 | % leases are allowed again after a low battery end |
| `quiet_min_vbat`, `quiet_end_vbat` | `auto` | V, used instead while the SoC is not anchored at full. `auto`: the end limit is the voltage of `quiet_end_soc` on the voltage table, the start limit 0.10 V above (7.42 and 7.32 V at 8.30 V VREG, 7.43 and 7.33 V at 8.40 V) |
| `low_soc`, `critical_soc` | 15, 5 | % for the `LowBattery` signal on battery |
| `battery`, `cells` | Retevis C62, 2 | pack description, series cells |
| `charge_voltage_v` | 8.30 | V per pack, written to VREG. 4.00 to 4.20 V per cell (8.00 to 8.40 V for 2S), the daemon refuses to start outside. See [Charge voltage](#charge-voltage) |
| `capacity_rated_wh`, `capacity_rated_mah` | 14.8, 2000 | from the pack label |

A new chassis or pack only changes the battery lines.

## D-Bus API

Bus name, interface and object path on the system bus:

```
org.m17project.linht.Charger1
/org/m17project/linht/Charger1
```

### Methods

| Method | What it does |
|---|---|
| `Quiet(u seconds) -> (u granted)` | Creates or renews the lease of the caller. One lease per bus connection. 1 to 300 s, 0 releases. `granted` is the clamped length. |
| `Release()` | Same as `Quiet(0)`. |
| `SetCharging(b enable)` | EN_CHG. Not stored, charging is on after every start. |
| `GetRegisters() -> (ay)` | REG00 to REG48 of the last poll. REG22 to REG27 hold every flag seen since the previous call. |

Errors: `org.m17project.linht.Charger1.Error.LowBattery`, `.Fault`, `.Chip`
and `.TooManyHolders` (more than 16 holders).

Lease rules:

- Quiet is on while at least one lease is valid. Holders are independent.
- A lease ends at its expiry, on `Release`, or when its holder leaves the
  bus. A client that crashes loses its lease at once. Renew before expiry,
  for example `Quiet(120)` every 60 s.
- On battery a lease is granted and costs nothing.
- On USB a lease is refused below `quiet_min_soc`. All leases end below
  `quiet_end_soc`, and new ones are refused until `quiet_resume_soc`.
- A charger fault ends all leases and refuses new ones. The one exception
  is VBAT_OVP at the end of charge (see below): leases are kept and granted,
  because HIZ stops the charging, the safest thing during that OVP. The
  battery limits above still apply.

### Properties

All are read only. The ones in the first table send `PropertiesChanged`.

| Property | Type | Meaning |
|---|---|---|
| `State` | s | `on-battery`, `charging`, `full`, `full-hold`, `quiet`, `charge-off`, `fault`, `chip-error` |
| `ChargeStatus` | s | `not-charging`, `trickle`, `precharge`, `fast`, `taper`, `top-off`, `done` |
| `UsbPresent` | b | VBUS present and power good |
| `InputLimit` | s | `none`, `current` (IINDPM), `voltage` (VINDPM) |
| `ChargingEnabled` | b | the `SetCharging` setting |
| `Quiet` | b | at least one lease and the radio is quiet |
| `QuietHolders` | u | number of valid leases |
| `QuietBlockedReason` | s | empty, `low-battery`, `fault`, `chip` |
| `Pfm` | b | PFM in forward mode enabled |
| `Faults` | as | fault names from REG20, REG21 and the recent fault flags |
| `CapacityReliable` | b | one learning discharge of 70 % or two of 50 % |
| `StateOfHealthSource` | s | capacity behind the SoH and the energy estimates: `full` (reliable, from full anchored discharges), `partial` (no reliable value yet, partial cycle estimate, low precision), `none` (neither, `CapacityLearnedWh` as it is) |

Measurements, poll them with `GetAll` every 1 to 5 s:

| Property | Unit | Meaning |
|---|---|---|
| `BatteryVoltage`, `SystemVoltage`, `UsbVoltage` | V | ADC VBAT, VSYS, VBUS |
| `BatteryCurrent` | A | positive charges, negative discharges |
| `UsbCurrent` | A | ADC IBUS |
| `BatteryPower`, `UsbPower` | W | V times I |
| `DieTemperature` | °C | charger die, not the battery |
| `ChargeVoltage` | V | VREG read back from the charger |
| `MeasurementAgeSec` | s | time since the ADC results last changed |
| `ChargerRecoveries` | u | charger init attempts for a stopped ADC since the start, see below |
| `BatteryPowerAvg1m`, `Avg10m`, `Avg60m` | W | NaN with less than half a window |
| `BatteryCurrentAvg1m`, `Avg10m`, `Avg60m` | A | same |
| `StateOfChargePercent` | % | estimate |
| `StateOfChargeSource` | s | `counter` (anchored at full) or `voltage` (table) |
| `EnergyRemainingWh` | Wh | estimate |
| `RunTimeRemainingSec` | x | down to `critical_soc`, -1 when not on battery |
| `TimeToFullSec` | x | -1 when not charging, optimistic in CV |
| `OnBatterySec`, `ChargingSec` | t | since the battery started to supply the unit, or since the charge started |
| `CapacityRatedWh`, `CapacityLearnedWh` | Wh | from the config, learned from full anchored discharges |
| `CapacityPartialWh` | Wh | low precision estimate from partial cycles (quiet and on battery), NaN before the first one |
| `CapacityPartialSamples` | u | partial cycles used |
| `CapacityPartialWeight` | | sum of their decayed weights, about 1 per 20 % deep cycle at light load. Below 2 the estimate is rough |
| `StateOfHealthPercent` | % | capacity in use / rated, see `StateOfHealthSource` |
| `EquivalentCycles` | | all discharged energy / rated capacity |

### Signals

- `PropertiesChanged` for the state properties.
- `LowBattery(s level, d percent)`, `level` is `low` or `critical`, once per
  crossing while on battery. The daemon does not shut the unit down.

### Access

Only root owns the name. Everyone reads the properties. Root and the group
`plugdev` (the `linht` user is in it) call the methods.

### busctl examples

```sh
B="org.m17project.linht.Charger1 /org/m17project/linht/Charger1 org.m17project.linht.Charger1"
busctl --system get-property $B StateOfChargePercent
busctl --system get-property $B State
busctl --system --json=pretty call org.m17project.linht.Charger1 /org/m17project/linht/Charger1 \
    org.freedesktop.DBus.Properties GetAll s org.m17project.linht.Charger1
busctl --system call $B SetCharging b false
busctl --system call $B SetCharging b true
busctl --system call $B GetRegisters
busctl --system monitor org.m17project.linht.Charger1
```

`busctl call $B Quiet u 120` returns the granted length, but the lease ends
at once, because busctl leaves the bus when it exits. Use the client mode
below from shells and scripts.

## Client modes

The same binary is also a small client:

```sh
linht-chargerd status            # all properties, one per line
linht-chargerd status --json     # one JSON line, NaN is null
linht-chargerd quiet 300         # holds a lease for 300 s, renews it, releases on exit
linht-chargerd quiet 0           # holds a lease until Ctrl-C or SIGTERM
linht-chargerd --restore         # stock charger values without D-Bus (systemd uses it)
```

`quiet` exits with status 1 when the daemon refuses the lease, for example
on a low battery.

From C: `sd_bus_open_system()` once, `sd_bus_get_property_trivial()` for a
single value and `sd_bus_call_method()` for `Quiet`. Keep the connection
open while the lease is needed.

## Charger policy

The daemon writes only these bits, always read, modify, write:

| Register | Bits | Why |
|---|---|---|
| REG01, REG02 | VREG | `charge_voltage_v` |
| REG0F | EN_HIZ, EN_CHG | quiet, `SetCharging` |
| REG10 | WD_RST | watchdog kick every 10 s, the 40 s setting stays |
| REG12 | PFM_FWD_DIS | `pfm` |
| REG14 | EN_IBAT | discharge current for the ADC |
| REG2E | ADC_EN, ADC_RATE, ADC_SAMPLE | continuous, 15 bit |

It never writes REG13 (PWM_FREQ, 750 kHz is not allowed with the 1 uH L9),
VINDPM, IINDPM, ICHG, REG_RST, the protection thresholds or the ADC channel
disables. At start it
refuses to run when REG48 does not show a BQ25792, and it warns when CELL is
not the configured cell count or PWM_FREQ is not 1.5 MHz.

Every poll compares the bits with what the daemon wants and sets them again
when they differ (watchdog expiry, USB plug in, a reset of the charger,
another writer). VREG is part of this, so a lost VREG is set again within
one second, also during a stopped ADC and after its recovery. Such a
difference is logged once per minute.

States, in priority order:

| State | When | Charger |
|---|---|---|
| `chip-error` | 3 failed polls in a row | nothing possible, retry every 5 s, leases kept |
| `fault` | REG20 or REG21 fault, or a fault flag in the last 10 s. VBAT_OVP at the end of charge is shown as `full` or `quiet`, see below | HIZ off, quiet refused |
| `on-battery` | no USB power good | nothing to switch |
| `quiet` | USB and at least one lease | EN_HIZ = 1 |
| `full-hold` | USB, battery full, `full_hold = on` | EN_HIZ = 1 until the SoC is below `full_hold_resume_soc` |
| `charge-off` | `SetCharging(false)` | EN_CHG = 0 |
| `full` | CHG_STAT done, or VBAT at least VREG minus 0.05 V with IBAT below 0.2 A for 60 s. VREG is the read back value, so this follows `charge_voltage_v` | EN_HIZ = 0 |
| `charging` | the rest | EN_HIZ = 0, EN_CHG = 1 |

The second `full` rule is for a weak USB source, where the charger stays in
VINDPM and never terminates. `full` ends after 10 s with IBAT above 0.3 A or
VBAT more than 0.15 V below VREG.

### VBAT_OVP at the end of charge and a stopped ADC

The Rev C unit showed this twice at the end of a charge: in constant voltage
at about 8.44 V and 0.23 A (VREG 8.40 V) the charger set VBAT_OVP, stopped
charging, and from then on every ADC result stayed the same, for hours. The
data sheet says that the ADC keeps converting after a fault, so this is
chip behaviour the data sheet does not describe. Turning ADC_EN off and on
by hand did not restart the ADC. A restart of the daemon did, and the
charge went on at once.

The daemon now does the same writes as a restart by itself:

- When the ADC results have not changed for 60 s, VBUS is present and the
  charger is not in HIZ, it writes the stock values of the clean stop except
  REG0F (PFM_FWD_DIS 0, EN_IBAT 0, REG2E 0x30, WD_RST). One poll later it
  writes the start-up values again (PFM, EN_IBAT, REG2E continuous 15 bit,
  WD_RST). The journal says "charger init again" and `ChargerRecoveries`
  counts the attempts.
- If the results stay frozen, the next attempt waits 120 s, then 240 s and
  so on, at most one hour. Results that stay fresh for 10 min start this
  over, so every end of charge gets a quick attempt.
- HIZ turns the ADC off by design. A quiet lease or full-hold therefore
  never starts a recovery, and the 60 s start again when HIZ ends. On
  battery there is no recovery either.
- The recovery never writes REG0F. EN_CHG stays as `SetCharging` has it and
  the charger's own OVP keeps the converter off as long as it is active.
  The start-up writes include VREG when it differs from `charge_voltage_v`.
  ICHG, the input limits and the protections are never written.

A VBAT_OVP that comes right at the end of charge is shown as State `full`,
not `fault`, when all of this holds: VBAT_OVP is the only fault (status or
flag), VBUS is present, VREG is not above 4.2 V per cell, and the last fresh
VBAT was between VREG minus 0.05 V and 101 % of VREG (the lowest OVP release
level of the data sheet, 8.48 V at 8.40 V, 8.38 V at 8.30 V). VREG is the
read back value, so the window follows `charge_voltage_v`. The battery is then at the end
of its charge and the charger only stopped early. `Faults` still lists
`vbat-ovp`, but quiet is not blocked: a lease puts the charger in HIZ, which
stops the charging and is the safest state during this OVP. State is then
`quiet`, `QuietBlockedReason` is empty, and the limits of `quiet_min_soc`
and `quiet_end_soc` apply as usual. As in every HIZ there is no ADC
recovery during the lease. The SoC is not anchored at 100 %, because the charge did not terminate (the
current was above the 0.2 A of the `full` rule) and the frozen results
cannot show the true end. Any other VBAT_OVP, for example with a fresh VBAT
above 101 % of VREG, stays `fault`.

### Fail safe

The charger watchdog resets EN_CHG, EN_IBAT and ADC_EN, but not EN_HIZ and
PFM_FWD_DIS. So:

- A clean stop (SIGTERM) releases the bus name and writes stock values:
  EN_HIZ 0, EN_CHG 1, PFM_FWD_DIS 0, EN_IBAT 0, REG2E 0x30, WD_RST. It logs
  the read back of REG0F, REG10, REG12, REG14 and REG2E (0xa2, 0x85, 0x00,
  0x16, 0x30 on our units).
- The unit file runs `linht-chargerd --restore` in `ExecStopPost`, after
  any exit, also after SIGKILL or a crash.
- Every start clears EN_HIZ first, because leases do not survive a restart.
- The stop and `--restore` leave VREG at `charge_voltage_v`. A lower charge
  voltage is the safe side. A power on reset of the charger sets VREG back
  to the value of its PROG pin (8.40 V on our units).
- Replugging USB clears EN_HIZ in the charger itself.

Other charger tools must stop the daemon first (`systemctl stop
linht-chargerd`). The daemon holds `flock` on `/dev/i2c-2`, so
`bq25792_status.py` refuses to run next to it. `i2cget` still works for
reads, but do not read REG22 to REG27 by hand, the read clears the flags.

## Charge voltage

`charge_voltage_v` (default 8.30 V, 4.15 V per cell) is written to VREG
(REG01, REG02, 10 mV steps) at start and whenever the read back differs.
Both bytes go in one I2C transfer: the Rev C charger ignored a one byte
write to REG02 (the read back stayed 8.40 V), a two byte write from REG01
took at once.
`ChargeVoltage` and the start log line show the read back value.
Everything that depends on the end of charge follows it: the voltage
table (100 % at VREG), the second `full` rule (VREG minus 0.05 V), the
full anchor and so the reliable learning, the VBAT_OVP at full window
(VREG minus 0.05 V to 101 % of VREG) and the `auto` quiet voltage limits.

The trade-off: a Li-ion NMC cell charged to 4.15 V instead of 4.20 V
stores about 8 to 10 % less energy (the generic table of the daemon says
5 %, it is coarse near full), and spends less time at the high voltage
that ages it fastest, so it gives clearly more cycles and lasts longer
when it is kept on USB. 8.40 V gives the longest run time. The charger
terminates the charge at VREG with its termination current, so the SoC is
100 % at either setting and `CapacityLearnedWh` learns the energy of the
chosen setting (expect it to drop by that amount after a change).

VBAT_OVP is not the charge target. It is a protection threshold of the
charger at about 104 % of VREG (8.74 V at 8.40 V, 8.63 V at 8.30 V). The
repeated VBAT_OVP of the Rev C unit at about 8.44 V (VREG 8.40 V) is far
below that threshold and is not explained. The lower VREG is not a fix for
it. Running at 8.30 V will show whether the OVP changes, it moves the end
of charge 0.10 V down.

## Estimates

Only VBAT, IBAT and time are measured. The rest is built on them.

- Energy counter: E += VBAT times IBAT times dt, every second.
- Full anchor: the `full` rule sets the SoC to 100 % and the counter to 0.
- SoC = 100 times (1 + E / C), with C the capacity in use (below). Without an
  anchor the SoC comes from a generic 2S NMC voltage table, 10 to 20 % off
  under load. After a restart the saved counter is used again when the
  voltage agrees within 15 %.
- The table has 100 % at 4.20 V per cell. The daemon scales it so that
  `charge_voltage_v` is 100 %: at 8.30 V the table value of 4.15 V per cell
  (95 %) becomes 100 %, 7.60 V becomes 52.6 % instead of 50 %. So the
  voltage SoC, the counter SoC after a full anchor, the depth of the
  learning discharges and the `auto` quiet voltage limits all refer to the
  energy of a charge to the configured VREG.
- Learning: a discharge from a full anchor at a mean load below 3 W that
  ends at a voltage SoC of 50 % or less gives C_est = E_out / (1 - SoC_v / 100).
  C moves towards C_est with the weight 0.5 times the depth, clamped to 50
  to 110 % of rated.
- Partial cycles: the FART auto mode keeps the battery between about 50 and
  70 % (quiet while the SoC is at least 50 %, charge back to 70 %), and in
  the field the radio runs on battery and is charged to whatever level the
  user likes. Neither reaches a full anchor and an empty battery. Both are
  learned from rest points (see the next point). A segment runs from a
  rest point to the last rest point before a charge current above 0.05 A
  (or a full anchor) ends it. The energy (VBAT times IBAT) is counted in
  between, whatever the load is, so TX in the field is no problem. A short
  gap without charge current, for example between two leases, does not end
  the segment. A segment of at least 15 % by the table, with a mean current
  up to 0.6 A between its two rest points, gives
  C_est = E_out / (SoC_v start - SoC_v end), dropped outside 40 to 160 %
  of rated. Its weight is (depth / 20 %) squared, at most 9, divided by
  1 + (I_rest / 0.25 A) squared, with I_rest the mean load at the two rest
  points: about 0.6 for a 20 % segment in quiet or idle RX (0.16 A), about
  6 for a 60 % field day. The estimate starts at `CapacityLearnedWh` with the
  weight 1, every new segment first multiplies the old weights by 0.9, then
  the estimate moves towards C_est by its share of the weight. So one
  shallow segment changes a settled estimate by about a tenth of its error
  at most, and old segments fade out as the battery ages. Clamped to 50 to
  110 % of rated.
- Rest point: a moment after at least 10 min of discharge without charge
  current, with a mean load of at most 0.3 A over those 10 min and over the
  last minute. VBAT (30 s average) is then close to the open circuit voltage
  and the table gives the SoC. In quiet on USB the first 10 min after the
  charge are skipped this way (VBAT relaxes after the charge), after that
  every second is a rest point. On battery a charge cannot happen, so the
  rule is the load: idle RX (about 0.16 A) is a rest, TX is not. Use in the
  field with TX every few minutes has no rest point, the idle time before
  it (from 10 min after the unplug) and after it (10 min or more) gives the
  two ends. A USB charge ends the segment, so no segment spans a charge.
  When the unit is switched off or the daemon stops, the segment so far
  (start and last rest point) is in `state.json` and is evaluated at the
  next start. The voltage table itself is not learned, `cycles.jsonl` keeps
  the discharge curves for a later fit.
- Accuracy of the partial estimate. The table is generic, a few 10 mV per
  pack off is 1 to 3 % of SoC at each end. The IR drop at the rest load
  (0.16 A times about 0.15 ohm, 25 mV) is about the same at both ends, but
  the table is steeper near full than near 30 %, so it reads the depth a
  little too large and C_est a few % too low. After heavy TX the cells
  relax for longer than 10 min, a rest point right after use reads a few
  10 mV low. The integration of VBAT times IBAT is better than 1 %. In the
  tests with the synthetic battery (table error up to 10 mV per pack at
  each end, IR drop, slow polarization) a single 60 % field day comes out
  about 6 % low (the IR drop effect above), a 20 % quiet cycle within
  10 %, and several of either settle within 10 % of the true capacity. A steady offset against a full
  anchored learning means the table does not fit the cells.
- Capacity in use (`StateOfHealthSource`): the learned one when it is
  reliable, else the partial estimate when there is one, else the learned
  one as it is. The SoC counter, `EnergyRemainingWh`, the run times and
  `StateOfHealthPercent` use it. The partial estimate rests on a generic
  table, so a table that does not fit the cells shifts it by a few %.
  Each partial segment is logged as "partial cycle" in the journal.
- Run time = (SoC - `critical_soc`) times C / the 10 min power average (the
  1 min average until 10 min are filled).

`state.json` holds the learned capacity, the partial estimate with its
weight and count, the counter, the anchor and a partial segment in
progress (evaluated at the next start). It is
written atomically every 15 min, on a full anchor, at the end of a cycle and
at a clean stop. `cycles.jsonl` gets one line per discharge or charge of at
least 10 %: times, VBAT and SoC at start and end, energy, charge, mean power,
maximum die temperature and VBAT at every 5 % of the energy. It keeps the
newest 1000 lines.

## Long tests

These need hours and are not part of `make test`:

- Capacity and run time: let the battery reach `full` on USB, then unplug
  USB and let the unit run idle RX until it switches off. The next start
  drops the old counter (the voltage no longer agrees), evaluates the
  discharge with its last voltage SoC and logs "capacity learned" in
  `journalctl -u linht-chargerd`. `cycles.jsonl` has the curve, the energy
  and the run time.
- The same on USB: after `full`, hold `linht-chargerd quiet 0` in a shell.
  The battery drains as on battery until `quiet_end_soc` ends the lease,
  then the charge starts and the discharge (about 80 % deep) is evaluated at
  once.
- Field use: charge, unplug, leave the unit idle for 20 min, use it on
  battery for a few hours (60 % or so), leave it idle for 20 min, plug in
  USB. The charge logs "partial cycle" with the depth, C_est and the
  weight. Switching the unit off instead of plugging in logs it at the next
  start as "partial cycle (segment of the last run)".
- Partial cycles: run the FART auto mode on USB for a day (several 50 to
  70 % cycles). Every return to charging logs "partial cycle" with the
  depth, C_est and the weight. Compare `CapacityPartialWh` with a full
  anchored learning of the same battery. A steady offset means the voltage
  table does not fit the cells in the 50 to 70 % range.
- Counter accuracy: a lab supply in place of the battery with its own current
  reading, 30 min on battery, compare `EnergyRemainingWh` steps with the
  supply.
- Reboot in the middle of a discharge: the SoC must continue from
  `state.json`.
