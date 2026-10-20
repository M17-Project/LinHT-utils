#!/usr/bin/env python3
"""Run: python3 -m unittest discover -s scripts/tests -p 'test_bq25792_status.py'."""
import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch

path = Path(__file__).resolve().parents[1] / 'bq25792_status.py'
spec = importlib.util.spec_from_file_location('bq', path)
bq = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bq)


class Bus:
    def __init__(self, part=8):
        self.control = 0x30
        self.part = part
        self.writes = []
        self.fail = None
        self.running = False

    def read(self, reg, count=1):
        if reg == 0x48:
            return bytes([self.part])
        if reg == 0x2e:
            return bytes([self.control & ~0x80])
        if reg == 0x2f:
            return bytes(2)
        if reg == 0x31:
            if self.fail:
                raise self.fail
            values = [424, -123, 4780, 4773, 4779, 8342, 9001, 591, 76, 0, 0]
            return b''.join((v & 65535).to_bytes(2, 'big') for v in values)
        if reg == 0x1b:
            assert count == 7, 'Never read clear-on-read flags'
            return bytes([0x0f, 0xea, 1, 0x20, 0, 0, 0])
        if reg == 0x14:
            return bytes([0x16])
        raise AssertionError(f'Unexpected read {reg:02x}')

    def write(self, reg, val):
        assert reg == 0x2e, 'Must only change ADC control'
        self.writes.append((reg, val))
        self.control = val


class DiagnosticsTest(unittest.TestCase):
    def test_measurement_units_and_signed_current(self):
        bus = Bus()
        result = bq.snapshot(bus)
        self.assertEqual(result['adc']['vbat_V'], 8.342)
        self.assertEqual(result['adc']['ibat_mA'], -123)
        self.assertEqual(result['adc']['die_temperature_C'], 38)
        self.assertEqual(result['charge_state'], 'charge termination done')
        self.assertFalse(result['battery_discharge_sensing_enabled'])
        self.assertEqual(bus.writes, [(0x2e, 0xc0), (0x2e, 0x30)])

    def test_cleanup_on_io_error_and_ctrl_c(self):
        for error in [OSError('lost bus'), KeyboardInterrupt()]:
            bus = Bus()
            bus.fail = error
            with self.assertRaises(type(error)):
                bq.measure(bus)
            self.assertEqual(bus.control, 0x30)

    def test_timeout_restores_control(self):
        bus = Bus()
        original_read = bus.read
        bus.read = lambda reg, count=1: bytes([bus.control]) if reg == 0x2e else original_read(reg, count)
        with patch.object(bq.time, 'monotonic', side_effect=[0, 4]):
            with self.assertRaises(TimeoutError):
                bq.measure(bus)
        self.assertEqual(bus.control, 0x30)

    def test_wrong_chip_no_writes(self):
        bus = Bus(part=0)
        with self.assertRaises(RuntimeError):
            bq.snapshot(bus)
        self.assertFalse(bus.writes)

    def test_read_only(self):
        bus = Bus()
        self.assertNotIn('adc', bq.snapshot(bus, adc=False))
        self.assertFalse(bus.writes)

    def test_active_adc_no_writes(self):
        bus = Bus()
        original_read = bus.read
        bus.read = lambda reg, count=1: bytes([0x80]) if reg == 0x2e else original_read(reg, count)
        with self.assertRaises(RuntimeError):
            bq.measure(bus)
        self.assertFalse(bus.writes)


if __name__ == '__main__':
    unittest.main()
