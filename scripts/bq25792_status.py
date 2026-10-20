#!/usr/bin/env python3
"""LinHT Rev C BQ25792 diagnostics; Python 3 standard library only.

Run as root: python3 bq25792_status.py [--watch 2] [--json]
Defaults: /dev/i2c-2, address 0x6b. --no-adc reads status without ANY writes.

Normal mode temporarily changes only REG2E to request a 15-bit one-shot ADC
conversion, then restores it (also on Ctrl-C/errors). Charging limits are never
changed. Disabled ADC channels and an already running ADC are left untouched:
stop its owner or use --no-adc. Do not run alongside another charger controller.
The advisory bus lock coordinates copies of this tool, not kernel drivers or
other I2C utilities. SIGKILL/power loss cannot execute cleanup.

REG22..27 clear-on-read interrupt flags are deliberately not read. IBAT discharge
measurement requires EN_IBAT; this tool reports its state without enabling it.
Positive IBAT means charging; negative means discharging when EN_IBAT is enabled.
VBAT is terminal voltage, not battery capacity or proof of a connected battery.
TDIE is charger die temperature, not battery temperature.
Register reference: TI BQ25792 datasheet SLUSDG1C:
https://www.ti.com/lit/ds/symlink/bq25792.pdf
"""
import argparse
import ctypes
import datetime
import fcntl
import json
import os
import signal
import sys
import time


class Message(ctypes.Structure):
    _fields_ = [("addr", ctypes.c_uint16), ("flags", ctypes.c_uint16),
                ("length", ctypes.c_uint16), ("buffer", ctypes.POINTER(ctypes.c_uint8))]


class Transfer(ctypes.Structure):
    _fields_ = [("messages", ctypes.POINTER(Message)), ("count", ctypes.c_uint32)]


class I2C:
    def __init__(self, bus, address):
        self.fd = os.open(f"/dev/i2c-{bus}", os.O_RDWR | os.O_CLOEXEC)
        self.address = address
        try:
            fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            # I2C_SLAVE checks for a bound kernel driver. Never use FORCE.
            fcntl.ioctl(self.fd, 0x0703, address)
        except BaseException:
            self.close()
            raise

    def close(self):
        os.close(self.fd)

    def transfer(self, register, count=None, value=None):
        outgoing = (ctypes.c_uint8 * (1 if count is not None else 2))()
        outgoing[0] = register
        if value is not None:
            outgoing[1] = value
        messages = [Message(self.address, 0, len(outgoing), outgoing)]
        if count is not None:
            incoming = (ctypes.c_uint8 * count)()
            messages.append(Message(self.address, 1, count, incoming))
        array = (Message * len(messages))(*messages)
        request = Transfer(array, len(messages))
        result = fcntl.ioctl(self.fd, 0x0707, request)  # I2C_RDWR, repeated start
        if result != len(messages):
            raise OSError(f"Incomplete I2C transfer: {result}/{len(messages)} messages")
        return bytes(incoming) if count is not None else None

    def read(self, register, count=1):
        return self.transfer(register, count=count)

    def write(self, register, value):
        self.transfer(register, value=value)


CHARGE = ["not charging", "trickle", "precharge", "fast charge (CC)",
          "taper charge (CV)", "reserved", "top-off", "charge termination done"]
SOURCE = ["no input / OTG / battery temperature condition", "USB SDP (500 mA class)",
          "USB CDP (1.5 A class)", "USB DCP (3.25 A class)", "HVDCP (1.5 A class)",
          "unknown adapter (3 A class)", "non-standard adapter", "OTG output",
          "input not qualified", "reserved", "reserved", "direct VBUS power",
          "reserved", "reserved", "reserved", "reserved"]
FAULT0 = ["VAC1 overvoltage", "VAC2 overvoltage", "converter overcurrent",
          "battery overcurrent", "input overcurrent", "battery overvoltage",
          "VBUS overvoltage", "battery discharge current regulation"]


def word(data, offset, signed=False):
    return int.from_bytes(data[offset:offset + 2], "big", signed=signed)


def measure(bus):
    original = bus.read(0x2e)[0]
    if original & 0x80:
        raise RuntimeError("ADC already active; stop its owner or use --no-adc")
    if any(bus.read(0x2f, 2)):
        raise RuntimeError("Some ADC channels are disabled; use --no-adc (configuration preserved)")
    try:
        bus.write(0x2e, (original & 0x03) | 0xc0)
        deadline = time.monotonic() + 3.0
        while bus.read(0x2e)[0] & 0x80:
            if time.monotonic() >= deadline:
                raise TimeoutError("BQ25792 ADC conversion timed out")
            time.sleep(0.05)
        return bus.read(0x31, 22)
    finally:
        bus.write(0x2e, original)


