"""The stage's private JACK server, port routing, and the sync slate click."""
from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

from .procs import Procs, wait_until


JACK_LIBS = ('libjack.so.0', 'libjackserver.so.0', 'libjacknet.so.0')


def real_jack_libs(ldconfig_text: str | None = None) -> dict[str, str]:
    """For each JACK library soname, the first entry in `ldconfig -p` that is not PipeWire's."""
    if ldconfig_text is None:
        try:
            ldconfig_text = subprocess.run(['ldconfig', '-p'], capture_output=True, text=True).stdout
        except OSError:
            return {}
    found = {}
    for line in ldconfig_text.splitlines():
        name = line.strip().split(' ', 1)[0]
        if name in JACK_LIBS and name not in found and '=>' in line:
            path = line.split('=>', 1)[1].strip()
            if 'pipewire' not in path:
                found[name] = path
    return found


def real_libjack(ldconfig_text: str | None = None) -> str | None:
    return real_jack_libs(ldconfig_text).get('libjack.so.0')


def libjack_dir(cfg) -> str | None:
    """In jackd mode, a private directory holding only links to the real JACK libraries, to put first on
    LD_LIBRARY_PATH for the stage's jackd and every JACK client. Where PipeWire's libraries are the system
    default (pipewire-jack ships libjack and libjackserver), a client ignores JACK_DEFAULT_SERVER and joins
    the desktop session, and jackd1 cannot even start. Links, not the whole system library directory, so
    nothing else a process loads changes."""
    a = cfg['audio']
    want = a.get('libjack', 'auto')
    if a['server'] != 'jackd' or want in (None, 'system'):
        return None
    if want == 'auto':
        libs = real_jack_libs()
    else:  # a path to libjack.so.0; its siblings come from the same directory
        lib = Path(os.path.expanduser(str(want)))
        libs = {n: str(lib if n == 'libjack.so.0' else lib.parent / n) for n in JACK_LIBS}
    libs = {n: p for n, p in libs.items() if os.path.exists(p)}
    if 'libjack.so.0' not in libs:
        return None
    d = cfg.stage_dir() / 'libjack'
    d.mkdir(exist_ok=True)
    for name, target in libs.items():
        link = d / name
        if not link.is_symlink() or os.readlink(link) != target:
            tmp = d / f'{name}.tmp'
            if tmp.is_symlink() or tmp.exists():
                tmp.unlink()
            tmp.symlink_to(target)
            tmp.replace(link)
    return str(d)


def _first_on_path(value: str, d: str) -> str:
    return ':'.join([d, *[p for p in value.split(':') if p and p != d]])


def jack_env(cfg, base: dict | None = None) -> dict:
    env = dict(base or os.environ)
    env['JACK_DEFAULT_SERVER'] = cfg['audio']['server_name']
    env['JACK_NO_AUDIO_RESERVATION'] = '1'
    env['JACK_NO_START_SERVER'] = '1'
    d = libjack_dir(cfg)
    if d:
        env['LD_LIBRARY_PATH'] = _first_on_path(env.get('LD_LIBRARY_PATH', ''), d)
    if cfg['audio']['server'] == 'pipewire':
        env['PIPEWIRE_LATENCY'] = f"{cfg['audio']['period']}/{cfg['audio']['rate']}"
    return env


def reexec_with_libjack(cfg, argv: list[str]):
    """gtut's own JACK clients (slate, port queries) load libjack in this process, and the loader reads
    LD_LIBRARY_PATH only at startup: restart under the stage's libjack directory if it isn't first yet."""
    d = libjack_dir(cfg)
    if not d or os.environ.get('LD_LIBRARY_PATH', '').split(':')[0] == d:
        return
    env = dict(os.environ)
    env['LD_LIBRARY_PATH'] = _first_on_path(env.get('LD_LIBRARY_PATH', ''), d)
    os.execve(sys.executable, [sys.executable, '-m', 'gtut.cli', *argv], env)


ISOLATION_PROBE = r'''
import json, sys
try:
    import jack
except Exception as exc:
    print(json.dumps(dict(error=f'{type(exc).__name__}: {exc}'))); sys.exit()
lib = next((l.split()[-1] for l in open('/proc/self/maps') if 'libjack' in l), None)
try:
    with jack.Client('gtut_isolation_probe', no_start_server=True) as c:  # registers no ports
        names = [p.name for p in c.get_ports()]
        print(json.dumps(dict(connected=True, libjack=lib, ports=len(names), sample=names[:4])))
except jack.JackError:
    print(json.dumps(dict(connected=False, libjack=lib)))
'''


