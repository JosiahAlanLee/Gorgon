"""gtut: film the Gorgon ecosystem tutorial on this machine.

  gtut preflight                      what's missing, with fixes
  gtut setup [hydrapy|plugins|all]    HydraPy worktree at the pin; build and install the Rack plugins
  gtut catalog                        start Rack once and record every module's ports and params
  gtut patches                        build work/patches/*.vcv from tutorial/patches/*.py
  gtut harvest SAVED.vcv PLUGIN MODEL NAME   keep a module's GUI-set data (audio devices) for the builders
  gtut renders                        offline HydraPy renders for render shots and cards
  gtut check                          validate every chapter script
  gtut actions                        the action vocabulary
  gtut narrate [REF...] [--force]     speech, cue times and captions; --prompter for recorded narration
  gtut stage up [--needs rack,...]    bring the stage up and leave it (gtut serve holds it for `do`)
  gtut stage down | status | shot FILE.png
  gtut serve                          hold a stage and accept `gtut do` / `gtut shoot --attach`
  gtut do '{"rack.view": {"modules": ["Gorgon"]}}'
  gtut shoot REF [--attach]           record takes (REF: 05, 05:shot-id, 05:a,b)
  gtut qc take REF | qc video FILE
  gtut compose REF                    segments for shots
  gtut chapter NN                     join a chapter
  gtut final                          the whole tutorial, with chapter markers
"""
from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.request
from pathlib import Path

from . import config as config_mod

SERVE_PORT = 8791


def _script_dir(cfg) -> Path:
    return cfg.root / 'script'


def _post(path: str, body: dict):
    req = urllib.request.Request(f'http://127.0.0.1:{SERVE_PORT}{path}', data=json.dumps(body).encode(),
                                 headers={'Content-Type': 'application/json'}, method='POST')
    with urllib.request.urlopen(req, timeout=3600) as r:
        return json.loads(r.read())


def _served() -> bool:
    try:
        urllib.request.urlopen(f'http://127.0.0.1:{SERVE_PORT}/ping', timeout=1)
        return True
    except Exception:
        return False


def cmd_preflight(cfg, a):
    from . import preflight
    results = preflight.run(cfg)
    print(preflight.report(results))
    (cfg.work / 'preflight.json').write_text(json.dumps(results, indent=1))
    return 1 if any(r['level'] == 'FAIL' for r in results) else 0


def cmd_setup(cfg, a):
    from . import setup
    what = a.what or 'all'
    if what in ('hydrapy', 'all'):
        print('HydraPy at', setup.prepare_hydrapy(cfg))
    if what in ('plugins', 'all'):
        for p in setup.build_plugins(cfg, clean=a.clean):
            print('installed', p.name)


def cmd_catalog(cfg, a):
    from . import setup
    from .runner import Stage
    st = Stage(cfg)
    try:
        print(setup.catalog(cfg, st))
    finally:
        st.down()


def cmd_patches(cfg, a):
    from . import setup
    for p in setup.build_patches(cfg):
        print(p)


def cmd_renders(cfg, a):
    from . import script, setup
    for p in setup.renders(cfg, script.chapters(_script_dir(cfg))):
        print(p)


def cmd_harvest(cfg, a):
    """Copy one module's data block out of a patch saved from Rack (GUI-only state)."""
    from .vcvpatch import harvest
    data = harvest(Path(a.patch), a.plugin, a.model)
    if data is None:
        print(f'no {a.plugin}/{a.model} with data in {a.patch}', file=sys.stderr)
        return 1
    out = cfg.root / 'patches' / 'harvested' / f'{a.name}.json'
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(data, indent=1))
    print(out)
    print(json.dumps(data, indent=1)[:800])


def cmd_check(cfg, a):
    from . import script
    total = 0
    for ch in script.chapters(_script_dir(cfg)):
        words = sum(len(s.spoken().split()) for s in ch.shots)
        total += words
        print(f'{ch.name}: {len(ch.shots)} shots, {words} words (~{words / 150:.1f} min spoken)')
    print(f'total ~{total / 150:.0f} min of narration')


def cmd_actions(cfg, a):
    from .actions import ACTIONS
    for name in sorted(ACTIONS):
        print(f'{name:16} {ACTIONS[name].doc}')


def _refs(cfg, refs):
    from . import script
    if not refs:
        return [(ch, ch.shots) for ch in script.chapters(_script_dir(cfg))]
    return [script.find(_script_dir(cfg), r) for r in refs]


