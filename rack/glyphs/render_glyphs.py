#!/usr/bin/env python3
"""Render the twelve Revision 3 sketching marks (Clavicula Matricis, Tabula XIII) as
clean panel glyphs: core shape only, no seal ornament, a light pen wobble, fixed seed.
Needs a checkout of Systems_Practice_Grimoire; pass its generator directory.
Writes markNN.svg (120 x 120, colours as placeholders INK/RED) next to this script.
Usage: render_glyphs.py /path/to/Systems_Practice_Grimoire/generator"""
import random
import sys
from pathlib import Path

gen = Path(sys.argv[1]).resolve()
sys.path.insert(0, str(gen))
from core import Doc, offset_line  # noqa: E402
from marks import (Hand, sig_standing, sig_candidate, sig_boundary, sig_authority, sig_bottleneck,  # noqa: E402
                   sig_regulation, sig_mark, sig_branch, sig_ground, sig_reflexive, sig_invariant, dotted, asterisk)

NAMES = ["Interest-bearing being", "Sentience candidate", "Boundary", "Centre of decision", "Bottleneck",
         "Regulation", "Revision", "Additive change", "Substitutive change", "Generative substrate",
         "Reflexive loop", "Learned invariant"]
INK, RED = "#INK", "#RED"  # replaced by each panel's palette


def styles():
    st = {}
    for c, col in (("ink", INK), ("red", RED)):
        for k, w in (("core", 3.4), ("orn", 2.3), ("hair", 1.4)):
            st[f"{c}_{k}"] = {"fill": "none", "stroke": col, "stroke-width": str(w), "stroke-linecap": "round",
                              "stroke-linejoin": "round"}
        st[f"{c}_fill"] = {"fill": col}
    return st


def draw(i, h, cx, cy, s):
    """Tabula XIII's draw_mark, with the seal ornament off (orn=0) where the sigil has one."""
    if i == 0: sig_standing(h, cx, cy + 4, s * 0.15, crown=True, orn=0)
    elif i == 1: sig_candidate(h, cx, cy, s * 0.15, orn=0)
    elif i == 2: sig_boundary(h, cx, cy, s * 0.42, orn=0)
    elif i == 3: sig_authority(h, cx, cy + s * 0.14, s * 0.68, s * 0.55, orn=0)
    elif i == 4: sig_bottleneck(h, cx, cy, s * 0.48, orn=0)
    elif i == 5: sig_regulation(h, cx, cy, s * 0.36, orn=0)
    elif i == 6:
        pts = sig_mark(h, cx, cy, s * 0.44, "red", "core", branch=False, turns=1.75, ang0=90)
        dotted(h, offset_line(pts[60:], s * 0.07), s * 0.075, 1.3, "red")
    elif i == 7: sig_branch(h, cx - s * 0.52, cx + s * 0.46, cy + s * 0.14, orn=0)
    elif i == 8: sig_branch(h, cx - s * 0.52, cx + s * 0.46, cy + s * 0.14, struck=True, orn=0)
    elif i == 9:
        sig_ground(h, cx - s * 0.55, cx + s * 0.55, cy + s * 0.04, s * 0.26, step=7)
        h.dot(cx, cy - s * 0.2, s * 0.08)
    elif i == 10: sig_reflexive(h, cx, cy + s * 0.12, s * 0.52, orn=0)
    elif i == 11: sig_invariant(h, cx + s * 0.04, cy, s * 0.6, orn=0)


out = Path(__file__).resolve().parent
for i in range(12):
    doc = Doc(120, 120, styles())
    h = Hand(doc, random.Random(1300 + i), mode="ink", amp=0.25)
    draw(i, h, 60, 60, 100)
    (out / f"mark{i + 1:02d}.svg").write_text(doc.svg())
print("wrote mark01..mark12.svg")
