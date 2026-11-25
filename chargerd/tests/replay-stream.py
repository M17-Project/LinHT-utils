#!/usr/bin/env python3
"""Polls for tests/replay from logged battery data, one line per second:
"now wall vbat ibat valid discharging charging full_event".

  replay-stream.py cycles cycles.jsonl
      Synthetic polls from the cycle log of the daemon: every cycle at its
      mean power with VBAT from its 21 point curve. A charge that ends at
      100 % and a discharge that starts at 100 % have a full anchor.
      Discharges and charges below 10 % are not in the log, so they are
      missing here too.
  replay-stream.py samples samples.jsonl
      The battery part of the FART field logger samples (decompress first),
      each sample held until the next one. A change from "charging" to
      "full" is a full anchor.
  replay-stream.py combined cycles.jsonl samples.jsonl
      The cycles before the first sample, then the samples.

A last poll with charge current ends a learning discharge still in progress.
"""
import json
import sys

T0 = None


def emit(wall, vbat, ibat, dis, chg, full):
    print('%.1f %.1f %.4f %.4f 1 %d %d %d' % (wall - T0, wall, vbat, ibat, dis, chg, full))


def cycles(path, before=None):
    global T0
    cyc = [json.loads(line) for line in open(path)]
    if before is not None:
        cyc = [c for c in cyc if c['end'] < before]
    if T0 is None:
        T0 = cyc[0]['start'] - 10
    v = cyc[0]['vbat_start']
    for c in cyc:
        d = int(c['duration_s'])
        dis = c['kind'] == 'discharge'
        p = c['energy_wh'] / (d / 3600.0)
        curve = c['vbat_curve']
        emit(c['start'] - 1, v, 0, 0, 0, int(dis and c['soc_start'] >= 99.95))
        for s in range(d):
            f = s / d * 20
            k = min(int(f), 19)
            v = curve[k] + (curve[k + 1] - curve[k]) * (f - k)
            emit(c['start'] + s, v, (-p if dis else p) / v, int(dis), int(not dis), 0)
        end = c['start'] + d
        emit(end, v, 0, 0, 0, int(not dis and c['soc_end'] >= 99.9))
        emit(end + 1, v, 0, 0, 0, 0)
    return v, end + 2


def samples(path):
    global T0
    last = None
    prev_state = None
    for line in open(path):
        d = json.loads(line)
        b = d.get('battery') if d.get('type') == 'sample' else None
        if not b or b.get('vbat_v') is None or b.get('ibat_a') is None:
            continue
        t = d['t']
        if T0 is None:
            T0 = t - 10
        st = b.get('state')
        full = int(prev_state == 'charging' and st == 'full')
        prev_state = st
        if last is not None:
            # hold the previous sample until this one, 1 s steps
            lt, lv, li, ldis, lchg = last
            for s in range(1, int(t - lt)):
                emit(lt + s, lv, li, ldis, lchg, 0)
        dis = int(st in ('on-battery', 'quiet'))
        chg = int(st == 'charging')
        emit(t, b['vbat_v'], b['ibat_a'], dis, chg, full)
        last = (t, b['vbat_v'], b['ibat_a'], dis, chg)
    return last[1], last[0] + 1


def first_sample_time(path):
    for line in open(path):
        d = json.loads(line)
        if d.get('type') == 'sample' and d.get('battery'):
            return d['t']
    return None


def main():
    mode = sys.argv[1]
    if mode == 'cycles':
        v, t = cycles(sys.argv[2])
    elif mode == 'samples':
        v, t = samples(sys.argv[2])
    elif mode == 'combined':
        cycles(sys.argv[2], before=first_sample_time(sys.argv[3]))
        v, t = samples(sys.argv[3])
    else:
        sys.exit(__doc__)
    emit(t, v, 0.3, 0, 1, 0)


if __name__ == '__main__':
    main()
