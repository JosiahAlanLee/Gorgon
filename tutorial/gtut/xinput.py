"""A visible, human-paced mouse and keyboard on the stage display (xdotool).

Every pointer action moves the real X cursor along a minimum-jerk path, so the
capture shows the hand travelling to what it clicks. Coordinates are stage-screen
pixels.
"""
from __future__ import annotations

import math
import random
import subprocess
import time

from .display import x_env


class Hand:
    def __init__(self, cfg, speed: float = 1.0, seed: int = 7):
        self.cfg = cfg
        self.speed = speed
        self.rng = random.Random(seed)  # deterministic wobble: takes are repeatable

    # low level
    def _xdo(self, args: list):
        r = subprocess.run(['xdotool', *map(str, args)], env=x_env(self.cfg), capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError(f'xdotool: {r.stderr.strip()}')
        return r.stdout

    def position(self) -> tuple[int, int]:
        out = self._xdo(['getmouselocation', '--shell'])
        vals = dict(line.split('=', 1) for line in out.splitlines() if '=' in line)
        return int(vals['X']), int(vals['Y'])

    def _path(self, start, end, seconds: float, fps: int = 90):
        (x0, y0), (x1, y1) = start, end
        n = max(2, int(seconds * fps))
        # a slight arc, as a wrist makes
        dx, dy = x1 - x0, y1 - y0
        dist = math.hypot(dx, dy)
        bend = self.rng.uniform(-0.08, 0.08) * dist
        nx, ny = (-dy / dist, dx / dist) if dist else (0, 0)
        pts = []
        for i in range(1, n + 1):
            t = i / n
            s = 10 * t**3 - 15 * t**4 + 6 * t**5  # minimum jerk
            arc = math.sin(math.pi * s) * bend
            pts.append((round(x0 + dx * s + nx * arc), round(y0 + dy * s + ny * arc)))
        return pts, seconds / n

    def _travel_time(self, start, end) -> float:
        dist = math.hypot(end[0] - start[0], end[1] - start[1])
        # Fitts-like: quick for short hops, ~0.9 s across a 1080p window
        return (0.18 + 0.25 * math.log2(1 + dist / 40)) / self.speed

    def move(self, x: float, y: float, seconds: float | None = None):
        start = self.position()
        end = (round(x), round(y))
        if start == end:
            return
        seconds = self._travel_time(start, end) if seconds is None else seconds
        pts, dt = self._path(start, end, seconds)
        chain: list = []
        for px, py in pts:
            chain += ['mousemove', px, py, 'sleep', f'{dt:.4f}']
        self._xdo(chain)

    def click(self, x=None, y=None, button: int = 1, settle: float = 0.12):
        if x is not None:
            self.move(x, y)
            time.sleep(settle / self.speed)
        self._xdo(['click', button])

    def right_click(self, x=None, y=None):
        self.click(x, y, button=3)

    def double_click(self, x=None, y=None):
        if x is not None:
            self.move(x, y)
            time.sleep(0.1)
        self._xdo(['click', '--repeat', 2, '--delay', 90, 1])

    def drag(self, start, end, seconds: float | None = None, button: int = 1):
        self.move(*start)
        time.sleep(0.15 / self.speed)
        self._xdo(['mousedown', button])
        time.sleep(0.12 / self.speed)
        self.move(*end, seconds=seconds or max(0.5, self._travel_time(start, end) * 1.4))
        time.sleep(0.15 / self.speed)
        self._xdo(['mouseup', button])

    def scroll(self, clicks: int):
        button = 4 if clicks > 0 else 5
        for _ in range(abs(clicks)):
            self._xdo(['click', button])
            time.sleep(0.04)

    def park(self, rect):
        """Move the cursor out of the way, to the bottom-right of a window rect."""
        x, y, w, h = rect
        self.move(x + w - 40, y + h - 40, seconds=0.6)

    # keyboard
    def type(self, text: str, cps: float = 14.0, window: int | None = None):
        """Type at a human rate, with small variations; deterministic per Hand."""
        for ch in text:
            args = ['type', '--delay', 0]
            if window is not None:
                args = ['type', '--window', window, '--delay', 0]
            self._xdo(args + [ch])
            pause = 1.0 / cps * self.rng.uniform(0.6, 1.5)
            if ch in ' ./-|':
                pause *= 1.4
            time.sleep(pause / self.speed)

    def key(self, *keys: str, window: int | None = None):
        args = ['key', '--clearmodifiers']
        if window is not None:
            args += ['--window', window]
        self._xdo(args + list(keys))
