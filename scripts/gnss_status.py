#!/usr/bin/env python3
"""Live LG77L GNSS monitor for LinHT Rev C, Python 3 standard library only.

Examples (on LinHT):
  /usr/local/bin/gnss-status
  /usr/local/bin/gnss-status --plain --interval 5
  /usr/local/bin/gnss-status --json --duration 30
  /usr/local/bin/gnss-status --raw

Passively reads /dev/ttyLP2 at 9600 8N1; sends no commands, changes no receiver
configuration. Restores terminal settings on exit and rejects a second monitor.
Stop gpsd/other UART readers first; advisory flock cannot exclude other software.
Ctrl-C exits. --once collects at least 3 seconds, then prints one snapshot.

C/N0 (NMEA GSV) is per-satellite carrier/noise density in dB-Hz, not RSSI in dBm.
DOP is dimensionless geometry, not a measured position error in metres.
Rev C has no active-antenna DC bias or antenna status detection circuit:
ANTSTATUS is reported verbatim but cannot diagnose the attached antenna.
An active antenna needs a suitable external bias tee with DC isolation toward
RF_IN; verify the antenna's voltage specification before supplying power.

Protocol: Quectel LG77L(C)&Lx0&Lx6&LC86L GNSS Protocol Specification V2.2,
NMEA RMC, GGA, GSA, GSV and TXT. No dependency on pyserial/gpsd.
"""
import argparse
import datetime
import fcntl
import json
import math
import os
import select
import signal
import sys
import termios
import time


def number(value, integer=False):
    try:
        result = int(value) if integer else float(value)
        return result if math.isfinite(result) else None
    except (ValueError, TypeError):
        return None


