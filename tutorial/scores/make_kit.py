#!/usr/bin/env python3
"""Write scores/kit.json: demo.json's picture automation with the four audio lanes as a drum kit.

DrumLab's Percussion plays Gorgon's lanes 1-4 as Kick, Snare, Hat and FM (its default four-lane kit), and
tunes each model from Gorgon's pitch with C4 (at key D) as the model's base. demo.json's Breaks lane uses
C-2/D-2, which DrumLab would play four octaves down, so this score keeps the notes around C4. The Dolly Z,
Roll and Feedback keys, easing, names, tempo and key are demo.json's, so the picture moves as before.

    python scores/make_kit.py      # rewrites scores/kit.json
"""
import copy
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent


def hit(note, vel):
    return {'note': note, 'vel': vel}


def lanes(index):
    """Kick, snare, hat and FM for one 64-row pattern (four bars of sixteen rows); `index` varies them."""
    kick, snare, hat, fm = ([None] * 64 for _ in range(4))
    for r in range(64):
        step = r % 16
        if index == 3:                       # SUSPEND: half time
            if step in (0, 10):
                kick[r] = hit('C-4', 112 if step == 0 else 84)
            if step == 8:
                snare[r] = hit('C-4', 104)
            if step % 4 == 2:
                hat[r] = hit('C-4', 50)
            continue
        if step % 4 == 0:
            kick[r] = hit('C-4', 116 if step == 0 else 100)
        if index >= 1 and step == 10:
            kick[r] = hit('C-4', 82)
        if step in (4, 12):
            snare[r] = hit('C-4', 102)
        if index in (1, 2) and step == 15:
            snare[r] = hit('C-4', 34)          # a ghost before the bar line
        if index == 2:                        # ASCENT: sixteenth hats, accented on the beat
            hat[r] = hit('G-4' if step % 4 == 2 else 'C-4', 64 if step % 4 == 0 else 30 + 8 * (step % 2 == 0))
        elif step % 2 == 0:
            hat[r] = hit('G-4' if step % 4 == 2 else 'C-4', 60 if step % 4 == 0 else 42)
    figure = {0: [(3, 'D-4', 80), (11, 'A-4', 70)], 1: [(3, 'D-4', 84), (7, 'F-4', 64), (11, 'A-4', 74), (14, 'C-5', 60)],
              2: [(3, 'F-4', 80), (6, 'A-4', 66), (11, 'C-5', 76), (13, 'D-5', 58)], 3: [(6, 'D-4', 70)]}[index]
    for bar in range(4):
        for step, note, vel in figure:
            fm[bar * 16 + step] = hit(note, vel - 6 * (bar % 2))
    return [kick, snare, hat, fm]


def main():
    demo = json.loads((HERE / 'demo.json').read_text())
    doc = copy.deepcopy(demo.get('document', demo))
    for i, pattern in enumerate(doc['patterns']):
        pattern['audio'] = lanes(i)
    doc['mutes'] = [False] * 4
    out = dict(demo, document=doc) if 'document' in demo else doc
    (HERE / 'kit.json').write_text(json.dumps(out, indent=1) + '\n')


if __name__ == '__main__':
    main()
