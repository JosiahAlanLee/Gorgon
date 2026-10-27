#!/usr/bin/env python3
"""Gorgon panel (outlined text) and the matching widget layout header.
One table drives both, so artwork and component positions cannot drift apart.
Requires fontTools and DejaVu Sans."""
from pathlib import Path
import sys

from fontTools.ttLib import TTFont
from fontTools.pens.svgPathPen import SVGPathPen

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / 'glyphs'))
from glyph import glyph  # the twelve marks (Clavicula Matricis, Tabula XIII)

font = TTFont('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf')
glyphs = font.getGlyphSet(); cmap = font.getBestCmap(); units = font['head'].unitsPerEm


def width(s, size):
    return sum(font['hmtx'][cmap[ord(c)]][0] for c in s) * size / units


def text(s, x, y, size=9, color='#d9e2e5', anchor='middle'):
    scale = size / units
    if anchor == 'middle': x -= width(s, size) / 2
    elif anchor == 'end': x -= width(s, size)
    out = []
    for c in s:
        pen = SVGPathPen(glyphs); glyphs[cmap[ord(c)]].draw(pen)
        out.append(f'<path d="{pen.getCommands()}" transform="translate({x:.3f},{y}) scale({scale},-{scale})" fill="{color}"/>')
        x += font['hmtx'][cmap[ord(c)]][0] * scale
    return ''.join(out)


W, H = 210, 380  # 14 HP: six gate jacks, and BUS IN beside BUS OUT
ACCENT, LABEL, DIM, OUT = '#f0b46a', '#d9e2e5', '#7f949c', '#8fdcc4'
FIVE = (25, 65, 105, 145, 185)                     # five jacks across
SIX = (25, 57, 89, 121, 153, 185)                  # the six gates

display = (12, 46, W - 24, 52)                      # x, y, w, h
# Transport row: button and trigger input for PLAY and RESET, then the offset trimpot.
TRANSPORT_Y = 130
buttons = {'PLAY': (FIVE[0], TRANSPORT_Y), 'RESET': (FIVE[2], TRANSPORT_Y)}
lights = {'PLAY_LIGHT': (FIVE[0], TRANSPORT_Y), 'LINK_LIGHT': (W - 18, 58), 'CLOCK_LIGHT': (W - 18, 74)}
trimpots = {'OFFSET': (FIVE[4], TRANSPORT_Y)}
jacks_in = {'PLAY_IN': (FIVE[1], TRANSPORT_Y), 'RESET_IN': (FIVE[3], TRANSPORT_Y),
            'CV1': (FIVE[0], 200), 'CV2': (FIVE[1], 200), 'CV3': (FIVE[2], 200), 'CV4': (FIVE[3], 200),
            'BUS_IN': (FIVE[4], 310)}
jacks_out = {'CLOCK': (FIVE[0], 246), 'RESET_OUT': (FIVE[1], 246), 'RUN': (FIVE[2], 246), 'VEL': (FIVE[3], 246), 'PITCH': (FIVE[4], 246),
             **{f'GATE{n}': (x, 278) for n, x in enumerate(SIX, 1)},
             'Z': (FIVE[0], 310), 'ROLL': (FIVE[1], 310), 'FEEDBACK': (FIVE[2], 310), 'GATES': (FIVE[3], 310),
             'OPTICAL_L': (FIVE[0], 342), 'OPTICAL_R': (FIVE[1], 342), 'BREAKS_L': (FIVE[2], 342), 'BREAKS_R': (FIVE[3], 342),
             'BUS': (FIVE[4], 342)}
transport_labels = {'PLAY': (FIVE[0], 'PLAY'), 'PLAY_IN': (FIVE[1], 'TRIG'), 'RESET': (FIVE[2], 'RESET'),
                    'RESET_IN': (FIVE[3], 'TRIG'), 'OFFSET': (FIVE[4], 'OFFSET')}