def coordinate(value, hemisphere, latitude):
    raw = number(value)
    if raw is None or hemisphere not in (('N', 'S') if latitude else ('E', 'W')):
        return None
    degrees = int(raw // 100)
    minutes = raw - degrees * 100
    limit = 90 if latitude else 180
    if raw < 0 or not 0 <= minutes < 60 or degrees + minutes / 60 > limit:
        return None
    return (degrees + minutes / 60) * (-1 if hemisphere in ('S', 'W') else 1)


class Monitor:
    def __init__(self, stale=15):
        self.stale = stale
        self.latest = {}
        self.pending = {}
        self.cycles = {}
        self.used = {}
        self.valid = self.bad = self.malformed = 0
        self.last_received = None

    def fresh(self, timestamp, now):
        return now - timestamp <= self.stale

    def feed(self, line, now=None):
        now = time.monotonic() if now is None else now
        if not line.startswith(b'$'):
            return
        try:
            payload, checksum = line.strip()[1:].rsplit(b'*', 1)
            calculated = 0
            for byte in payload:
                calculated ^= byte
            if len(checksum) != 2 or calculated != int(checksum, 16):
                raise ValueError('checksum')
            fields = payload.decode('ascii').split(',')
        except (ValueError, UnicodeError):
            self.bad += 1
            return
        self.valid += 1
        self.last_received = now
        tag, f = fields[0], fields[1:]
        kind, talker = tag[-3:], tag[:2]
        minimum = {'RMC': 9, 'GGA': 10, 'GSA': 17, 'GSV': 3, 'TXT': 4}
        if kind not in minimum:
            return
        if len(f) < minimum[kind]:
            self.malformed += 1
            return
        if kind in ('RMC', 'GGA'):
            self.latest[kind] = (now, f)
        elif kind == 'GSA':
            system = f[17] if len(f) > 17 else talker
            self.used[(talker, system)] = (now, f)
        elif kind == 'TXT':
            text = ','.join(f[3:])
            if 'ANTSTATUS=' in text:
                self.latest['antenna'] = (now, text)
            self.latest['message'] = (now, text)
        elif kind == 'GSV':
            total, index, count = [number(x, integer=True) for x in f[:3]]
            if total is None or index is None or count is None or not 1 <= index <= total <= 9 or count < 0:
                self.malformed += 1
                return
            body = f[3:]
            # NMEA 4.1 adds one signal-ID field after complete groups of four.
            if len(body) % 4 == 1:
                signal_id, body = body[-1], body[:-1]
            elif len(body) % 4 == 0:
                signal_id = ''
            else:
                self.malformed += 1
                return
            key = (talker, signal_id)
            if index == 1:
                self.pending[key] = {'time': now, 'next': 1, 'total': total,
                                     'count': count, 'satellites': []}
            cycle = self.pending.get(key)
            if (not cycle or cycle['next'] != index or cycle['total'] != total
                    or cycle['count'] != count or not self.fresh(cycle['time'], now)):
                self.pending.pop(key, None)
                return
            for i in range(0, len(body), 4):
                sat, elevation, azimuth, cn0 = body[i:i + 4]
                if sat:
                    cycle['satellites'].append({
                        'talker': talker, 'id': sat, 'signal': signal_id or None,
                        'elevation_deg': number(elevation), 'azimuth_deg': number(azimuth),
                        'cn0_dbhz': number(cn0)})
            cycle['next'] += 1
            if index == total:
                self.cycles[key] = (now, cycle['count'], cycle['satellites'])
                del self.pending[key]
        # Bound history even on a long-running malformed or changing stream.
        self.pending = {k: v for k, v in self.pending.items() if self.fresh(v['time'], now)}
        self.cycles = {k: v for k, v in self.cycles.items() if self.fresh(v[0], now)}
        self.used = {k: v for k, v in self.used.items() if self.fresh(v[0], now)}

    def snapshot(self, now=None):
        now = time.monotonic() if now is None else now
        def get(key):
            value = self.latest.get(key)
            return value[1] if value and self.fresh(value[0], now) else None
        gga, rmc = get('GGA'), get('RMC')
        age = None if self.last_received is None else now - self.last_received
        live = age is not None and age <= self.stale
        quality = number(gga[5], integer=True) if gga else None
        rmc_valid = bool(rmc and rmc[1] == 'A')
        # If either available position report says no fix, suppress coordinates.
        fixed = live and bool((quality is not None and quality > 0) or rmc_valid)
        if (gga and not quality) or (rmc and not rmc_valid):
            fixed = False
        fresh_gsa = [f for timestamp, f in self.used.values() if self.fresh(timestamp, now)]
        dims = [number(f[1], integer=True) for f in fresh_gsa]
        dimension = max((x for x in dims if x is not None), default=None)
        lat = lon = None
        if fixed:
            if gga:
                lat, lon = coordinate(gga[1], gga[2], True), coordinate(gga[3], gga[4], False)
            elif rmc:
                lat, lon = coordinate(rmc[2], rmc[3], True), coordinate(rmc[4], rmc[5], False)
        utc = None
        if rmc:
            # Raw receiver date/time is deliberately retained, even before fix.
            utc = f'{rmc[8]} {rmc[0]} UTC (ddmmyy hhmmss.sss)'
        satellites, groups = [], []
        for (talker, sig), (timestamp, count, sats) in sorted(self.cycles.items()):
            if self.fresh(timestamp, now):
                satellites.extend(sats)
                groups.append({'talker': talker, 'signal': sig or None, 'reported': count})
        unique = {(s['talker'], s['id']) for s in satellites}
        tracked = {(s['talker'], s['id']) for s in satellites
                   if s['cn0_dbhz'] is not None and s['cn0_dbhz'] > 0}
        cn0 = [s['cn0_dbhz'] for s in satellites if s['cn0_dbhz'] is not None and s['cn0_dbhz'] > 0]
        dop = next((f for f in reversed(fresh_gsa) if number(f[14]) is not None), None)
        return {
            'time': datetime.datetime.now().astimezone().isoformat(timespec='seconds'),
            'uart_live': live, 'last_nmea_age_s': round(age, 1) if age is not None else None,
            'fix_valid': fixed, 'fix_dimension': dimension, 'gga_quality': quality,
            'latitude_deg': lat, 'longitude_deg': lon,
            'altitude_m_msl': number(gga[8]) if fixed and gga else None,
            'speed_kmh': number(rmc[6]) * 1.852 if fixed and rmc and number(rmc[6]) is not None else None,
            'course_deg': number(rmc[7]) if fixed and rmc else None,
            'receiver_utc_raw': utc, 'receiver_time_valid': rmc_valid and live,
            'satellites_used': number(gga[6], integer=True) if gga else None,
            'satellites_in_view': len(unique) if groups else None,
            'satellites_with_cn0': len(tracked) if groups else None,
            'cn0_max_dbhz': max(cn0) if cn0 else None,
            'hdop': number(gga[7]) if gga else (number(dop[15]) if dop else None),
            'pdop': number(dop[14]) if dop else None,
            'vdop': number(dop[16]) if dop else None,
            'antenna_report': get('antenna'), 'antenna_detection_supported': False,
            'antenna_bias_available': False, 'last_message': get('message'),
            'gsv_groups': groups, 'satellites': satellites,
            'nmea_valid': self.valid, 'nmea_bad_checksum': self.bad,
            'nmea_malformed': self.malformed,
        }


def value(v, places=1):
    return '--' if v is None else (f'{v:.{places}f}' if isinstance(v, float) else str(v))


def display(s, limit=20):
    fix = ('3D' if s['fix_dimension'] == 3 else '2D' if s['fix_dimension'] == 2 else 'valid') if s['fix_valid'] else 'NO FIX'
    lines = [f"LinHT LG77L GNSS | {s['time']}",
             f"UART: {'receiving' if s['uart_live'] else 'WAITING / STALE'}  last valid NMEA: {value(s['last_nmea_age_s'])} s ago",
             f"Fix: {fix}  GGA quality: {value(s['gga_quality'])}",
             f"Position: {value(s['latitude_deg'], 7)}, {value(s['longitude_deg'], 7)} deg  altitude MSL: {value(s['altitude_m_msl'])} m",
             f"Speed: {value(s['speed_kmh'])} km/h  course: {value(s['course_deg'])} deg",
             f"Receiver UTC: {s['receiver_utc_raw'] or '--'}{' [UNCONFIRMED]' if not s['receiver_time_valid'] else ''}",
             f"Satellites: used {value(s['satellites_used'])}, in view {value(s['satellites_in_view'])}, with C/N0 {value(s['satellites_with_cn0'])}",
             f"DOP (geometry, not metres): H {value(s['hdop'],2)}  P {value(s['pdop'],2)}  V {value(s['vdop'],2)}",
             f"Antenna message: {s['antenna_report'] or '--'} [detection NOT wired on Rev C]",
             'Rev C: NO DC antenna bias. An active antenna needs external power via a suitable bias tee.',
             f"NMEA: valid {s['nmea_valid']}, bad checksum {s['nmea_bad_checksum']}, malformed {s['nmea_malformed']}",
             'Signal: per-satellite C/N0 in dB-Hz (not RSSI/dBm). # = about 2 dB-Hz.',
             ' SAT     SIG  ELEV   AZIM   C/N0   SIGNAL']
    sats = sorted(s['satellites'], key=lambda x: (-(x['cn0_dbhz'] or 0), x['talker'], x['id']))
    for sat in sats[:limit]:
        cn0 = sat['cn0_dbhz']
        bar = '#' * min(30, max(0, int((cn0 or 0) / 2)))
        lines.append(f" {sat['talker']+sat['id']:7} {str(sat['signal'] or '-'):>3} {value(sat['elevation_deg'],0):>5} {value(sat['azimuth_deg'],0):>6} {value(cn0):>6}   {bar}")
    if not sats:
        lines.append(' No satellite records received yet.' if not s['gsv_groups'] else ' Receiver currently reports zero satellites.')
    if len(sats) > limit:
        lines.append(f' ... {len(sats)-limit} more signal records; use --max-satellites or --json')
    return '\n'.join(lines)


class Serial:
    def __init__(self, path, baud):
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK | os.O_CLOEXEC)
        self.saved = None
        try:
            fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.saved = termios.tcgetattr(self.fd)
            a = termios.tcgetattr(self.fd)
            a[0] = a[1] = a[3] = 0
            a[2] = termios.CS8 | termios.CLOCAL | termios.CREAD
            a[4] = a[5] = getattr(termios, f'B{baud}')
            a[6][termios.VMIN] = a[6][termios.VTIME] = 0
            termios.tcsetattr(self.fd, termios.TCSANOW, a)
        except BaseException:
            self.close()
            raise

    def close(self):
        try:
            if self.saved is not None:
                termios.tcsetattr(self.fd, termios.TCSANOW, self.saved)
        finally:
            os.close(self.fd)