def cmd_narrate(cfg, a):
    from . import narrate, script
    if a.prompter:
        print(narrate.prompter(script.chapters(_script_dir(cfg)), cfg.work / 'narration' / 'PROMPTER.md'))
        return
    for ch, shots in _refs(cfg, a.refs):
        for s in shots:
            meta = narrate.narrate_shot(cfg, s, cfg.work / 'narration', force=a.force)
            cues = ', '.join(f'{k}@{v:.2f}' for k, v in meta['cues'].items())
            print(f"{s.key}: {meta['duration']:.1f}s [{meta['cue_method']}] {cues}")


def cmd_stage(cfg, a):
    from .runner import Stage
    from . import display
    if a.action == 'down':
        if _served():
            _post('/down', {})
        Stage(cfg).procs.stop_all(order=['terminal', 'rack', 'studio', 'jack', 'wm', 'display'])
    elif a.action == 'status':
        from .procs import Procs
        p = Procs(cfg.stage_dir())
        for name, info in p._load().items():
            print(f"{name:10} {'up' if p.alive(name) else 'DEAD'}  pid {info['pid']}  {' '.join(info['argv'])[:90]}")
    elif a.action == 'shot':
        print(display.screenshot(cfg, Path(a.arg or cfg.work / 'stage.png')))
    elif a.action == 'up':
        needs = set((a.needs or 'controller,program,rack,terminal').split(','))
        if _served():
            print(_post('/up', dict(needs=sorted(needs))))
        else:
            print('use `gtut serve` to hold a stage between commands')


