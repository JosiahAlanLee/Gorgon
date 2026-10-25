# The README quick-start patch, as chapter 3 writes it on camera (03-hydrapy.yaml, shot patch):
# its render shot renders this copy, so it does not depend on what is left in the sandbox.
from hydrapy import *

osc(10, 0.1, 0.8).rotate(Lfo(0.1, lo=0, hi=6.28)).kaleid(5).out(o0)
render(o0)
