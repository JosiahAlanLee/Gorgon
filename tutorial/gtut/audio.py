"""The stage's private JACK server, port routing, and the sync slate click."""
from __future__ import annotations

import os
import re
import shutil
import subprocess
import threading
import time

from .procs import Procs, wait_until


def jack_env(cfg, base: dict | None = None) -> dict:
    env = dict(base or os.environ)
    env['JACK_DEFAULT_SERVER'] = cfg['audio']['server_name']
    env['JACK_NO_AUDIO_RESERVATION'] = '1'
    env['JACK_NO_START_SERVER'] = '1'
    if cfg['audio']['server'] == 'pipewire':
        env['PIPEWIRE_LATENCY'] = f"{cfg['audio']['period']}/{cfg['audio']['rate']}"
    return env


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
    wait_until(_server_ready, 20, what='JACK server')


def _server_ready():
    import jack
    with jack.Client('gtut_ready', no_start_server=True):
        return True


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
