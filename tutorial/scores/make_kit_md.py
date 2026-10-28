#!/usr/bin/env python3
"""Write scores/kit-md.json: kit.json as a hydrapy-tracker-3 score for Machinemodule (chapter 10).

Hits, velocities, picture automation and tempo are kit.json's. Track sounds (OS 1.63 machine IDs):
  1 kick  TRXBD (16)      locks: ghost kicks (step 10) shorter and lower; SUSPEND's downbeats long
  2 snare TRXSD (17)      locks: ghost snares (step 15) higher and short; the phrase's last snare tuned up
  3 hat   TRXCH (22)      locks: the accented off-beat (step 2 of each beat) becomes TRXOH (23), a machine lock;
                          ASCENT's sixteenths open their decay through the bar
  4 FM    EFMXT (34)      P1 (PTCH) locks follow kit.json's notes; ASCENT plays the figure on P-IMT (66), a
                          machine lock
Unlocked hits start from the track defaults, so every lock lasts exactly one hit.

    python scores/make_kit_md.py      # rewrites scores/kit-md.json from scores/kit.json
"""
import copy
import json
from pathlib import Path

NOTES = {'C': 0, 'C#': 1, 'D': 2, 'D#': 3, 'E': 4, 'F': 5, 'F#': 6, 'G': 7, 'G#': 8, 'A': 9, 'A#': 10, 'B': 11}


def semis(note):
    name, octave = note[:-2].rstrip('-'), int(note[-1])
    return NOTES[name] + 12 * octave


def main():
    here = Path(__file__).resolve().parent
    src, out = here / 'kit.json', here / 'kit-md.json'
    doc = copy.deepcopy(json.load(open(src)))
    doc['format'] = 'hydrapy-tracker-3'
    doc['sounds'] = [
        {'machine': 16, 'params': {}},
        {'machine': 17, 'params': {}},
        {'machine': 22, 'params': {'p2': 30}},
        {'machine': 34, 'params': {'p2': 60}},
        {'machine': None, 'params': {}},
        {'machine': None, 'params': {}},
    ]
    base = semis('D-4')
    for index, pattern in enumerate(doc['patterns']):
        if pattern is None:
            continue
        kick, snare, hat, fm = [lane['cells'] for lane in pattern['lanes'][:4]]
        for row in range(64):
            step = row % 16
            if kick[row]:
                if index == 3 and step == 0:
                    kick[row]['locks'] = {'p2': 110}
                elif step == 10:
                    kick[row]['locks'] = {'p1': 20, 'p2': 25}
            if snare[row]:
                if step == 15:
                    snare[row]['locks'] = {'p1': 90, 'p2': 12}
                elif row == 60:
                    snare[row]['locks'] = {'p1': 80}
            if hat[row]:
                if step % 4 == 2:
                    hat[row]['locks'] = {'machine': 23, 'p2': 40}
                elif index == 2:
                    hat[row]['locks'] = {'p2': 10 + 3 * step}
            if fm[row]:
                locks = {'p1': max(0, min(127, 40 + 3 * (semis(fm[row]['note']) - base)))}
                if index == 2:
                    locks['machine'] = 66
                fm[row]['locks'] = locks
    out.write_text(json.dumps(doc, indent=1) + '\n')
    locks = sum(1 for p in doc['patterns'] if p for lane in p['lanes'] for c in lane['cells'] if c and c.get('locks'))
    hits = sum(1 for p in doc['patterns'] if p for lane in p['lanes'] for c in lane['cells'] if c)
    print(f'{out}: {hits} hits, {locks} locked')


if __name__ == '__main__':
    main()
