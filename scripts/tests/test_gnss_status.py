"""Parser regression tests and real pseudo-terminal I/O/cleanup checks."""
import importlib.util
from pathlib import Path
import os
import signal
import subprocess
import sys
import termios
import time
import unittest

path = Path(__file__).resolve().parents[1] / 'gnss_status.py'
spec = importlib.util.spec_from_file_location('gnss', path)
gnss = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gnss)


def nmea(payload):
    data = payload.encode()
    checksum = 0
    for b in data:
        checksum ^= b
    return b'$' + data + ('*%02X\r\n' % checksum).encode()


class ParserTests(unittest.TestCase):
    def test_position_units_and_loss_of_fix(self):
        m = gnss.Monitor()
        m.feed(nmea('GNRMC,123519.000,A,4807.038,N,01131.000,E,10.0,84.4,181026,,,A'), 1)
        m.feed(nmea('GNGGA,123519.000,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,'), 1)
        s = m.snapshot(1)
        self.assertTrue(s['fix_valid'])
        self.assertAlmostEqual(s['latitude_deg'], 48.1173)
        self.assertAlmostEqual(s['longitude_deg'], 11.5166666667)
        self.assertAlmostEqual(s['speed_kmh'], 18.52)
        self.assertEqual(s['altitude_m_msl'], 545.4)
        m.feed(nmea('GNRMC,123520.000,V,,,,,0.0,0.0,181026,,,N'), 2)
        self.assertFalse(m.snapshot(2)['fix_valid'])
        self.assertIsNone(m.snapshot(2)['latitude_deg'])

    def test_no_fix_and_stale_are_not_positions(self):
        m = gnss.Monitor(stale=3)
        m.feed(nmea('GNGGA,000148.096,,,,,0,0,,,M,,M,,'), 1)
        self.assertFalse(m.snapshot(1)['fix_valid'])
        self.assertEqual(m.snapshot(1)['satellites_used'], 0)
        self.assertFalse(m.snapshot(5)['uart_live'])
        self.assertIsNone(m.snapshot(5)['satellites_used'])

    def test_gsv_multipart_signal_id_no_double_count(self):
        m = gnss.Monitor()
        m.feed(nmea('GPGSV,2,1,05,01,60,010,40,02,50,020,35,03,40,030,,04,30,040,0,1'), 1)
        self.assertIsNone(m.snapshot(1)['satellites_in_view'])
        m.feed(nmea('GPGSV,2,2,05,05,20,050,25,1'), 2)
        s = m.snapshot(2)
        self.assertEqual(s['satellites_in_view'], 5)
        self.assertEqual(s['satellites_with_cn0'], 3)
        m.feed(nmea('GPGSV,1,1,01,01,60,010,42,2'), 3)
        self.assertEqual(m.snapshot(3)['satellites_in_view'], 5)
        self.assertEqual(m.snapshot(3)['cn0_max_dbhz'], 42)
        m.feed(nmea('GPGSV,1,1,00,1'), 4)
        self.assertEqual(m.snapshot(4)['satellites_in_view'], 1)
        self.assertIsNone(m.snapshot(30)['satellites_in_view'])

    def test_old_gsv_without_signal_id_and_out_of_order(self):
        m = gnss.Monitor()
        m.feed(nmea('GLGSV,2,2,05,69,10,150,30'), 1)
        self.assertIsNone(m.snapshot(1)['satellites_in_view'])
        m.feed(nmea('GLGSV,1,1,01,65,10,150,30'), 2)
        s = m.snapshot(2)
        self.assertEqual(s['satellites_in_view'], 1)
        self.assertIsNone(s['satellites'][0]['signal'])

    def test_bad_checksum_and_unsupported_antenna_detection(self):
        m = gnss.Monitor()
        m.feed(b'$GPGSV,1,1,00,1*00', 1)
        self.assertEqual(m.bad, 1)
        self.assertEqual(m.valid, 0)
        m.feed(nmea('GPTXT,01,01,02,ANTSTATUS=OPEN'), 2)
        s = m.snapshot(2)
        self.assertIn('OPEN', s['antenna_report'])
        self.assertFalse(s['antenna_detection_supported'])
        self.assertFalse(s['antenna_bias_available'])

    def test_coordinate_south_west_and_invalid_minutes(self):
        self.assertAlmostEqual(gnss.coordinate('4807.038', 'S', True), -48.1173)
        self.assertLess(gnss.coordinate('01131.000', 'W', False), 0)
        self.assertIsNone(gnss.coordinate('4867.038', 'N', True))
        self.assertIsNone(gnss.coordinate('9100.000', 'N', True))

    def test_gsa_dimension_and_dop(self):
        m = gnss.Monitor()
        f = ['A','3'] + ['01','02'] + [''] * 10 + ['1.7','0.9','1.4','1']
        m.feed(nmea('GNGSA,' + ','.join(f)), 1)
        s = m.snapshot(1)
        self.assertEqual(s['fix_dimension'], 3)
        self.assertEqual(s['pdop'], 1.7)
        self.assertEqual(s['hdop'], 0.9)


class SerialTests(unittest.TestCase):
    def test_serial_framing_restore_and_exclusion(self):
        master, slave = os.openpty()
        before = termios.tcgetattr(slave)
        serial = gnss.Serial(os.ttyname(slave), 9600)
        try:
            self.assertEqual(termios.tcgetattr(slave)[4], termios.B9600)
            with self.assertRaises(BlockingIOError):
                gnss.Serial(os.ttyname(slave), 9600)
            sentence = nmea('GPGSV,1,1,00,1')
            os.write(master, sentence)
            time.sleep(0.01)
            self.assertEqual(os.read(serial.fd, 8192), sentence)
        finally:
            serial.close()
            self.assertEqual(termios.tcgetattr(slave), before)
            os.close(master)
            os.close(slave)

    def test_sigterm_cleanup(self):
        master, slave = os.openpty()
        before = termios.tcgetattr(slave)
        p = subprocess.Popen([sys.executable, str(path), '--device', os.ttyname(slave), '--json', '--interval', '0.1'],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            # A snapshot proves initialization and the signal handler completed.
            self.assertIn(b'fix_valid', p.stdout.readline())
            p.send_signal(signal.SIGTERM)
            p.communicate(timeout=3)
            self.assertEqual(p.returncode, 143)
            self.assertEqual(termios.tcgetattr(slave), before)
        finally:
            if p.poll() is None:
                p.kill()
                p.communicate()
            os.close(master)
            os.close(slave)


if __name__ == '__main__':
    unittest.main()
