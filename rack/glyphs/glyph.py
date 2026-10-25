"""Place one of the twelve marks on a panel: returns an SVG <g> scaled to `size`
(px across the 120-unit glyph box), centred on (cx, cy), in the panel's colours.
Strokes are thickened so they stay legible at panel scale (about 1.6 px core)."""
import re
from pathlib import Path

HERE = Path(__file__).resolve().parent


def glyph(n, cx, cy, size, ink, red, weight=2.0):
    svg = (HERE / f"mark{n:02d}.svg").read_text()
    body = svg.split(">", 1)[1].rsplit("</svg>", 1)[0]
    body = re.sub(r'stroke-width="([\d.]+)"', lambda m: f'stroke-width="{float(m.group(1)) * weight:.2f}"', body)
    body = body.replace("#INK", ink).replace("#RED", red)
    s = size / 120
    return f'<g transform="translate({cx - 60 * s:.2f},{cy - 60 * s:.2f}) scale({s:.4f})">{body}</g>'
