"""Background for the tutorial's title cards: slow, dark, amber; text is drawn over it in compose."""
from hydrapy import *

set_bpm(138)
cells = voronoi(4, 0.05, 0.4).edge().color(1.0, 0.78, 0.2).luma(0.2, 0.1)
grain = noise(60, 0.1).color(0.06, 0.05, 0.03)
src(o1).scale(1.004).rotate(0.0006).blend(cells.mult(solid(0.5, 0.5, 0.5)), 0.08).out(o1)
src(o1).add(grain).brightness(-0.05).out(o0)
render(o0)
