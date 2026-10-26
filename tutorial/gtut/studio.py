"""HydraPy Studio on the stage: launch, its API, and a filmed Chromium controller.

Studio runs without DISPLAY, so moderngl falls back to EGL and renders on the GPU
(on the stage's Xvfb it would pick software GLX). It advertises itself in the
stage's own studio.json (HYDRAPY_STUDIO_FILE), never ~/.hydrapy/studio.json, and
keeps its .hydrapy/ state in a stage root that links to the HydraPy checkout.

The controller is Chromium in app mode (no address bar, so the session token in
the URL fragment is never on screen). Clicks go through the visible X cursor:
Playwright finds the element, the Hand moves there and clicks.
"""
from __future__ import annotations

import json
import os
import shutil
import sys
import time
import urllib.request
from pathlib import Path

from . import audio, display
from .procs import Procs, wait_until


class StudioError(RuntimeError):
    pass


def bridge_file(cfg) -> Path:
    return cfg.stage_dir() / 'studio.json'


def make_root(cfg) -> Path:
    """Studio's workspace root. Studio resolves symlinks and refuses files outside its
    root, so this is a real tree: the HydraPy worktree gtut setup made (its .hydrapy/
    is the stage's own), or else a copy of the checkout without .git and .hydrapy."""
    from .setup import hydrapy_tree
    if hydrapy_tree(cfg).exists():
        return hydrapy_tree(cfg)
    root = cfg.stage_dir() / 'studio-root'
    if not (root / 'hydrapy').exists():
        shutil.copytree(cfg.path('hydrapy'), root, dirs_exist_ok=True, symlinks=True,
                        ignore=shutil.ignore_patterns('.git', '.hydrapy', '__pycache__', '*.mp4', '*.gif'))
    return root


def reset_state(cfg, score: Path | None = None, outputs: dict | None = None):
    """Start a shot from a known score and output settings (Studio must be stopped)."""
    state = make_root(cfg) / '.hydrapy'
    state.mkdir(exist_ok=True)
    for name in ('tracker.json', 'outputs.json'):
        p = state / name
        if p.exists():
            p.unlink()
    if score:
        (state / 'tracker.json').write_text(Path(score).read_text())
    if outputs:
        (state / 'outputs.json').write_text(json.dumps(outputs, indent=2))


def launch(cfg, procs: Procs, patch: str | None = None, tracker: bool = True, display: bool = False) -> dict:
    """Start the stage's Studio. Without ``display`` it has no X display, renders on the GPU through EGL,
    and runs with --no-native-output (no Display menu). With it, Studio sees the stage's display so its
    Display menu can open a native Program window there; it then renders through the stage's software GL."""
    s = cfg['studio']
    root = make_root(cfg)
    env = audio.jack_env(cfg)
    if display:
        from .display import x_env
        env = x_env(cfg, env)
    else:
        env.pop('DISPLAY', None)
    env['HYDRAPY_STUDIO_FILE'] = str(bridge_file(cfg))
    if bridge_file(cfg).exists():
        bridge_file(cfg).unlink()
    hydrapy = Path(sys.executable).parent / 'hydrapy'
    argv = [hydrapy if hydrapy.exists() else 'hydrapy', 'studio', patch or s['patch'], '--root', root, '--backend', s['backend'],
            '--size', f"{s['size'][0]}x{s['size'][1]}", '--fps', s['fps'], '--port', s['port'], '--no-browser']
    if tracker:
        argv.append('--tracker')
    if not display:
        argv.append('--no-native-output')
    procs.start('studio', audio.wrap(cfg, argv), env=env, cwd=root)
    try:
        info = wait_until(lambda: json.loads(bridge_file(cfg).read_text()), 90, interval=0.25,
                          what='Studio to advertise itself')
    except TimeoutError as exc:
        raise StudioError(f'{exc}\n--- studio log ---\n{procs.tail("studio", 30)}') from None
    return info


class StudioAPI:
    def __init__(self, info: dict):
        self.info = info
        self.base = info['url']
        self.token = info['token']

    def call(self, action: str, **body):
        req = urllib.request.Request(f'{self.base}/api/{action}', data=json.dumps(body).encode(), method='POST',
                                     headers={'Content-Type': 'application/json', 'X-Hydrapy-Token': self.token})
        with urllib.request.urlopen(req, timeout=10) as r:
            return json.loads(r.read())

    def get(self, path: str):
        req = urllib.request.Request(f'{self.base}{path}', headers={'X-Hydrapy-Token': self.token})
        with urllib.request.urlopen(req, timeout=10) as r:
            return json.loads(r.read())


