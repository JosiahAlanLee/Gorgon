"""Isolated, wall-clock Rack/Studio capture. Never starts a hardware JACK driver."""
from __future__ import annotations

import copy
import io
import importlib.util
import json
import math
import os
from pathlib import Path
import queue
import re
import shutil
import signal
import subprocess
import sys
import tarfile
import threading
import time
import uuid

from . import audio, launcher
from .config import LaunchError, rack_user_dir, write_private


def commands(config, options, output, server):
    """Pure command construction, shared by tests and execution."""
    s = config['studio']
    studio = [*s['command'], 'studio', s['patch'], '--root', str(output / 'studio'),
              '--tracker', '--no-browser', '--port', '0', '--backend', s['backend'],
              '--size', options.get('size') or s['size'], '--fps', str(options.get('fps') or s['fps'])]
    monitor = options.get('native_output')
    studio += ['--native-output', monitor] if monitor else ['--no-native-output']
    studio += s['args']
    rack = [*config['rack']['command'], '-h', '-u', str(output / 'rack-user'),
            *config['rack']['args'], str(output / 'capture.vcv')]
    # Each headless process gets a free X server >=147; never inherit display :1.
    if not monitor:
        studio = ['xvfb-run', '-a', '-n', '147', *studio]
    rack = ['xvfb-run', '-a', '-n', '147', *rack]
    jack = ['jackd', '--no-realtime', '-t', str(options['jack_timeout']), '-n', server,
            '-d', 'dummy', '-r', str(config['audio']['rate']), '-p', str(config['audio']['quantum'])]
    return dict(studio=studio, rack=rack, jack=jack)


def validate(config, options):
    for key in ('seconds', 'warmup', 'jack_timeout'):
        value = options[key]
        if not math.isfinite(value) or value < (0 if key == 'warmup' else .001):
            raise LaunchError(f'{key} must be a finite positive number')
    for key in ('optical_gain', 'av_offset_ms'):
        if not math.isfinite(options[key]):
            raise LaunchError(f'{key} must be finite')
    if not 0 <= options['optical_gain'] <= 10 or not 0 <= options['lead_blocks'] <= 100:
        raise LaunchError('optical gain must be 0–10; lead blocks must be 0–100')
    if options.get('size') and not re.fullmatch(r'[1-9][0-9]*x[1-9][0-9]*', options['size']):
        raise LaunchError('size must be WIDTHxHEIGHT')
    if options.get('fps') is not None and not 1 <= options['fps'] <= 240:
        raise LaunchError('fps must be 1–240')
    if options.get('command') == 'live' and not options.get('native_output'):
        raise LaunchError('live requires --native-output MONITOR')
    if options.get('command') == 'render' and options.get('native_output'):
        raise LaunchError('render is offscreen; use live for fullscreen output')
    if options.get('hardware') and options.get('command') != 'live':
        raise LaunchError('Only live accepts --hardware')
    if options.get('jack_server') and not re.fullmatch(r'[A-Za-z0-9_-]+', options['jack_server']):
        raise LaunchError('JACK server name must contain letters, numbers, underscores or hyphens')
    if options.get('hardware') and not options.get('jack_server') and config['audio']['backend'] != 'pipewire':
        raise LaunchError('Native JACK --hardware requires --jack-server NAME; start the real device server yourself')
    if not config['rack']['patch']:
        raise LaunchError('Capture requires rack.patch with a Core Audio-16 module wired to mix/stems')
    for source in options.get('source', []):
        if ':' not in source:
            raise LaunchError('--source must be an exact JACK port name')
    if options.get('sync_gate_channel') is not None and not 1 <= options['sync_gate_channel'] <= 16:
        raise LaunchError('sync gate channel must be 1–16')
    if not math.isfinite(options.get('sync_delay_ms', 0)):
        raise LaunchError('sync delay must be finite')
    if options.get('source') and len(options['source']) != 16:
        raise LaunchError('Supply exactly sixteen --source ports, in capture channel order')


