#!/usr/bin/env python3
"""Reference outputs from HydraPy itself, for tests/engine_test.cpp and tests/evaluator_test.cpp.

Random scores are evaluated by hydrapy.tracker_cv.TrackerCV (the code that
drives the ES-3 from Gorgon), one sample at each chosen beat, and written as
plain text the C++ test reads. Regenerate with HydraPy installed:

    python rack/tools/make_fixture.py > rack/tests/fixture.txt
    python rack/tools/make_fixture.py --v3 > rack/tests/fixture3.txt

The version-3 fixture (spec/evaluator.md#fixture) holds random hydrapy-tracker-3
scores as JSON, segments (anchor, transport_revision, FILL and the bus layer)
and each segment's candidates over a stated interval, as hydrapy.tracker_v3
writes them in its trace. Then, per score, the signals TrackerCV renders while
Studio's tracker plays it: the playheads a follower is sent (seeks, one heard
late, FILL, bus windows, a pause, a tempo change), every sample where a gate,
clock, reset, run, velocity or pitch changes, and the automation lanes every
50 ms (spec/cv-semantics.md#fixture).
"""
import copy
import json
import random
import sys

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
        # Either side of the 5 ms phase-based reset after the cycle start.
        window = cv.reset_s * d['bpm'] / 60
        beats += [start + f * window for f in (.25, .5, .99, 1.01, 1.5, 3)]
        print(len(beats))
        for beat in beats:
            moving = rng.random() < .85
            tracker.enabled = True
            # One stateless sample per row: not a seek, no stopped-to-moving
            # transition, nothing held from the previous row. Transport latches
            # and held values have their own tests on both sides.
            tracker.seek(beat, 0., discontinuity=False)
            cv._transport, cv._moving = tracker.transport_revision, moving
            cv._velocity[:] = 0.
            cv._pitch[:] = 0.
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


