"""VCV Rack on the stage: an isolated user folder, the Director probe, and actions.

Rack runs with `-u <stage>/rack-user`, so its settings, plugins, autosave and log
never touch the user's own Rack. The Director module (tutorial/probe) reports
where everything is on screen and takes view/highlight/param/load commands; the
mouse itself is real (xinput.Hand).
"""
from __future__ import annotations

import json
import math
import re
import shutil
import time
from pathlib import Path

from . import audio, display
from .procs import Procs, wait_until

PLUGIN_DIR = 'plugins-lin-x64'


class RackError(RuntimeError):
    pass


def user_dir(cfg) -> Path:
    return cfg.stage_dir() / 'rack-user'


def probe_dir(cfg) -> Path:
    d = cfg.stage_dir() / 'probe'
    d.mkdir(parents=True, exist_ok=True)
    return d


def write_settings(cfg):
    u = user_dir(cfg)
    u.mkdir(parents=True, exist_ok=True)
    x, y, w, h = cfg['stage']['windows']['rack']
    path = u / 'settings.json'
    try:
        settings = json.loads(path.read_text())
    except (OSError, ValueError):
        settings = {}
    settings.update({
        'windowMaximized': False, 'windowSize': [w, h], 'windowPos': [x, y],
        'pixelRatio': float(cfg['stage']['ui_scale']),
        'autoCheckUpdates': False, 'showTipsOnLaunch': False, 'verifyHttpsCerts': True,
        'tooltips': True, 'cpuMeter': False, 'lockModules': False,
        'sampleRate': float(cfg['rack']['sample_rate']),
        'frameRateLimit': float(cfg['rack']['frame_rate_limit']),
        'cableTension': 0.75, 'cableOpacity': 0.6, 'haloBrightness': 0.25,
        'autosaveInterval': 0.0,  # takes start from patch files, never an autosave
    })
    path.write_text(json.dumps(settings, indent=1))


def install_plugins(cfg, packages: list[Path]):
    d = user_dir(cfg) / PLUGIN_DIR
    d.mkdir(parents=True, exist_ok=True)
    for pkg in packages:
        slug = pkg.name.split('-')[0]
        for old in d.iterdir():  # one version of each plugin
            if old.name.split('-')[0] == slug or old.name == slug:
                shutil.rmtree(old) if old.is_dir() else old.unlink()
        shutil.copy2(pkg, d / pkg.name)


def set_aside_log(cfg) -> Path | None:
    """Rack ends log.txt with an END line when it quits cleanly; at launch, a log without it means "crashed",
    and Rack blocks on a dialog asking to clear the patch, which nobody on the stage answers. The stage stops
    Rack with a signal, so the line is usually missing: keep the previous log as log.prev.txt instead."""
    log = user_dir(cfg) / 'log.txt'
    if not log.exists():
        return None
    prev = log.with_name('log.prev.txt')
    log.replace(prev)
    return prev


def launch(cfg, procs: Procs, patch: Path):
    write_settings(cfg)
    set_aside_log(cfg)
    rack = cfg.path('rack')
    if not rack or not rack.exists():
        raise RackError(f'paths.rack {rack} does not exist')
    env = audio.jack_env(cfg, display.x_env(cfg))
    env['GTUT_PROBE_DIR'] = str(probe_dir(cfg))
    env['HYDRAPY_STUDIO_FILE'] = str(cfg.stage_dir() / 'studio.json')
    (probe_dir(cfg) / 'commands.jsonl').write_text('')
    layout = probe_dir(cfg) / 'layout.json'
    if layout.exists():
        layout.unlink()
    argv = audio.wrap(cfg, display.gl_prefix(cfg) + [rack, '-u', user_dir(cfg), str(patch)])
    p = procs.start('rack', argv, env=env, cwd=rack.parent)
    # By pid when Rack is the process we started; by title when a wrapper (vglrun, pw-jack) forked it.
    from .procs import wait_until as _wait
    wid = _wait(lambda: (display.find_windows(cfg, pid=p.pid) or display.find_windows(cfg, name='^VCV Rack') or [None])[-1],
                60, interval=0.25, what='the Rack window')
    display.place(cfg, wid, cfg['stage']['windows']['rack'])
    probe = Probe(cfg)
    try:
        wait_until(lambda: probe.layout(max_age=2.0), 60, what='the Director probe in Rack (is it in the patch?)')
    except TimeoutError as exc:
        raise RackError(f'{exc}\n--- rack log ---\n{procs.tail("rack", 30)}') from None
    return wid, probe