def invoke(config, options):
    validate(config, options)
    missing = [name for name in ('jack', 'numpy', 'soundfile') if importlib.util.find_spec(name) is None]
    if missing:
        raise LaunchError('Install gorgon-live[capture]; missing: ' + ', '.join(missing))
    required = ('xvfb-run', 'ffmpeg', 'ffprobe') + (() if options.get('hardware') else ('jackd',))
    for tool in required:
        if not shutil.which(tool):
            raise LaunchError(f'Capture requires {tool} on PATH')
    config = copy.deepcopy(config)
    if options.get('audio_device'):
        config['audio']['device'] = options['audio_device']
    pipewire = options.get('hardware') and not options.get('jack_server') and config['audio']['backend'] == 'pipewire'
    # Load the selected JACK implementation before importing its Python bindings.
    if pipewire:
        config['audio']['start_services'] = False
        audio.ensure_server(config)
        env = dict(audio.environment(config), PYTHONDONTWRITEBYTECODE='1')
    else:
        env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', LD_LIBRARY_PATH='/lib/x86_64-linux-gnu', JACK_NO_START_SERVER='1')
    command = [sys.executable, '-m', 'gorgon.capture', json.dumps(config), json.dumps(options, default=str)]
    return subprocess.call(audio.wrap(config, command) if pipewire else command, env=env)


def read_patch(path):
    raw = Path(path).read_bytes()
    if raw[:4] == b'\x28\xb5\x2f\xfd':
        import zstandard
        with zstandard.ZstdDecompressor().stream_reader(io.BytesIO(raw)) as reader:
            with tarfile.open(fileobj=io.BytesIO(reader.read())) as archive:
                member = archive.extractfile('patch.json')
                if member is None:
                    raise LaunchError('Rack archive has no patch.json')
                return json.load(member)
    return json.loads(raw)


def write_patch(source, document, target):
    """Retain embedded assets without extracting untrusted archive paths."""
    raw = Path(source).read_bytes()
    if raw[:4] != b'\x28\xb5\x2f\xfd':
        write_private(target, document)
        return
    import zstandard
    with zstandard.ZstdDecompressor().stream_reader(io.BytesIO(raw)) as reader:
        original = reader.read()
    result = io.BytesIO()
    with tarfile.open(fileobj=io.BytesIO(original)) as archive, tarfile.open(fileobj=result, mode='w') as output:
        for member in archive.getmembers():
            member = copy.copy(member)
            if member.name == 'patch.json':
                data = json.dumps(document, allow_nan=False).encode()
                member.size = len(data)
                output.addfile(member, io.BytesIO(data))
            else:
                output.addfile(member, archive.extractfile(member) if member.isfile() else None)
    with Path(target).open('xb') as stream:
        stream.write(zstandard.ZstdCompressor().compress(result.getvalue()))


def prepare_patch(document, studio_client, capture_client, explicit_sources=False, calibrate=False):
    document = copy.deepcopy(document)
    captures = []
    for module in document['modules']:
        if module.get('plugin') == 'GorgonBridge' and module.get('model') == 'Gorgon':
            data = module.setdefault('data', {})
            data.setdefault('audio', {}).update(driver=4, deviceName=studio_client)
            if calibrate:
                data['laneOffsetMs'] = [0] * 6
        if module.get('plugin') == 'Core' and module.get('model') == 'AudioInterface16':
            captures.append(module)
    if not explicit_sources:
        if len(captures) != 1:
            raise LaunchError('Patch must have exactly one Core Audio-16, or supply sixteen --source ports')
        captures[0].setdefault('data', {}).setdefault('audio', {}).update(driver=4, deviceName=capture_client,
                                                                       inputOffset=0, outputOffset=0)
    return document


class Recorder:
    """Bounded callback queue, streamed to a float WAV outside the JACK callback."""
    def __init__(self, jack, path, monitor=False):
        import numpy as np
        import soundfile as sf
        self.client = jack.Client('gorgon_capture', no_start_server=True, use_exact_name=True)
        self.inputs = [self.client.inports.register(f'in{i:02}') for i in range(1, 17)]
        self.outputs = [self.client.outports.register(f'out{i:02}') for i in range(1, 17)]
        self.monitors = [self.client.outports.register('monitor_' + side) for side in ('L', 'R')] if monitor else []
        self.queue = queue.Queue(maxsize=256)
        self.enabled = False
        self.error = None
        self.xruns = 0
        self.first_time = None
        self.blocks = 0
        self.clock = []
        self.samples = 0
        self.writer = sf.SoundFile(path, mode='x', samplerate=self.client.samplerate, channels=16, subtype='FLOAT')
        @self.client.set_process_callback
        def process(frames):
            for port in self.outputs:
                port.get_array().fill(0)
            for source, target in zip(self.inputs, self.monitors):
                target.get_array()[:] = source.get_array()
            if self.enabled:
                try:
                    self.queue.put_nowait((time.monotonic(), self.client.last_frame_time, np.stack([p.get_array() for p in self.inputs], axis=1)))
                except queue.Full:
                    self.error = 'Recorder queue overflow; capture is incomplete'
        @self.client.set_xrun_callback
        def xrun(delay):
            self.xruns += 1
        self.thread = threading.Thread(target=self._write, daemon=True)
        self.thread.start()
        self.client.activate()

    def _write(self):
        try:
            while True:
                item = self.queue.get()
                if item is None:
                    break
                stamp, frame, block = item
                self.clock.append((self.samples, frame, stamp))
                self.samples += len(block)
                if self.first_time is None:
                    self.first_time = stamp
                self.writer.write(block)
                self.blocks += 1
        except Exception as exc:
            self.error = str(exc)

    def close(self):
        self.enabled = False
        self.client.deactivate()
        self.client.close()
        while self.thread.is_alive():
            try:
                self.queue.put(None, timeout=.1)
                break
            except queue.Full:
                continue
        self.thread.join(timeout=10)
        self.writer.close()
        if self.thread.is_alive():
            raise LaunchError('Recorder writer did not finish')
        if self.error:
            raise LaunchError(self.error)