def isolation_probe(cfg) -> dict:
    """Start a JACK client exactly as the stage starts its clients, but aimed at a server name that does
    not exist (and, for pipewire, an empty runtime directory). It must fail to connect: if it connects,
    stage clients can reach a server that isn't the stage's. The probe registers no ports."""
    import json
    import tempfile
    import uuid
    env = jack_env(cfg)
    env['JACK_DEFAULT_SERVER'] = f'gtut-isolation-{uuid.uuid4().hex[:8]}'
    with tempfile.TemporaryDirectory() as empty:
        if cfg['audio']['server'] == 'pipewire':
            env['XDG_RUNTIME_DIR'] = env['PIPEWIRE_RUNTIME_DIR'] = empty
        r = subprocess.run(wrap(cfg, [sys.executable, '-c', ISOLATION_PROBE]), capture_output=True, text=True,
                           env=env, timeout=30)
    lines = [l for l in r.stdout.splitlines() if l.startswith('{')]
    return json.loads(lines[-1]) if lines else dict(error=(r.stderr or r.stdout).strip()[-300:])


def wrap(cfg, argv: list) -> list:
    """Prefix a JACK client command so it reaches the stage's server."""
    if cfg['audio']['server'] == 'pipewire' and shutil.which('pw-jack'):
        return ['pw-jack', *argv]
    return list(argv)


def start_server(cfg, procs: Procs):
    a = cfg['audio']
    os.environ.update(jack_env(cfg))  # this process's own clients (slate, port queries)
    if a['server'] == 'jackd':
        procs.start('jack', ['jackd', '-n', a['server_name'], '--no-realtime', '-d', 'dummy',
                             '-r', a['rate'], '-p', a['period']], env=jack_env(cfg))
    elif a['server'] == 'pipewire':
        runtime = cfg.stage_dir() / 'xdg-runtime'
        runtime.mkdir(mode=0o700, exist_ok=True)
        env = jack_env(cfg)
        env['XDG_RUNTIME_DIR'] = str(runtime)
        os.environ['XDG_RUNTIME_DIR'] = str(runtime)
        procs.start('jack', ['pipewire'], env=env)
    else:
        raise ValueError(f"unknown audio.server {a['server']!r}")
    check_own_server(wait_until(_server_ready, 20, what='JACK server'))


def _server_ready():
    import jack
    with jack.Client('gtut_ready', no_start_server=True) as c:
        return [p.name for p in c.get_ports()] or ['(no ports)']


def check_own_server(names: list[str]):
    """A fresh stage server has only its dummy driver's system ports. Anything else means this process
    reached some other server (a desktop session): stop before anything plays into it."""
    foreign = [n for n in names if n != '(no ports)' and not n.startswith('system:')]
    if foreign:
        raise RuntimeError(f'the JACK server this stage reached is not its own (it has {foreign[:4]}); '
                           'check audio.libjack and run gtut preflight')


def ports(output: bool | None = None, audio: bool = True) -> list[str]:
    import jack
    with jack.Client('gtut_query', no_start_server=True) as c:
        kw = {'is_audio': audio}
        if output is not None:
            kw['is_output' if output else 'is_input'] = True
        return [p.name for p in c.get_ports(**kw)]


def connect(src: str, dst: str):
    import jack
    with jack.Client('gtut_connect', no_start_server=True) as c:
        try:
            c.connect(src, dst)
        except jack.JackErrorCode as exc:
            if 'already' not in str(exc).lower() and exc.code != 17:  # EEXIST
                raise


def resolve_record_ports(cfg) -> list[str]:
    """The programme's left and right source ports (config audio.record)."""
    rec = cfg['audio']['record']
    outs = ports(output=True)
    if rec == 'auto':
        found = [p for p in outs if re.search(r'rack|rtapi', p.split(':')[0], re.I)]
        if len(found) < 2:
            raise RuntimeError(f'no Rack output ports on the stage JACK server; saw {outs}. '
                               "Is the patch's Audio module on the JACK driver and running?")
        return found[:2]
    chosen = []
    for pattern in rec:
        m = [p for p in outs if re.search(pattern, p)]
        if not m:
            raise RuntimeError(f'audio.record pattern {pattern!r} matched none of {outs}')
        chosen.append(m[0])
    return chosen


class Slate:
    """A JACK client that writes a 2 ms full-scale burst when fired.

    The runner fires it together with the Rack probe's white flash; compose finds both
    in the take and aligns picture and sound to them.
    """

    def __init__(self):
        import jack
        self.client = jack.Client('gtut_slate', no_start_server=True)
        self.port = self.client.outports.register('click')
        self._pending = threading.Event()
        self._remaining = 0
        self.fired_frame = None

        @self.client.set_process_callback
        def process(frames):
            buf = self.port.get_array()
            buf[:] = 0
            if self._pending.is_set() and self._remaining == 0:
                self._pending.clear()
                self._remaining = int(self.client.samplerate * 0.002)
                self.fired_frame = self.client.last_frame_time
            if self._remaining:
                n = min(frames, self._remaining)
                buf[:n] = 1.0
                self._remaining -= n

        self.client.activate()

    @property
    def name(self) -> str:
        return self.port.name

    def fire(self):
        self._pending.set()

    def close(self):
        self.client.deactivate()
        self.client.close()
