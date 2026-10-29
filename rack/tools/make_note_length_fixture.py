#!/usr/bin/env python3
"""Generate fixture-length3.txt with the pinned HydraPy (no hardware audio).

Run from the repository root: python rack/tools/make_note_length_fixture.py > rack/tests/fixture-length3.txt
Validates the saved evaluator vectors and compares variable callback sizes, seeks,
clipped loops, explicit/legacy replacement, fractional lengths and dense ply.
"""
import copy
import json
from pathlib import Path
import random

from hydrapy.studio_tracker import Tracker
from hydrapy.tracker_cv import TrackerCV, snapshot
from hydrapy.tracker_v3 import Timeline, anchor, evaluate, trace_line, validate
from make_fixture import SIGNALS_V3, signals


def controlled(score, block, seek=False, delayed=False, gate=.1):
    sr = 48000
    tracker = Tracker(copy.deepcopy(score))
    tracker.enabled = True
    tracker.seek(4800 / 1920 if seek else 0., 0.)
    cv = TrackerCV(dict(enumerate(SIGNALS_V3)), samplerate=sr, gate=gate, open_stream=False)
    blocks = (102000 if not seek else 30000) // block + 1
    rows, heads, last = [], [], None
    published = snapshot(tracker, 0., 0., True)
    published = dict(published, layers=None)
    if delayed:
        published = dict(published, wall=-.1)  # arrive 100 ms after the seek
    for k in range(blocks):
        wall = k * block / sr
        # Interrupt an open gate with a live seek, then delay a second seek.
        if seek and k == max(1, 6000 // block):
            tracker.seek(4800 / 1920, wall)
            published = dict(snapshot(tracker, wall, wall, True), layers=None)
        if k == 0 or (seek and k == max(1, 6000 // block)):
            h = published
            heads.append((k, dict(transport_revision=h['transport_revision'], start=h['start'], elapsed=h['elapsed'],
                enabled=h['enabled'], playing=h['playing'], anchor_time=h['anchor_time'], clock_time=h['clock_time'],
                wall=h['wall'], fill=h['fill'], bus=h['bus'], bpm=score['bpm'])))
        out = cv.render(block, wall, published)
        for i, v in enumerate(out):
            bits = sum(int(v[c] > 0) << c for c in range(9))
            row = ' '.join([str(bits)] + [f'{x / 5:.9g}' for x in v[9:15]] + [f'{x:.9g}' for x in v[15:21]])
            if row != last:
                rows.append(f'{k * block + i} {row}')
                last = row
    print('SIGNALS_VARIABLE', 0, blocks, len(heads), block, gate)
    for k, h in heads:
        print('PLAYHEAD', k, json.dumps(h, separators=(',', ':')))
    print('ROWS', len(rows))
    print('\n'.join(rows))
    print('AUTOS', 0)


def main():
    vectors = json.loads((Path(__file__).parent.parent / 'tests/note-length-vectors.json').read_text())
    cases = []
    for v in vectors:
        assert evaluate(validate(v['score']), v['transport'], v['u0'], v['u1']) == v['candidates'], v['name']
        cases.append((v['score'], v, None))
    base = copy.deepcopy(vectors[0]['score'])
    for lane in base['patterns'][0]['lanes']:
        lane['cells'] = [None] * 16
    for kind in ('single', 'shared-end', 'seek', 'delayed', 'replace', 'wrap', 'dense'):
        d = copy.deepcopy(base)
        cells = d['patterns'][0]['lanes'][0]['cells']
        cells[0] = {'note': 'C-4', 'vel': 100, 'len': 12 if kind in ('seek', 'delayed', 'replace') else 3}
        if kind == 'shared-end':
            cells[3] = {'note': 'D-4', 'vel': 110, 'len': 1}
        if kind == 'replace':
            cells[2] = {'note': 'D-4', 'vel': 64}
        if kind == 'wrap':
            cells[0] = None
            cells[15] = {'note': 'C-4', 'vel': 100, 'len': 16}
        if kind == 'dense':
            p = d['patterns'][0]
            p.update(rate=8, bars=8)
            for lane in p['lanes']:
                lane.update(len=128, cells=[{'note':'C-4','vel':100,'len':16}] * 128,
                            transforms=[{'kind':'ply','n':4}])
        for block in (1, 511, 6000, 18000, 24000):
            cases.append((d, None, (block, kind in ('seek','delayed'), kind == 'delayed')))
    print('GORGON_FIXTURE_3', len(cases))
    for revision, (d, v, opts) in enumerate(cases):
        print('SCORE', json.dumps(d, separators=(',', ':')))
        t, u0, u1 = (v['transport'], v['u0'], v['u1']) if v else ({'tick': 0}, -7680, min(Timeline(d).cycle * 2, 32000))
        if opts and d['patterns'][0]['rate'] == 8:
            u1 = 61  # Dense attachment/capacity has its own C++ test; avoid repeated trace bulk.
        print('SEGMENTS', 1)
        start = anchor(d, t['tick'])
        print('SEGMENT', json.dumps(dict(transport_revision=revision, start=start, u0=u0, u1=u1, **{k:v for k,v in t.items() if k != 'tick'})))
        candidates = evaluate(d, t, u0, u1)
        print('CANDIDATES', len(candidates))
        for c in candidates:
            print(trace_line(c, revision, start))
        if opts:
            controlled(d, *opts)
        else:
            signals(random.Random(20261028 + revision), d)


if __name__ == '__main__':
    main()