def run(config, options):
    validate(config, options)
    import jack
    import soundfile as sf
    output = Path(options['output']).expanduser().resolve()
    root = Path(config['workspace'])
    if output.is_relative_to(root):
        raise LaunchError('Capture output must be outside the Studio workspace')
    output.mkdir(parents=True, exist_ok=False)
    children, logs = [], []
    recorder = None
    stopped = threading.Event()
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, lambda *_: stopped.set())
    private = not options.get('hardware')
    pipewire = not private and not options.get('jack_server') and config['audio']['backend'] == 'pipewire'
    server = options.get('jack_server') or ('pipewire' if pipewire else 'gorgon_' + uuid.uuid4().hex[:12])
    env = dict(os.environ, JACK_DEFAULT_SERVER=server, JACK_NO_START_SERVER='1',
               LD_LIBRARY_PATH='/lib/x86_64-linux-gnu', HYDRAPY_STUDIO_FILE=str(output / 'studio.json'))
    if pipewire:
        env = dict(audio.environment(config), HYDRAPY_STUDIO_FILE=str(output / 'studio.json'))
    else:
        os.environ.update(JACK_DEFAULT_SERVER=server, JACK_NO_START_SERVER='1')
    cmd = commands(config, options, output, server)
    def spawn(name, command, cwd=None):
        log = open(output / f'{name}.log', 'xb')
        logs.append(log)
        app_env = audio.rack_environment(config, env) if pipewire and name == 'rack' else env
        p = subprocess.Popen(command, cwd=cwd, env=app_env, stdin=subprocess.PIPE,
                             stdout=log, stderr=log, start_new_session=True)
        children.append((name, p))
        return p
    def wait_for(callback, what, timeout=None):
        deadline = time.monotonic() + (timeout or config['timeout'])
        last = None
        while time.monotonic() < deadline:
            if stopped.is_set():
                raise LaunchError('Capture interrupted')
            for name, child in children:
                if child.poll() is not None:
                    raise LaunchError(f'{name} exited ({child.returncode}); see {output}/{name}.log')
            try:
                result = callback()
                if result:
                    return result
            except (OSError, ValueError, KeyError, jack.JackError) as exc:
                last = exc
            stopped.wait(.1)
        raise LaunchError(f'Timed out waiting for {what}: {last or "not ready"}')
    def pause(seconds):
        until = time.monotonic() + seconds
        wait_for(lambda: time.monotonic() >= until, 'capture duration', seconds + 5)
    try:
        # Work only in copies: Studio settings, exports, autosave, Rack plugin cache.
        shutil.copytree(root, output / 'studio', ignore=shutil.ignore_patterns('.git', 'exports', '__pycache__'))
        plugins = output / 'rack-user' / 'plugins-lin-x64'
        source_plugins = rack_user_dir(config) / 'plugins-lin-x64'
        if source_plugins.exists():
            shutil.copytree(source_plugins, plugins)
        else:
            plugins.mkdir(parents=True)
        for source in options.get('plugin_dir', []):
            source = Path(source).expanduser().resolve()
            slug = json.loads((source / 'plugin.json').read_text())['slug']
            if not isinstance(slug, str) or not re.fullmatch(r'[A-Za-z0-9_-]+', slug):
                raise LaunchError('Plugin slug must be a simple directory name')
            target = plugins / slug
            if target.exists():
                shutil.rmtree(target)  # created by this run's copytree
            target.mkdir()
            for name in ('plugin.json', 'plugin.so', 'res', 'presets'):
                item = source / name
                if item.is_dir():
                    shutil.copytree(item, target / name)
                elif item.exists():
                    shutil.copy2(item, target / name)
        write_private(output / 'studio' / '.hydrapy' / 'outputs.json',
                      dict(audio=dict(enabled=True, optical_gain=options['optical_gain'], breaks_gain=0.,
                                      lead_blocks=options['lead_blocks'])))
        if private:
            try:
                existing = jack.Client('gorgon_collision_check', no_start_server=True)
            except jack.JackError:
                existing = None
            if existing is not None:
                existing.close()
                raise LaunchError('JACK server name already exists; choose a new private name, or explicitly use live --hardware')
            spawn('jack', cmd['jack'])
        def jack_ready():
            with jack.Client('gorgon_probe', no_start_server=True):
                return True
        wait_for(jack_ready, 'private JACK server')
        recorder = Recorder(jack, str(output / 'capture.wav'), monitor=not private)
        studio_process = spawn('studio', cmd['studio'], output / 'studio')
        def studio_ready():
            info = json.loads((output / 'studio.json').read_text())
            if Path(info['root']).resolve() != output / 'studio' or os.getpgid(info['pid']) != studio_process.pid:
                raise LaunchError('Studio advertisement does not belong to this capture')
            current = launcher.api(info, 'tracker')
            sound = current.get('audio', {})
            if current.get('error') or sound.get('error'):
                raise LaunchError('Studio: ' + str(current.get('error') or sound['error']))
            if current.get('path') and sound.get('active') and len(sound.get('ports', [])) == 4:
                return info, sound['client'], current
        info, studio_client, current = wait_for(studio_ready, 'Studio tracker and optical ports')
        if options['command'] == 'measure-latency':
            from .latency import probe_score
            score = probe_score()
            base = json.loads(Path(options['score']).expanduser().read_text()) if options.get('score') else current['document']
            if isinstance(base.get('sounds'), list):
                for lane, sound in enumerate(base['sounds'][:6]):
                    score['sounds'][lane] = copy.deepcopy(sound)
        else:
            score = json.loads(Path(options['score']).expanduser().read_text())
        launcher.api(info, 'tracker_transport', dict(enabled=True, playing=False))
        launcher.api(info, 'tracker_score', dict(document=score, revision=current['revision']))
        document = prepare_patch(read_patch(config['rack']['patch']), studio_client, recorder.client.name,
                                 bool(options.get('source')), options['command'] == 'measure-latency')
        write_patch(config['rack']['patch'], document, output / 'capture.vcv')
        rack_exe = launcher.executable(config['rack']['command'])
        spawn('rack', cmd['rack'], Path(rack_exe).parent)
        for pair in config['audio']['connections'] + options.get('link', []):
            a, b = [p.replace('{studio}', studio_client).replace('{capture}', recorder.client.name) for p in pair]
            def connect(a=a, b=b):
                if b not in [p.name for p in recorder.client.get_all_connections(a)]:
                    recorder.client.connect(a, b)
                return True
            wait_for(connect, f'JACK link {a} -> {b}')
        if options.get('source'):
            for source, port in zip(options['source'], recorder.inputs):
                wait_for(lambda source=source, port=port: recorder.client.connect(source, port) or True, source)
        wait_for(lambda: all(recorder.client.get_all_connections(p) for p in recorder.inputs), '16 capture channels')
        device = None
        if not private:
            graph_config = dict(config, audio=dict(config['audio'], backend='pipewire' if pipewire else 'jack'))
            graph = audio.probe(graph_config)
            device = audio.playback_device(graph_config, graph)
            targets = [p['name'] for p in graph['ports'] if p['flags'] & 1 and p['name'].split(':')[0] == device]
            if len(targets) < 2:
                raise LaunchError('Playback device needs two JACK input ports')
            for source, target in zip(recorder.monitors, targets[:2]):
                recorder.client.connect(source, target)
        pause(options['warmup'])
        video = options['command'] != 'measure-latency'
        recorder.enabled = True
        record_time = time.monotonic()
        if video:
            launcher.api(info, 'record', {})
        play_time = time.monotonic()
        launcher.api(info, 'tracker_transport', dict(enabled=True, playing=True, beat=0))
        pause(options['seconds'])
        launcher.api(info, 'tracker_transport', dict(playing=False))
        if video:
            launcher.api(info, 'record', {})
        recorder.close()
        import numpy as np
        if len(recorder.clock) < 2:
            raise LaunchError('JACK delivered fewer than two recorded audio blocks')
        stamps = np.array(recorder.clock)
        np.savez_compressed(output / 'capture-clock.npz', samples=stamps[:, 0], jack_frames=stamps[:, 1],
                            monotonic=stamps[:, 2])
        discontinuities = int(np.count_nonzero(np.diff(stamps[:, 1]) != np.diff(stamps[:, 0])))
        report = dict(server=server, private=private, playback_device=device, discontinuities=discontinuities, record_monotonic=record_time, play_monotonic=play_time,
                      audio_first_callback=recorder.first_time, xruns=recorder.xruns, blocks=recorder.blocks,
                      synchronization='monotonic estimate; video API/encoder startup latency is not measured')
        recorder = None
        write_private(output / 'capture.json', report)
        if report['xruns'] or discontinuities:
            print(f"Audio warning: {report['xruns']} xruns, {discontinuities} recorded discontinuities; inspect capture.json", file=sys.stderr)
        if options['command'] == 'measure-latency':
            if discontinuities:
                raise LaunchError('Latency capture has discontinuities; increase the JACK quantum and repeat')
            capture, rate = sf.read(output / 'capture.wav', dtype='float32', always_2d=True)
            from .latency import measure
            write_private(output / 'latency.json', measure(capture, rate))
        else:
            from contextlib import ExitStack
            stems = output / 'stems'
            stems.mkdir()
            with ExitStack() as stack:
                source = stack.enter_context(sf.SoundFile(output / 'capture.wav'))
                mix = stack.enter_context(sf.SoundFile(output / 'mix.wav', mode='x', samplerate=source.samplerate,
                                                       channels=2, subtype='FLOAT'))
                writers = [stack.enter_context(sf.SoundFile(stems / f'channel-{ch + 1:02}.wav', mode='x',
                            samplerate=source.samplerate, channels=1, subtype='FLOAT')) for ch in range(2, 16)]
                for block in source.blocks(blocksize=65536, dtype='float32', always_2d=True):
                    mix.write(block[:, :2])
                    for ch, writer in enumerate(writers, 2):
                        writer.write(block[:, ch])
            exports = output / 'studio' / '.hydrapy' / 'exports'
            def video_ready():
                for path in sorted(exports.glob('*.mp4'), key=lambda p: p.stat().st_mtime, reverse=True):
                    if subprocess.run(['ffprobe', '-v', 'error', str(path)], capture_output=True).returncode == 0:
                        return path
            movie = wait_for(video_ready, 'Studio video export', 300)
            shutil.copy2(movie, output / 'studio.mp4')
            offset = record_time - report['audio_first_callback']
            if options.get('sync_gate_channel'):
                from .avsync import align
                # Only the gate channel is needed for alignment, not the complete 16-channel take.
                with sf.SoundFile(output / 'capture.wav') as stream:
                    import numpy as np
                    gate = np.concatenate([b[:, options['sync_gate_channel'] - 1] for b in
                                           stream.blocks(blocksize=65536, dtype='float32', always_2d=True)])
                    offset, alignment = align(movie, gate[:, None], stream.samplerate, 1, record_time,
                                              report['audio_first_callback'], options.get('sync_delay_ms', 0))
                alignment['gate_channel'] = options['sync_gate_channel']
                write_private(output / 'av-sync.json', alignment)
            offset += options['av_offset_ms'] / 1000
            subprocess.run(['ffmpeg', '-v', 'error', '-n', '-i', str(output / 'studio.mp4'),
                            '-ss', str(max(offset, 0)), '-i', str(output / 'mix.wav'),
                            '-map', '0:v:0', '-map', '1:a:0', '-c:v', 'copy', '-c:a', 'aac',
                            '-af', f'adelay={max(-offset, 0) * 1000}:all=1', '-shortest', str(output / 'render.mp4')], check=True)
        print(f'Capture: {output}')
        return 0
    finally:
        if recorder:
            try:
                recorder.close()
            except Exception as exc:
                print(f'Recorder cleanup: {exc}', file=sys.stderr)
        for name, p in reversed(children):
            if p.poll() is None:
                if p.stdin:
                    p.stdin.close()
                launcher._terminate(p.pid, signal.SIGTERM)
        for name, p in reversed(children):
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                launcher._terminate(p.pid, signal.SIGKILL)
                p.wait(timeout=5)
        for log in logs:
            log.close()


if __name__ == '__main__':
    try:
        sys.exit(run(json.loads(sys.argv[1]), json.loads(sys.argv[2])))
    except (LaunchError, OSError, ValueError, subprocess.SubprocessError) as exc:
        print(f'gorgon capture: {exc}', file=sys.stderr)
        sys.exit(1)
