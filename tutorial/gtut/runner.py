"""The stage (display, JACK, Studio, controller, Rack, terminal) and the takes shot on it."""
from __future__ import annotations

import json
import os
import subprocess
import sys
import time
import traceback
from pathlib import Path

from . import audio, display, rack, studio
from .actions import ACTIONS
from .capture import Take
from .procs import Procs
from .script import CONTROL, Shot
from .terminal import Terminal
from .xinput import Hand

VISUAL = {'controller': 'studio', 'studio': 'studio', 'program': 'program', 'aux': 'program',
          'rack': 'rack', 'terminal': 'terminal'}
LEAD = 0.8  # seconds from the slate to the first word


class Stage:
    def __init__(self, cfg):
        self.cfg = cfg
        self.procs = Procs(cfg.stage_dir())
        self.hand = Hand(cfg)
        self.api = None
        self.controller = None
        self.rack = None
        self.term = None
        self.slate = None
        self.up_services: set[str] = set()
        self.take_dir = cfg.work / 'takes'

    # lifecycle ---------------------------------------------------------
    def env(self) -> dict:
        env = audio.jack_env(self.cfg, display.x_env(self.cfg))
        env['HYDRAPY_STUDIO_FILE'] = str(studio.bridge_file(self.cfg))
        return env

    def up(self, needs: set[str]):
        needs = set(needs)
        if 'controller' in needs or 'program' in needs or 'aux' in needs:
            needs.add('studio')
        if 'base' not in self.up_services:
            display.start_display(self.cfg, self.procs)
            audio.start_server(self.cfg, self.procs)
            self.slate = audio.Slate()
            self.up_services.add('base')
        if 'studio' in needs and 'studio' not in self.up_services:
            info = studio.launch(self.cfg, self.procs)
            self.api = studio.StudioAPI(info)
            self.up_services.add('studio')
        if needs & {'controller', 'program', 'aux'} and 'controller' not in self.up_services:
            self.controller = studio.Controller(self.cfg, json.loads(studio.bridge_file(self.cfg).read_text()), self.hand)
            # View → Text size lives in a closed <details>; set it as the menu would.
            self.controller.page.evaluate(
                "v => { const s = document.getElementById('textSize'); if (s) { s.value = v;"
                " s.dispatchEvent(new Event('change')); } }", self.cfg['studio']['text_size'])
            self.up_services.add('controller')
        for out in ('program', 'aux'):
            if out in needs and out not in self.up_services:
                self.controller.open_output(out)
                self.up_services.add(out)
        if 'rack' in needs and 'rack' not in self.up_services:
            blank = self.cfg.work / 'patches' / 'blank.vcv'
            if not blank.exists():
                blank.parent.mkdir(parents=True, exist_ok=True)
                blank.write_text(json.dumps(dict(version='2.6.6', cables=[], modules=[
                    dict(id=1, plugin='GorgonTutorialProbe', model='Director', version='2.0.0', pos=[0, 0], params=[])])))
            wid, probe = rack.launch(self.cfg, self.procs, blank)
            self.rack = rack.RackDriver(self.cfg, probe, self.hand, wid)
            self.up_services.add('rack')
        if 'terminal' in needs and 'terminal' not in self.up_services:
            (self.cfg.work / 'sandbox').mkdir(parents=True, exist_ok=True)
            # A Studio started on camera must not replace the stage's advertisement, or the
            # stage's Rack would link to it; it gets its own bridge file.
            self.term = Terminal(self.cfg, self.procs, self.hand, cwd=self.cfg.work / 'sandbox', extra_env={
                'HYDRAPY_STUDIO_FILE': str(self.cfg.stage_dir() / 'terminal-studio.json'),
                'RACK_USER': str(rack.user_dir(self.cfg)),
                'RACK_SDK': str(self.cfg.path('rack_sdk') or ''),
                'PATH': f"{Path(sys.executable).parent}:{os.environ.get('PATH', '')}"})
            self.up_services.add('terminal')

    def down(self):
        for closer in (lambda: self.controller and self.controller.close(),
                       lambda: self.slate and self.slate.close()):
            try:
                closer()
            except Exception:
                pass
        self.procs.stop_all(order=['terminal', 'rack', 'studio', 'jack', 'wm', 'display'])
        self.up_services.clear()

    def patch_path(self, name) -> Path:
        p = Path(name)
        if p.suffix == '.vcv' and p.exists():
            return p.resolve()
        built = self.cfg.work / 'patches' / f'{p.stem}.vcv'
        if not built.exists():
            raise FileNotFoundError(f'{built} is not built; run gtut patches')
        return built

    # setup ---------------------------------------------------------------
    def setup(self, shot: Shot):
        s = shot.setup or {}
        if 'studio' in s and self.api:
            st = s['studio']
            if st.get('patch'):
                self.api.call('load', path=st['patch'])
            if st.get('score'):
                score_path = Path(st['score'])
                if not score_path.is_absolute():
                    score_path = self.cfg.root / score_path
                doc = json.loads(score_path.read_text())
                snap = self.api.get('/api/tracker')
                self.api.call('tracker_score', document=doc.get('document', doc), revision=snap['revision'])
            transport = st.get('transport', 'stop')
            self.api.call('tracker_transport', enabled=transport != 'off', playing=transport == 'play', beat=0)
            if 'jack' in st:
                self.api.call('jack_audio', enabled=bool(st['jack']))
            if self.controller:
                ACTIONS['studio.reload'](self, None)
        if 'rack' in s and self.rack:
            r = s['rack']
            if r.get('patch'):
                self.rack.load(self.patch_path(r['patch']))
            if r.get('view', True):
                v = r['view'] if isinstance(r.get('view'), dict) else {}
                self.rack.view(v.get('modules'), v.get('zoom'), seconds=0, margin=v.get('margin', 24))
        if 'terminal' in s and self.term:
            if s['terminal'].get('clear', True):
                self.term.quiet('cd ' + str(self.cfg.work / 'sandbox'))
        for a in s.get('actions', []):
            self.do(a)
        windows = self.windows_for(shot)
        if windows:
            self.hand.park(self.cfg['stage']['windows'][windows[0]])

    # takes ---------------------------------------------------------------
    def windows_for(self, shot: Shot) -> list[str]:
        names = []
        for f in shot.frames:
            if f.get('window') and f['window'] not in names:
                names.append(f['window'])
        if not names:
            for n in shot.needs:
                w = VISUAL.get(n)
                if w and w not in names:
                    names.append(w)
                    break
        return names

    def region(self, shot: Shot):
        rects = [self.cfg['stage']['windows'][w] for w in self.windows_for(shot)]
        if not rects:
            raise ValueError(f'{shot.key}: nothing to film (no frames or visual needs)')
        x0 = min(r[0] for r in rects)
        y0 = min(r[1] for r in rects)
        x1 = max(r[0] + r[2] for r in rects)
        y1 = max(r[1] + r[3] for r in rects)
        return [x0, y0, x1 - x0, y1 - y0]

    def flash(self, shot: Shot) -> str:
        """Show the slate flash in the filmed region and fire the click together."""
        first = self.windows_for(shot)[0]
        if first == 'rack' and self.rack:
            self.rack.probe.send('flash', wait=False, seconds=0.1)
            time.sleep(0.008)
            self.slate.fire()
            return 'rack'
        if first in ('studio', 'program') and self.controller:
            page = self.controller.page if first == 'studio' else self.controller.popups['program'][0]
            page.evaluate("""() => new Promise(r => { const d = document.createElement('div');
                d.style.cssText = 'position:fixed;inset:0;background:#fff;z-index:2147483647';
                document.body.appendChild(d);
                requestAnimationFrame(() => requestAnimationFrame(() => { r(); setTimeout(() => d.remove(), 100); })); })""")
            self.slate.fire()
            return first
        if first == 'terminal' and self.term:
            tty = _child_tty(self.term.proc.pid)
            if tty:
                with open(tty, 'w') as t:
                    t.write('\033[?5h')
                    t.flush()
                self.slate.fire()
                time.sleep(0.1)
                with open(tty, 'w') as t:
                    t.write('\033[?5l')
                return 'terminal'
        self.slate.fire()
        return 'none'

    def do(self, a: dict, t0: float | None = None, cues: dict | None = None, log: list | None = None):
        name = next(k for k in a if k not in CONTROL)
        due = None
        if 'at' in a and t0 is not None:
            due = t0 + LEAD + cues[a['at']]
            wait = due - time.time()
            if wait > 0:
                time.sleep(wait)
        if a.get('after'):
            time.sleep(float(a['after']))
        start = time.time()
        if 'mark' in a and log is not None:
            log.append(dict(mark=a['mark'], t=start - t0 if t0 else 0))
        ACTIONS[name](self, a[name])
        if log is not None and t0 is not None:
            log.append(dict(action=name, start=start - t0, end=time.time() - t0,
                            late=max(0.0, start - due) if due else 0.0))

    def shoot(self, shot: Shot, meta: dict, take_name: str | None = None) -> Path:
        self.up(set(shot.needs))
        self.setup(shot)
        out = self.take_dir / shot.key / (take_name or time.strftime('%Y%m%d-%H%M%S'))
        out.mkdir(parents=True, exist_ok=True)
        self.take_dir_current = out
        rect = self.region(shot)
        log: list = []
        info = dict(shot=shot.key, rect=rect, lead=LEAD, narration=meta, windows=self.windows_for(shot),
                    stage_windows=self.cfg['stage']['windows'], fps=self.cfg['capture']['fps'])
        error = None
        take = Take(self.cfg, out / 'take.mkv', rect, self.slate)
        take.start()
        try:
            time.sleep(self.cfg['capture']['preroll'])
            t0 = time.time()
            info['slate_method'] = self.flash(shot)
            info['slate_wall'] = t0 - take.started
            for a in shot.actions:
                self.do(a, t0, meta.get('cues', {}), log)
            end = t0 + LEAD + meta.get('duration', 0) + shot.hold
            while time.time() < end:
                time.sleep(0.05)
            time.sleep(self.cfg['capture']['tail'])
        except Exception as exc:
            error = ''.join(traceback.format_exception(exc))
            from .display import screenshot
            try:
                screenshot(self.cfg, out / 'failure.png')
            except Exception:
                pass
        finally:
            take.stop()
        info['log'] = log
        info['error'] = error
        if self.rack:
            info['rack_fps'] = (self.rack.probe.layout() or {}).get('fps')
            info['probe_log'] = self.rack.probe.log()
        (out / 'take.json').write_text(json.dumps(info, indent=1))
        latest = out.parent / 'latest'
        if latest.is_symlink() or latest.exists():
            latest.unlink()
        latest.symlink_to(out.name)
        if error:
            raise RuntimeError(f'{shot.key}: take failed; {out}/take.json has the trace\n{error}')
        return out

    @property
    def take_dir_current(self):
        return getattr(self, '_take_dir_current', self.take_dir)

    @take_dir_current.setter
    def take_dir_current(self, v):
        self._take_dir_current = v


def _child_tty(pid: int) -> str | None:
    """The pts of the shell xterm runs (for the terminal slate)."""
    try:
        kids = subprocess.run(['pgrep', '-P', str(pid)], capture_output=True, text=True).stdout.split()
        for k in kids + [str(pid)]:
            fd0 = os.readlink(f'/proc/{k}/fd/0')
            if fd0.startswith('/dev/pts/'):
                return fd0
    except OSError:
        pass
    return None
