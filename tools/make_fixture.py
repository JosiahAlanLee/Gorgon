#!/usr/bin/env python3
"""Reference outputs from HydraPy itself, for tests/engine_test.cpp.

Random scores are evaluated by hydrapy.tracker_cv.TrackerCV (the code that
drives the ES-3 from Gorgon), one sample at each chosen beat, and written as
plain text the C++ test reads. Regenerate with HydraPy installed:

    python vcv/gorgon/tools/make_fixture.py > vcv/gorgon/tests/fixture.txt
"""
import random

import numpy as np

from hydrapy.studio_tracker import LANES, Tracker, validate, note_midi
from hydrapy.tracker_cv import LANE_VOLTS, TrackerCV

SIGNALS = {0: 'clock', 1: 'reset', 2: 'run', 3: 'gate1', 4: 'gate2', 5: 'gate3', 6: 'gate4',
           7: 'vel1', 8: 'vel2', 9: 'vel3', 10: 'vel4', 11: 'z', 12: 'roll', 13: 'feedback',
           14: 'pitch1', 15: 'pitch2', 16: 'pitch3', 17: 'pitch4'}
NAMES = list(LANES)


def random_score(rng):
    d = Tracker().document
    d['bpm'] = rng.choice([60, 90, 120, 138, 174, 200])
    d['loop'] = rng.random() < .4
    d['pattern'] = rng.randrange(4)
    d['key'] = rng.choice([0, 2, 5, 7])
    d['mutes'] = [rng.random() < .2 for _ in range(4)]
    for p in d['patterns']:
        p['audio'] = [[{'note': rng.choice(['C0', 'C-4', 'D#4', 'A-5', 'G9', 'AIR']),
                       'vel': rng.choice([0, rng.randrange(1, 128)])} if rng.random() < .3 else None for _ in range(64)]
                      for _ in range(4)]
        for name, (lo, hi, _) in LANES.items():
            rows = sorted(rng.sample(range(64), rng.randrange(0, 7)))
            p['keys'][name] = [{'r': r, 'v': round(rng.uniform(lo, hi), 4)} for r in rows]
            p['easing'][name] = rng.choice(['hold', 'linear', 'smooth'])
    return validate(d)


def main():
    rng = random.Random(20261024)
    cv = TrackerCV(SIGNALS, samplerate=48000, open_stream=False)
    scores = [random_score(rng) for _ in range(8)]
    print('GORGON_FIXTURE_2', len(scores))
    for d in scores:
        print(d['bpm'], d['key'], int(d['loop']), d['pattern'], *[int(m) for m in d['mutes']])
        for p in d['patterns']:
            hits = [(lane, r, cell['vel'], -1 if note_midi(cell['note']) is None else note_midi(cell['note']))
                    for lane, cells in enumerate(p['audio']) for r, cell in enumerate(cells) if cell]
            print(len(hits), *[x for h in hits for x in h])
            for name in NAMES:
                keys = p['keys'][name]
                print(['hold', 'linear', 'smooth'].index(p['easing'][name]), len(keys),
                      *[x for k in keys for x in (k['r'], repr(k['v']))])
        tracker = Tracker(d)
        start, length = (d['pattern'] * 16, 16) if d['loop'] else (0, 64)
        beats = [rng.uniform(start, start + length) for _ in range(300)]
        # Exact row and pulse boundaries are where off-by-one errors live.
        beats += [start + k / 8 for k in range(0, int(length * 8))]
        print(len(beats))
        for beat in beats:
            moving = rng.random() < .85
            tracker.enabled = True
            tracker.seek(beat, 0.)
            cv._velocity[:] = 0.
            cv.publish(tracker, 0., 0., moving)
            v = cv.render(1, 0.)[0]
            gates = [int(v[c] > 0) for c in range(3, 7)]
            vel = [v[c] / 5 for c in range(7, 11)]           # 0..1 of full velocity, 0 if no hit on this row
            lanes = []
            for k, name in enumerate(NAMES):
                lo, hi = LANES[name][:2]
                vlo, vhi = LANE_VOLTS[name]
                lanes.append(lo + (v[11 + k] - vlo) / (vhi - vlo) * (hi - lo))
            print(repr(float(tracker.beat(0.))), int(moving), tracker.transport_revision,
                  int(v[0] > 0), int(v[1] > 0), int(v[2] > 0), *gates,
                  *[f'{x:.9g}' for x in vel], *[f'{x:.9g}' for x in lanes], *[f'{x:.9g}' for x in v[14:18]])


if __name__ == '__main__':
    main()