def snapshot(bus, adc=True):
    part = bus.read(0x48)[0]
    if (part >> 3) & 7 != 1:
        raise RuntimeError(f"Expected BQ25792 (PN=1), got REG48=0x{part:02x}; no writes performed")
    raw_adc = measure(bus) if adc else None
    status = bus.read(0x1b, 7)  # Never extend into clear-on-read REG22!
    control14 = bus.read(0x14)[0]
    faults = [name for bit, name in enumerate(FAULT0) if status[5] & (1 << bit)]
    # Keep less common fault bits raw, rather than silently ignore unknown bits.
    if status[6]:
        faults.append(f"FAULT_STATUS_1=0x{status[6]:02x} (see datasheet REG21)")
    result = {
        "time": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
        "chip": "BQ25792", "revision": part & 7,
        "charge_state": CHARGE[status[1] >> 5],
        "input_classification": SOURCE[(status[1] >> 1) & 15],
        "input_power_good": bool(status[0] & 8),
        "input_current_limited": bool(status[0] & 0x80),
        "input_voltage_limited": bool(status[0] & 0x40),
        "watchdog_expired": bool(status[0] & 0x20),
        "thermal_regulation": bool(status[2] & 4),
        "battery_discharge_sensing_enabled": bool(control14 & 0x20),
        "battery_temperature_status": [name for bit, name in enumerate(
            ["hot", "warm", "cool", "cold"]) if status[4] & (1 << bit)],
        "faults": faults,
        "status_registers": {f"0x{0x1b + i:02x}": f"0x{v:02x}" for i, v in enumerate(status)},
    }
    if raw_adc is not None:
        result["adc"] = {
            "vbus_V": word(raw_adc, 4) / 1000,
            "vac1_V": word(raw_adc, 6) / 1000,
            "vac2_V": word(raw_adc, 8) / 1000,
            "vbat_V": word(raw_adc, 10) / 1000,
            "vsys_V": word(raw_adc, 12) / 1000,
            "ibus_mA": word(raw_adc, 0, signed=True),
            "ibat_mA": word(raw_adc, 2, signed=True),
            "die_temperature_C": word(raw_adc, 16, signed=True) / 2,
        }
    return result


def display(s):
    lines = [f"{s['time']}  {s['chip']} rev {s['revision']}",
             f"Charge: {s['charge_state']}",
             f"Input: {s['input_classification']} (chip classification, not a USB power contract)",
             f"Input power good: {'yes' if s['input_power_good'] else 'no'}"]
    a = s.get("adc")
    if a:
        lines.extend([
            f"VBAT {a['vbat_V']:.3f} V   VSYS {a['vsys_V']:.3f} V   VBUS {a['vbus_V']:.3f} V",
            f"VAC1 {a['vac1_V']:.3f} V   VAC2 {a['vac2_V']:.3f} V",
            f"IBUS {a['ibus_mA']:+d} mA   IBAT {a['ibat_mA']:+d} mA (+ charging, - discharging)",
            f"Charger die: {a['die_temperature_C']:.1f} C",
        ])
    if not s["battery_discharge_sensing_enabled"]:
        lines.append("IBAT discharge sensing OFF: zero IBAT does NOT mean zero battery drain.")
    active = [name for key, name in [
        ("input_current_limited", "input current limit"),
        ("input_voltage_limited", "input voltage limit"),
        ("watchdog_expired", "watchdog expired"),
        ("thermal_regulation", "thermal regulation")] if s[key]]
    lines.extend(["Conditions: " + (", ".join(active) or "none"),
                  "Battery TS: " + (", ".join(s['battery_temperature_status']) or "no temperature flags"),
                  "Faults: " + (", ".join(s["faults"]) or "none"),
                  "Status: " + " ".join(f"{k}={v}" for k, v in s["status_registers"].items())])
    return "\n".join(lines)


def interval(value):
    value = float(value)
    if not 0.5 <= value <= 86400:
        raise argparse.ArgumentTypeError("watch interval must be 0.5..86400 seconds")
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--bus", type=int, default=2)
    parser.add_argument("--address", type=lambda v: int(v, 0), default=0x6b)
    parser.add_argument("--watch", nargs="?", const=2.0, type=interval, metavar="SECONDS")
    parser.add_argument("--json", action="store_true", help="one JSON object per sample (NDJSON)")
    parser.add_argument("--no-adc", action="store_true", help="read status only; no register writes")
    args = parser.parse_args()
    if args.bus < 0 or not 0x08 <= args.address <= 0x77:
        parser.error("invalid bus or 7-bit I2C address")
    # Raise through finally blocks on SIGTERM, just as Ctrl-C does.
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
    try:
        bus = I2C(args.bus, args.address)
        try:
            while True:
                sample = snapshot(bus, adc=not args.no_adc)
                print(json.dumps(sample) if args.json else display(sample), flush=True)
                if args.watch is None:
                    break
                if not args.json:
                    print(flush=True)
                time.sleep(args.watch)
        finally:
            bus.close()
    except KeyboardInterrupt:
        return 130
    except (OSError, RuntimeError) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