class Probe:
    """Client for the Director agent inside Rack."""

    def __init__(self, cfg):
        self.cfg = cfg
        self.dir = probe_dir(cfg)
        self.commands = self.dir / 'commands.jsonl'
        self.layout_path = self.dir / 'layout.json'
        self.seq = int(time.time() * 1000) % 10_000_000

    def layout(self, max_age: float | None = None) -> dict | None:
        try:
            data = json.loads(self.layout_path.read_text())
        except (OSError, ValueError):
            return None
        if max_age is not None and time.time() - self.layout_path.stat().st_mtime > max_age:
            return None
        return data

    def send(self, op: str, wait: bool = True, **kw) -> dict | None:
        self.seq += 1
        line = json.dumps(dict(op=op, seq=self.seq, **kw))
        with open(self.commands, 'a') as f:
            f.write(line + '\n')
        if not wait:
            return None
        seq = self.seq
        self.send_raw_dump()
        return wait_until(lambda: (l := self.layout()) and l.get('seq', 0) >= seq and l, 10,
                          interval=0.05, what=f'probe to run {op}')

    def send_raw_dump(self):
        with open(self.commands, 'a') as f:
            f.write(json.dumps(dict(op='dump')) + '\n')

    def fresh(self) -> dict:
        """A layout written after now (positions after any animation settles)."""
        t = time.time()
        self.send_raw_dump()
        return wait_until(lambda: self.layout_path.stat().st_mtime > t and self.layout(), 5,
                          interval=0.05, what='a fresh layout')

    def settle(self, timeout: float = 10):
        wait_until(lambda: not (l := self.fresh()).get('viewAnimating'), timeout, interval=0.1,
                   what='Rack view animation to finish')

    def log(self) -> list[str]:
        return (self.layout() or {}).get('log', [])


# References -------------------------------------------------------------
# "Gorgon"            the first Gorgon module (left to right, then top to bottom)
# "Vastish#2"         the second Vastish
# "Gorgon.out.Clock"  an output port by name (case-insensitive; exact, then prefix, then contains)
# "Gorgon.in.CV 1"    an input port; "Gorgon.out#0" by index
# "Vastish.param.Cutoff"

def _match(items: list[dict], key: str, want: str, what: str) -> dict:
    if want.startswith('#'):
        idx = int(want[1:])
        for it in items:
            if it.get('index') == idx:
                return it
        raise RackError(f'{what}: no index {idx}')
    w = want.lower()
    for rule in (lambda n: n == w, lambda n: n.startswith(w), lambda n: w in n):
        hits = [it for it in items if rule((it.get(key) or '').lower())]
        if len(hits) == 1:
            return hits[0]
        if len(hits) > 1:
            names = [h.get(key) for h in hits]
            raise RackError(f'{what}: {want!r} is ambiguous among {names}')
    raise RackError(f'{what}: no {want!r}; have {[it.get(key) for it in items]}')


def find_module(layout: dict, ref: str) -> dict:
    name, _, nth = ref.partition('#')
    mods = sorted(layout['modules'], key=lambda m: (m['grid'][1], m['grid'][0]))
    hits = [m for m in mods if name.lower() in (m.get('model', '').lower(), m.get('name', '').lower())]
    if not hits:
        raise RackError(f'no module {name!r} in the patch; have {[m.get("model") for m in mods]}')
    i = int(nth) - 1 if nth else 0
    if i >= len(hits):
        raise RackError(f'only {len(hits)} {name} modules')
    return hits[i]


def blank_point(mod: dict) -> tuple[float, float]:
    """A point on the module's panel, along its top or bottom edge, clear of every control and jack, for a
    right-click that should open the module's own menu. The middle of the top edge is a control on some
    panels (Vastish), and right-clicking a control opens that control's menu instead."""
    x, y, w, h = mod['box']
    boxes = [q['box'] for q in mod.get('params', []) if q.get('box')]
    centres = [q['centre'] for q in mod.get('ports', []) + mod.get('params', []) if q.get('centre')]
    best, clearance = (x + w / 2, y + 10), -1.0
    for dy in (10, 6, h - 10, h - 6):
        for i in range(1, 12):
            px, py = x + w * i / 12, y + dy
            if any(bx <= px <= bx + bw and by <= py <= by + bh for bx, by, bw, bh in boxes):
                continue
            d = min((math.hypot(px - cx, py - cy) for cx, cy in centres), default=1e9)
            if d > clearance:
                best, clearance = (px, py), d
    return best


def resolve(layout: dict, ref: str) -> tuple[dict, dict | None, str | None]:
    """'Module[#n][.in|.out|.param[.name|#i]]' -> (module, item, kind)."""
    m = re.match(r'^([^.]+?)(?:\.(in|out|input|output|param)(?:[.](.+)|(#\d+))?)?$', ref)
    if not m:
        raise RackError(f'bad reference {ref!r}')
    mod = find_module(layout, m.group(1))
    kind = m.group(2)
    if not kind:
        return mod, None, None
    sel = m.group(3) or m.group(4)
    if kind == 'param':
        return mod, _match(mod['params'], 'name', sel, f'{ref}'), 'param'
    ptype = 'input' if kind.startswith('in') else 'output'
    ports = [p for p in mod['ports'] if p['type'] == ptype]
    return mod, _match(ports, 'name', sel, f'{ref}'), ptype