def positive(text):
    n = float(text)
    if not math.isfinite(n) or n <= 0:
        raise argparse.ArgumentTypeError('must be a finite positive number')
    return n


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--device', default='/dev/ttyLP2')
    p.add_argument('--baud', type=int, choices=[4800,9600,19200,38400,57600,115200], default=9600)
    p.add_argument('--interval', type=positive, default=1.0)
    p.add_argument('--stale-after', type=positive, default=15.0)
    p.add_argument('--duration', type=positive)
    p.add_argument('--once', action='store_true')
    output = p.add_mutually_exclusive_group()
    output.add_argument('--json', action='store_true', help='NDJSON snapshots')
    output.add_argument('--raw', action='store_true', help='raw NMEA lines')
    output.add_argument('--plain', action='store_true', help='append snapshots instead of redrawing')
    p.add_argument('--max-satellites', type=int, default=20)
    args = p.parse_args()
    if args.max_satellites < 1:
        p.error('--max-satellites must be positive')
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
    redraw = sys.stdout.isatty() and not (args.json or args.raw or args.plain)
    try:
        serial = Serial(args.device, args.baud)
        try:
            monitor = Monitor(args.stale_after)
            start = time.monotonic()
            finish = start + args.duration if args.duration else None
            next_print = start + (max(3, args.interval) if args.once else args.interval)
            buffer = b''
            while True:
                now = time.monotonic()
                wait = max(0, min(0.25, next_print - now))
                if select.select([serial.fd], [], [], wait)[0]:
                    chunk = os.read(serial.fd, 8192)
                    if not chunk:
                        raise OSError('UART disconnected')
                    buffer += chunk
                    while b'\n' in buffer:
                        line, buffer = buffer.split(b'\n', 1)
                        # Resynchronize after starting mid-sentence or line noise.
                        if b'$' in line:
                            line = line[line.rfind(b'$'):]
                        monitor.feed(line)
                        if args.raw:
                            print(line.decode('ascii', errors='replace').rstrip(), flush=True)
                    if len(buffer) > 8192:
                        buffer = b''
                now = time.monotonic()
                expired = finish is not None and now >= finish
                if now >= next_print or expired:
                    if not args.raw:
                        sample = monitor.snapshot(now)
                        text = json.dumps(sample) if args.json else display(sample, args.max_satellites)
                        print(('\x1b[H\x1b[2J' if redraw else '') + text, flush=True)
                        if not redraw and not args.json:
                            print(flush=True)
                    if args.once or expired:
                        break
                    next_print = now + args.interval
        finally:
            serial.close()
    except KeyboardInterrupt:
        return 130
    except (OSError, termios.error) as exc:
        print(f'Error: {exc}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