def random_score_v3(rng):
    """A random version-3 score: chains, rates, short lanes, conditions and transform trees."""
    from hydrapy.tracker_v3 import empty_pattern, upgrade, validate
    d = upgrade(Tracker().document)
    patterns = [None] * 128
    used = rng.sample(range(128), rng.randint(1, 4))

    def node(depth):
        kind = rng.choice(['rot', 'rev', 'degrade', 'ply'] + ['every'] * (depth > 1))
        if kind == 'every':
            b = rng.randint(1, 8)
            return {'kind': 'every', 'a': rng.randint(1, b), 'b': b, 'inner': node(depth - 1)}
        return {'kind': kind, **{'rot': {'k': rng.randint(-127, 127)}, 'rev': {}, 'degrade': {'p': rng.randint(1, 99)},
                                 'ply': {'n': rng.randint(2, 4)}}[kind]}

    def plies(nodes):
        product = 1
        for n in nodes:
            product *= n['n'] if n['kind'] == 'ply' else plies([n['inner']]) if n['kind'] == 'every' else 1
        return product

    for index in used:
        p = empty_pattern(index)
        p['bar'] = rng.choice([3, 4, 7, 8, 12, 16])
        p.update(rate=rng.choice([.25, .5, 1, 2, 4, 8]), bars=rng.randint(1, min(8, 128 // p['bar'])))
        rows = p['bar'] * p['bars']
        for lane in p['lanes']:
            lane['len'] = rng.randint(1, rows)
            lane['cells'] = [None] * lane['len']
            for k in range(lane['len']):
                if rng.random() < .35:
                    cell = {'note': rng.choice(['C0', 'C-4', 'D#4', 'A-5', 'G9', 'AIR']), 'vel': rng.choice([0, 1, 64, 127])}
                    if rng.random() < .15:
                        cell['locks'] = {'p1': rng.randrange(128)}
                    if rng.random() < .4:
                        kind = rng.choice(['every', 'prob', 'fill', 'first'])
                        b = rng.randint(1, 8)
                        cell['cond'] = {'kind': kind, **{'every': {'a': rng.randint(1, b), 'b': b},
                                                         'prob': {'p': rng.randint(1, 99)}}.get(kind, {})}
                        if rng.random() < .3:
                            cell['cond']['not'] = True
                    lane['cells'][k] = cell
            while True:
                lane['transforms'] = [node(3) for _ in range(rng.randint(0, 3))]
                if plies(lane['transforms']) <= 4:
                    break
        patterns[index] = p
    d.update(patterns=patterns, loop=rng.random() < .4, pattern=rng.choice(used), seed=rng.randrange(1 << 32),
             chain=[rng.choice(used + [rng.randrange(128)]) for _ in range(rng.randint(1, 5))],
             mutes=[rng.random() < .15 for _ in range(6)])
    return validate(d)


def automate(rng, score):
    """Automation keys on the score's patterns, for the signal section."""
    for p in score['patterns']:
        if p is None:
            continue
        rows = p['bar'] * p['bars']
        for name, (lo, hi, _) in LANES.items():
            keys = sorted(rng.sample(range(rows), rng.randrange(0, min(rows, 6) + 1)))
            p['keys'][name] = [{'r': r, 'v': round(rng.uniform(lo, hi), 4)} for r in keys]
            p['easing'][name] = rng.choice(['hold', 'linear', 'smooth'])


SR, BLOCK = 48000, 480
SIGNALS_V3 = ('clock', 'reset', 'run', *(f'gate{n}' for n in range(1, 7)), *(f'vel{n}' for n in range(1, 7)),
              *(f'pitch{n}' for n in range(1, 7)), 'z', 'roll', 'feedback')
SENDER = '0123456789abcdef'


def signals(rng, score):
    """Play ``score`` in Studio's tracker and render it with TrackerCV, as a Rack module hears it.

    Studio publishes its playhead every third block (30 ms) and at once after a change; the
    follower joins the first segment late. Two seeks are heard late: they land inside a hit
    that fires and reach the follower after its gate window, so the landing gate decides
    (cv-semantics.md#landing-gate). The snapshots carry FILL and the bus layer as lists,
    without Studio's live layers, as a follower outside Studio has them.
    """
    from hydrapy.studio_tracker import cycle_beats
    from hydrapy.tracker_cv import snapshot
    from hydrapy.tracker_v3 import TICKS, evaluate, timeline, transport
    tracker = Tracker(copy.deepcopy(score))
    tracker.enabled = True
    cv = TrackerCV(dict(enumerate(SIGNALS_V3)), samplerate=SR, open_stream=False)
    blocks, first = rng.randint(240, 320), rng.randint(5, 40)
    kinds = ['fill', 'fill', 'bus', 'bus', 'seek', 'late', 'late', 'tempo', 'pause']
    rng.shuffle(kinds)
    at = sorted(rng.sample(range(first + 6, blocks - 50, 6), len(kinds)))
    events = dict(zip(at, kinds))
    for block, kind in list(events.items()):
        if kind == 'pause':
            resume = block + rng.randint(8, 40)
            while resume in events:
                resume += 1
            events[resume] = 'resume'
    playing, clock, hold, pending = True, 0., 0, False
    playheads, rows, autos, last = [], [], [], None
    for k in range(blocks):
        wall = k * BLOCK / SR
        stamp = max([0] + [t for t in [cv.evaluated(tracker.segment)] if t is not None])
        kind = events.get(k)
        if kind == 'fill':
            tracker.set_fill(not tracker.fill[-1]['on'], stamp)
        elif kind == 'bus':
            lists = [[{'at': stamp, 'on': rng.random() < .4}] for _ in range(7)]
            tracker.apply_window({'transport_revision': tracker.transport_revision, 'start': dict(tracker.start)},
                                 SENDER, {'from': stamp, 'fill': lists[0], 'mutes': lists[1:]}, wall, stamp)
        elif kind == 'seek':
            tracker.seek(rng.uniform(0, cycle_beats(tracker.document)), clock)
        elif kind == 'late':
            # Into the first ~40% of a hit that fires, heard 50–95% of the way through it.
            d = tracker.document
            hits = [c for c in evaluate(d, {'tick': 0}, 0, timeline(d).cycle) if c['fires'] and c['duration'] >= 240]
            hit = rng.choice(hits) if hits else {'at': rng.randrange(timeline(d).cycle), 'duration': 480}
            tracker.seek((hit['at'] + int(rng.uniform(0, .4) * hit['duration'])) / TICKS, clock)
            landing = [c for c in evaluate(d, transport(snapshot(tracker, clock, wall, playing)), -TICKS, 1)
                       if c['fires'] and c['at'] <= 0 < c['at'] + c['duration']]
            heard = (rng.uniform(.5, .95) * landing[0]['duration'] + landing[0]['at']) if landing else rng.uniform(0, 300)
            hold = max(1, round(heard / (BLOCK / SR * d['bpm'] / 60 * TICKS)))
        elif kind == 'tempo':
            document = copy.deepcopy(tracker.document)
            document['bpm'] = rng.choice([60, 90, 120, 138, 174, 200])
            tracker.replace(document, clock, tracker.revision)
        elif kind == 'pause':
            playing = False
        elif kind == 'resume':
            tracker.play(clock)
            playing = True
        pending = pending or kind is not None
        if k >= first and hold == 0 and ((k - first) % 3 == 0 or pending):
            pending = False
            published = snapshot(tracker, clock, wall, playing)
            published = dict(published, layers=None, fill=copy.deepcopy(published['fill']), bus=copy.deepcopy(published['bus']))
            playheads.append((k, published))
        hold = max(0, hold - 1)
        if k >= first:
            out = cv.render(BLOCK, wall, playheads[-1][1])
            for i, v in enumerate(out):
                bits = sum(int(v[c] > 0) << c for c in range(9))
                row = ' '.join([str(bits)] + [f'{x / 5:.9g}' for x in v[9:15]] + [f'{x:.9g}' for x in v[15:21]])
                if row != last:
                    rows.append(f'{k * BLOCK + i} {row}')
                    last = row
            if k * BLOCK % 2400 == 0:
                lanes = []
                for c, name in enumerate(NAMES):
                    lo, hi = LANES[name][:2]
                    vlo, vhi = LANE_VOLTS[name]
                    lanes.append(lo + (out[0][21 + c] - vlo) / (vhi - vlo) * (hi - lo))
                autos.append(f'{k * BLOCK} ' + ' '.join(f'{x:.9g}' for x in lanes))
        if playing:
            clock += BLOCK / SR
    print('SIGNALS', first, blocks, len(playheads))
    for k, h in playheads:
        print('PLAYHEAD', k, json.dumps(dict(
            transport_revision=h['transport_revision'], start=h['start'], elapsed=h['elapsed'], enabled=h['enabled'],
            playing=h['playing'], anchor_time=h['anchor_time'], clock_time=h['clock_time'], wall=h['wall'],
            fill=h['fill'], bus=h['bus'], bpm=h['document']['bpm']), separators=(',', ':')))
    print('ROWS', len(rows))
    print('\n'.join(rows))
    print('AUTOS', len(autos))
    print('\n'.join(autos))


def transitions(rng, span):
    changes = sorted((rng.randrange(span), rng.random() < .5) for _ in range(rng.randint(0, 4)))
    return [{'at': 0, 'on': rng.random() < .3}] + [{'at': at, 'on': on} for at, on in changes]


def main_v3():
    from hydrapy.tracker_v3 import TICKS, Timeline, anchor, evaluate, trace_line
    rng = random.Random(20261027)
    scores = [random_score_v3(rng) for _ in range(8)]
    # The signal section has its own generator, so the candidate section is as before.
    play = random.Random(20261028)
    for score in scores:
        automate(play, score)
    print('GORGON_FIXTURE_3', len(scores))
    for revision, score in enumerate(scores):
        print('SCORE', json.dumps(score, separators=(',', ':')))
        cycle = Timeline(score).cycle
        segments = []
        for k in range(3):
            tick = rng.randrange(cycle)
            transport = {'tick': tick, 'fill': transitions(rng, 2 * cycle)}
            if k:
                transport['bus'] = {'fill': transitions(rng, 2 * cycle),
                                    'mutes': [transitions(rng, 2 * cycle) for _ in range(6)]}
            u0 = -TICKS if k != 1 else rng.randrange(cycle)
            u1 = u0 + min(rng.randrange(cycle, 2 * cycle), 16 * TICKS)
            segments.append((tick, transport, u0, u1))
        print('SEGMENTS', len(segments))
        for tick, transport, u0, u1 in segments:
            start = anchor(score, tick)
            candidates = evaluate(score, transport, u0, u1)
            print('SEGMENT', json.dumps(dict(transport_revision=revision, start=start, fill=transport['fill'],
                                             bus=transport.get('bus'), u0=u0, u1=u1), separators=(',', ':')))
            print('CANDIDATES', len(candidates))
            for c in candidates:
                print(trace_line(c, revision, start))
        signals(play, score)


if __name__ == '__main__':
    main_v3() if sys.argv[1:] == ['--v3'] else main()