class Controller:
    """The tracker page in Chromium on the stage, driven through the visible cursor."""

    def __init__(self, cfg, info: dict, hand, tracker: bool = True):
        from playwright.sync_api import sync_playwright
        self.cfg, self.info, self.hand = cfg, info, hand
        self.pw = sync_playwright().start()
        x, y, w, h = cfg['stage']['windows']['studio']
        url = f"{info['url']}/{'tracker/' if tracker else ''}#{info['token']}"
        profile = cfg.stage_dir() / 'chromium-profile'
        args = [f'--app={url}', f'--window-position={x},{y}', f'--window-size={w},{h}',
                f"--force-device-scale-factor={cfg['stage']['ui_scale']}", '--no-first-run',
                '--no-default-browser-check', '--disable-infobars', '--hide-crash-restore-bubble',
                '--autoplay-policy=no-user-gesture-required', '--disable-features=Translate',
                '--password-store=basic']
        if os.geteuid() == 0:
            args.append('--no-sandbox')  # Chromium refuses to run as root otherwise
        kw = dict(headless=False, args=args, no_viewport=True, env=display.x_env(cfg))
        if cfg.path('chromium'):
            kw['executable_path'] = str(cfg.path('chromium'))
        self.context = self.pw.chromium.launch_persistent_context(str(profile), **kw)
        self.page = wait_until(lambda: next((p for p in self.context.pages if '127.0.0.1' in p.url), None), 30,
                               what='the controller page')
        if tracker:
            self.page.wait_for_function('window.gorgonPreview?.frames >= 1', timeout=60_000)
        self.window = display.wait_window(cfg, cls='chromium')
        display.place(cfg, self.window, cfg['stage']['windows']['studio'])
        time.sleep(0.5)
        self.offset = self.calibrate()
        self.popups: dict[str, object] = {}

    def calibrate(self, page=None, window_rect=None) -> tuple[float, float, float]:
        """Find page-to-screen mapping by moving the real cursor and reading clientX/Y."""
        page = page or self.page
        x, y, w, h = window_rect or self.cfg['stage']['windows']['studio']
        page.evaluate("""() => { window.__gtut = null;
            addEventListener('mousemove', e => { window.__gtut = [e.clientX, e.clientY, devicePixelRatio]; },
                             {capture: true}); }""")
        tx, ty = x + w // 2, y + h // 2
        self.hand.move(tx - 5, ty - 5, seconds=0.2)
        self.hand.move(tx, ty, seconds=0.2)
        got = wait_until(lambda: page.evaluate('window.__gtut'), 5, what='a mousemove in the page')
        cx, cy, dpr = got
        # screen = origin + client * dpr
        return tx - cx * dpr, ty - cy * dpr, dpr

    def _box(self, locator, page=None, offset=None) -> tuple[float, float, float, float]:
        locator.scroll_into_view_if_needed()
        b = locator.bounding_box()
        if not b:
            raise StudioError(f'{locator} has no box (hidden?)')
        ox, oy, dpr = offset or self.offset
        return ox + b['x'] * dpr, oy + b['y'] * dpr, b['width'] * dpr, b['height'] * dpr

    def locate(self, target, page=None):
        """target: a CSS selector string, or {role, name}, {label}, {text}, {selector}."""
        page = page or self.page
        if isinstance(target, str):
            return page.locator(target).first
        if 'role' in target:
            return page.get_by_role(target['role'], name=target.get('name'), exact=target.get('exact', True)).first
        if 'label' in target:
            return page.get_by_label(target['label'], exact=target.get('exact', False)).first
        if 'text' in target:
            return page.get_by_text(target['text'], exact=target.get('exact', False)).first
        return page.locator(target['selector']).first

    def point(self, target, page=None):
        x, y, w, h = self._box(self.locate(target, page))
        self.hand.move(x + w / 2, y + h / 2)

    def click(self, target, page=None, double: bool = False):
        """target may add modifiers: ['shift'] / ['ctrl'] (held on the X keyboard during the click)
        and double: true."""
        loc = self.locate(target, page)
        loc.wait_for(state='visible', timeout=10_000)
        x, y, w, h = self._box(loc)
        mods = target.get('modifiers', []) if isinstance(target, dict) else []
        double = double or (isinstance(target, dict) and target.get('double', False))
        self.hand.move(x + w / 2, y + h / 2)
        time.sleep(0.1)
        for m in mods:
            self.hand._xdo(['keydown', m])
        try:
            if double:
                self.hand.double_click()
            else:
                self.hand.click()
        finally:
            for m in reversed(mods):
                self.hand._xdo(['keyup', m])

    def fill(self, target, text: str, press: str | None = 'Tab', cps: float = 10):
        """Click into a field, select its contents and type, visibly."""
        self.click(target)
        self.page.keyboard.press('Control+A')
        for ch in str(text):
            self.page.keyboard.type(ch)
            time.sleep(1 / cps)
        if press:
            self.page.keyboard.press(press)

    def select(self, target, value: str):
        loc = self.locate(target)
        self.click(target)  # the dropdown opens on camera
        time.sleep(0.6)
        self.page.keyboard.press('Escape')
        loc.select_option(value)

    def press(self, key: str):
        self.page.keyboard.press(key)

    def type(self, text: str, cps: float = 8):
        for ch in text:
            self.page.keyboard.type(ch)
            time.sleep(1 / cps)

    def wait_for(self, js: str | None = None, selector: str | None = None, timeout: float = 15):
        if js:
            self.page.wait_for_function(js, timeout=timeout * 1000)
        if selector:
            self.page.locator(selector).first.wait_for(timeout=timeout * 1000)

    def eval(self, js: str):
        return self.page.evaluate(js)

    def open_output(self, which: str = 'program'):
        """Open Program or Auxiliary and put it in its stage window."""
        button = '#programBtn' if which == 'program' else '#auxBtn'
        with self.page.expect_popup() as pop:
            self.click(button)
        page = pop.value
        page.wait_for_load_state()
        try:
            page.locator('#outputDismiss').click(timeout=3000)
        except Exception:
            pass
        rect = self.cfg['stage']['windows'][which if which in self.cfg['stage']['windows'] else 'program']
        wins = display.find_windows(self.cfg, cls='chromium')
        wid = [w for w in wins if w != self.window][-1]
        display.place(self.cfg, wid, rect)
        self.popups[which] = (page, wid)
        return page

    def close(self):
        try:
            self.context.close()
        finally:
            self.pw.stop()