labels = {'CV1': 'CV 1', 'CV2': 'CV 2', 'CV3': 'CV 3', 'CV4': 'CV 4', 'BUS_IN': 'BUS IN',
          'CLOCK': 'CLOCK', 'RESET_OUT': 'RESET', 'RUN': 'RUN', 'VEL': 'VEL', 'PITCH': 'V/OCT',
          **{f'GATE{n}': f'GATE {n}' for n in range(1, 7)},
          'Z': 'Z', 'ROLL': 'ROLL', 'FEEDBACK': 'FDBK', 'GATES': 'GATES', 'OPTICAL_L': 'OPT L', 'OPTICAL_R': 'OPT R',
          'BREAKS_L': 'BRK L', 'BREAKS_R': 'BRK R', 'BUS': 'BUS OUT'}
# BUS IN sits in the outputs block beside BUS OUT, on a cut-out of the inputs' lighter panel.
BUS_IN_PAD = (FIVE[4] - 19, 299, 38, 32)            # x, y, w, h

p = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">',
     f'<rect width="{W}" height="{H}" fill="#161d22"/>',
     f'<rect x="1" y="1" width="{W-2}" height="{H-2}" fill="none" stroke="#5c6d74"/>',
     f'<rect x="6" y="226" width="{W-12}" height="148" rx="4" fill="#0e1417"/>',            # outputs block
     f'<rect x="{BUS_IN_PAD[0]}" y="{BUS_IN_PAD[1]}" width="{BUS_IN_PAD[2]}" height="{BUS_IN_PAD[3]}" rx="3" fill="#161d22"/>',
     f'<rect x="{display[0]}" y="{display[1]}" width="{display[2]}" height="{display[3]}" rx="3" fill="#0a0f11" stroke="#41545c"/>']
p += [text('Gorgon', 12, 30, 20, OUT, 'start'),
      text('BRIDGE', 12 + width('Gorgon', 20) + 8, 30, 8, LABEL, 'start')]
# XI Reflexive loop (the map moves it): Rack follows Gorgon's clock and moves it back.
p.append(glyph(11, W - 26, 22, 26, LABEL, ACCENT))
p += [text('LINK', W - 26, 61, 6, DIM, 'end'), text('CLK', W - 26, 77, 6, DIM, 'end')]
for y in (106, 168, 224):
    p.append(f'<path d="M12 {y}H{W-12}" stroke="#41545c"/>')
p += [text('TRANSPORT', 12, 117, 7, ACCENT, 'start'), text('TO GORGON', 12, 180, 7, ACCENT, 'start'),
      text('FROM GORGON', 12, 233, 6, OUT, 'start')]
for name, (x, label) in transport_labels.items():
    p.append(text(label, x, TRANSPORT_Y + 23, 6.5))
for name, (x, y) in jacks_in.items():
    if name.startswith('CV'):
        p.append(text(labels[name], x, y + 22, 6.5))
    elif name == 'BUS_IN':
        p.append(text(labels[name], x, y + 19, 6, ACCENT))
for name, (x, y) in jacks_out.items():
    p.append(text(labels[name], x, y + 19, 6 if name == 'BUS' or name.startswith('GATE') and name != 'GATES' else 6.5,
                  ACCENT if name == 'BUS' else OUT))
panel = ''.join(p) + '</svg>'
(root / 'res').mkdir(exist_ok=True)
(root / 'res/Gorgon.svg').write_text(panel)

# Layout header for the widget.
lines = ['// Generated by tools/make_panel.py together with res/Gorgon.svg; do not edit.', '#pragma once',
         'namespace gorgon_layout {']
for group, prefix in ((buttons, 'BUTTON'), (lights, 'LIGHT'), (trimpots, 'TRIMPOT'), (jacks_in, 'JACK'), (jacks_out, 'JACK')):
    for name, (x, y) in group.items():
        lines.append(f'constexpr float {prefix}_{name}_X = {x}, {prefix}_{name}_Y = {y};')
lines += [f'constexpr float DISPLAY_X = {display[0]}, DISPLAY_Y = {display[1]}, DISPLAY_W = {display[2]}, DISPLAY_H = {display[3]};',
          f'constexpr float WIDTH = {W};', '} // namespace gorgon_layout', '']
(root / 'src/Layout.hpp').write_text('\n'.join(lines))