def cmd_serve(cfg, a):
    from http.server import BaseHTTPRequestHandler, HTTPServer
    from . import display, narrate, script
    from .runner import Stage
    stage = Stage(cfg)

    class H(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def _reply(self, code, body):
            data = json.dumps(body).encode()
            self.send_response(code)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            self._reply(200, dict(ok=True, up=sorted(stage.up_services)))

        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers.get('Content-Length', 0))) or b'{}')
            try:
                if self.path == '/up':
                    stage.up(set(body.get('needs', [])))
                    out = dict(up=sorted(stage.up_services))
                elif self.path == '/do':
                    stage.do(body)
                    out = dict(ok=True)
                elif self.path == '/shoot':
                    ch, shots = script.find(_script_dir(cfg), body['ref'])
                    takes = []
                    for s in shots:
                        meta = narrate.narrate_shot(cfg, s, cfg.work / 'narration')
                        takes.append(str(stage.shoot(s, meta)))
                    out = dict(takes=takes)
                elif self.path == '/shot':
                    out = dict(path=str(display.screenshot(cfg, Path(body['path']))))
                elif self.path == '/layout':
                    out = stage.rack.probe.fresh() if stage.rack else {}
                elif self.path == '/down':
                    stage.down()
                    out = dict(ok=True)
                else:
                    return self._reply(404, dict(error='unknown'))
                self._reply(200, out)
            except Exception as exc:
                import traceback
                self._reply(500, dict(error=str(exc), trace=traceback.format_exc()))

    server = HTTPServer(('127.0.0.1', SERVE_PORT), H)
    # `gtut serve &` from a script or non-interactive shell starts with SIGINT ignored, so Python never
    # installs its Ctrl+C handler; SIGTERM would skip the teardown and leave the stage's processes up.
    # Both now stop serving and tear the stage down.
    import signal

    def stop(signum, frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    print(f'gtut serving on 127.0.0.1:{SERVE_PORT}; Ctrl+C (or kill) stops it and tears the stage down', flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        stage.down()


def cmd_do(cfg, a):
    if not _served():
        print('no gtut serve running', file=sys.stderr)
        return 2
    print(json.dumps(_post('/do', json.loads(a.json)), indent=1))


def cmd_shoot(cfg, a):
    from . import narrate, qc
    if a.attach or _served():
        for ref in a.refs:
            print(json.dumps(_post('/shoot', dict(ref=ref)), indent=1))
        return
    from .runner import Stage
    stage = Stage(cfg)
    failed = 0
    try:
        for ch, shots in _refs(cfg, a.refs):
            for s in shots:
                if s.kind != 'live':
                    continue
                meta = narrate.narrate_shot(cfg, s, cfg.work / 'narration')
                try:
                    take = stage.shoot(s, meta)
                    r = qc.take_report(cfg, take)
                    status = 'ok' if not r['problems'] else 'CHECK: ' + '; '.join(r['problems'])
                    print(f'{s.key}: {take}  {status}\n  contact sheet: {r["contact_sheet"]}')
                except Exception as exc:
                    failed += 1
                    print(f'{s.key}: FAILED {exc}', file=sys.stderr)
                    if a.stop:
                        raise
    finally:
        if not a.keep:
            stage.down()
    return 1 if failed else 0


def cmd_qc(cfg, a):
    from . import qc, script
    if a.what == 'take':
        for ch, shots in _refs(cfg, [a.target]):
            for s in shots:
                d = cfg.work / 'takes' / s.key / 'latest'
                if d.exists():
                    print(json.dumps(qc.take_report(cfg, d), indent=1))
    else:
        print(json.dumps(qc.video_report(cfg, Path(a.target), ocr=not a.no_ocr), indent=1))


def cmd_compose(cfg, a):
    from .compose import Composer
    comp = Composer(cfg)
    for ch, shots in _refs(cfg, a.refs):
        for s in shots:
            t = time.time()
            seg = comp.shot(s)
            print(f'{s.key}: {seg} ({time.time() - t:.0f}s)')


def cmd_chapter(cfg, a):
    from . import compose, qc, script
    for ref in a.refs:
        ch, _ = script.find(_script_dir(cfg), ref)
        out = compose.chapter(cfg, ch)
        r = qc.video_report(cfg, out)
        print(out)
        print('\n'.join(['problems:'] + r['problems'] or ['problems: none']))


def cmd_final(cfg, a):
    from . import compose, qc, script
    out = compose.tutorial(cfg, script.chapters(_script_dir(cfg)))
    r = qc.video_report(cfg, out)
    print(out)
    print(json.dumps(r, indent=1))


# Commands that start JACK clients in this process or on the stage (see audio.reexec_with_libjack).
JACK_COMMANDS = {'preflight', 'catalog', 'stage', 'serve', 'shoot', 'narrate'}


def main(argv=None):
    p = argparse.ArgumentParser(prog='gtut', description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--config', help='config.yaml (default tutorial/config.yaml or $GTUT_CONFIG)')
    sub = p.add_subparsers(dest='cmd', required=True)
    sub.add_parser('preflight')
    s = sub.add_parser('setup')
    s.add_argument('what', nargs='?', choices=['hydrapy', 'plugins', 'all'])
    s.add_argument('--clean', action='store_true')
    sub.add_parser('catalog')
    sub.add_parser('patches')
    s = sub.add_parser('harvest')
    s.add_argument('patch', help='a .vcv saved from the stage Rack (rack.save)')
    s.add_argument('plugin')
    s.add_argument('model')
    s.add_argument('name', help='patches/harvested/<name>.json')
    sub.add_parser('renders')
    sub.add_parser('check')
    sub.add_parser('actions')
    s = sub.add_parser('narrate')
    s.add_argument('refs', nargs='*')
    s.add_argument('--force', action='store_true')
    s.add_argument('--prompter', action='store_true')
    s = sub.add_parser('stage')
    s.add_argument('action', choices=['up', 'down', 'status', 'shot'])
    s.add_argument('arg', nargs='?')
    s.add_argument('--needs')
    sub.add_parser('serve')
    s = sub.add_parser('do')
    s.add_argument('json')
    s = sub.add_parser('shoot')
    s.add_argument('refs', nargs='+')
    s.add_argument('--attach', action='store_true')
    s.add_argument('--keep', action='store_true', help='leave the stage up afterwards')
    s.add_argument('--stop', action='store_true', help='stop at the first failed take')
    s = sub.add_parser('qc')
    s.add_argument('what', choices=['take', 'video'])
    s.add_argument('target')
    s.add_argument('--no-ocr', action='store_true')
    s = sub.add_parser('compose')
    s.add_argument('refs', nargs='+')
    s = sub.add_parser('chapter')
    s.add_argument('refs', nargs='+')
    sub.add_parser('final')
    a = p.parse_args(argv)
    cfg = config_mod.load(a.config)
    if argv is None and a.cmd in JACK_COMMANDS:
        from . import audio
        audio.reexec_with_libjack(cfg, sys.argv[1:])
    fn = globals()[f"cmd_{a.cmd}"]
    return fn(cfg, a) or 0


if __name__ == '__main__':
    sys.exit(main())