class RackDriver:
    """Actions on the Rack window, used by the shot runner."""

    def __init__(self, cfg, probe: Probe, hand, window: int):
        self.cfg = cfg
        self.probe = probe
        self.hand = hand
        self.window = window

    def _point(self, ref: str) -> tuple[float, float]:
        mod, item, _ = resolve(self.probe.fresh(), ref)
        if item:
            return tuple(item['centre'])
        return blank_point(mod)

    def focus(self):
        display.focus(self.cfg, self.window)

    def load(self, patch: Path, settle: float = 1.5):
        self.probe.send('load', path=str(Path(patch).resolve()))
        time.sleep(settle)
        self.probe.fresh()

    def save(self, path: Path):
        self.probe.send('save', path=str(Path(path).resolve()))

    def view(self, modules=None, zoom=None, seconds: float = 1.2, margin: float = 24):
        kw = dict(seconds=seconds, margin=margin)
        if modules:
            mods = [modules] if isinstance(modules, str) else modules
            layout = self.probe.fresh()
            kw['modules'] = [find_module(layout, m)['id'] for m in mods]
        if zoom is not None:
            kw['zoom'] = zoom
        self.probe.send('view', **kw)
        time.sleep(seconds)
        self.probe.settle()

    def highlight(self, ref: str, label: str = '', seconds: float = 3.0):
        mod, item, kind = resolve(self.probe.fresh(), ref)
        kw = dict(module=mod['id'], label=label, seconds=seconds)
        if item:
            kw.update(type=kind, index=item['index'])
        self.probe.send('highlight', **kw)

    def point(self, ref: str):
        self.hand.move(*self._point(ref))

    def click(self, ref: str):
        self.hand.click(*self._point(ref))

    def cable(self, src: str, dst: str, seconds: float | None = None):
        a = self._point(src)
        b = self._point(dst)
        self.hand.drag(a, b, seconds=seconds)
        time.sleep(0.2)
        _, port, _ = resolve(self.probe.fresh(), dst)
        if not port or port['cables'] < 1:
            raise RackError(f'cable {src} -> {dst} did not connect (check coordinates and zoom)')

    def unplug(self, ref: str):
        """Drag a cable's plug off an input and let go over empty rack."""
        x, y = self._point(ref)
        self.hand.drag((x, y), (x, y + 140))

    def knob(self, ref: str, value: float | None = None, scaled: float | None = None, seconds: float = 1.5):
        """Hover the control and turn it: the probe sets the value smoothly (exact, repeatable)."""
        mod, item, kind = resolve(self.probe.fresh(), ref)
        if kind != 'param':
            raise RackError(f'{ref} is not a param')
        self.hand.move(*item['centre'])
        kw = dict(module=mod['id'], index=item['index'], seconds=seconds)
        if value is not None:
            kw['value'] = value
        else:
            kw['scaled'] = scaled
        self.probe.send('param', **kw)
        time.sleep(seconds)

    def menu(self, ref: str, path: list[str], pause: float = 0.5):
        """Right-click a module (or a port/param) and walk a context-menu path by item text."""
        self.focus()
        point = self._point(ref)
        self.hand.right_click(*point)
        row = None  # y of the item whose submenu is open
        for i, text in enumerate(path):
            time.sleep(pause)
            items = [m for m in self.probe.fresh().get('menu', []) if 'text' in m]
            if not items and i == 0:  # the first click after a load can be taken by window focus
                self.hand.right_click(*point)
                time.sleep(pause)
                items = [m for m in self.probe.fresh().get('menu', []) if 'text' in m]
            item = _match(items, 'text', text, f'menu item under {ref}')
            if item.get('disabled'):
                raise RackError(f'menu item {text!r} is disabled')
            x, y = item['centre']
            if row is not None:
                # Into a submenu along the parent's row, as a hand does, and in straight lines: a diagonal
                # or arcing path crosses the parent menu's other items and opens their submenus instead.
                self.hand.move(x, row, straight=True)
            if i < len(path) - 1:
                self.hand.move(x, y, straight=row is not None)  # submenus open on hover
                row = y
            else:
                self.hand.move(x, y, straight=row is not None)
                self.hand.click(x, y)
        time.sleep(pause)

    def menu_items(self) -> list[dict]:
        return self.probe.fresh().get('menu', [])

    def escape(self):
        self.hand.key('Escape', window=self.window)

    def add_module(self, query: str, at: tuple[float, float] | None = None, fallback_patch: Path | None = None):
        """Open the module browser on empty rack, search, and add the first result.

        Rack's browser is not a menu, so the probe can't see its entries. Enter adds the
        top result in Rack 2; if the module count doesn't rise, fall back to loading the
        next patch state (a cut), when one is given.
        """
        layout = self.probe.fresh()
        before = len(layout['modules'])
        if at is None:
            x, y, w, h = self.cfg['stage']['windows']['rack']
            at = (x + w * 0.75, y + h * 0.5)
        self.focus()
        self.hand.right_click(*at)
        time.sleep(0.8)
        self.hand.type(query)
        time.sleep(0.8)
        self.hand.key('Return', window=self.window)
        time.sleep(1.0)
        if len(self.probe.fresh()['modules']) > before:
            return True
        self.escape()
        if fallback_patch:
            self.load(fallback_patch)
            return False
        raise RackError(f'could not add {query!r} from the browser; give add_module a fallback patch')
