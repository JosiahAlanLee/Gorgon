"""An explicit, user-editable launch profile. No shell commands or global audio edits."""
from __future__ import annotations

import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import shutil
import sys


class LaunchError(RuntimeError):
    pass


def xdg(name, fallback):
    return Path(os.environ.get(name) or Path.home() / fallback).expanduser().resolve()


def default_config():
    return xdg('XDG_CONFIG_HOME', '.config') / 'gorgon' / 'launch.json'


def discover_rack():
    candidates = [shutil.which('Rack'), shutil.which('rack')]
    candidates += [str(Path.home() / p) for p in ('opt/Rack2Free/Rack', 'opt/Rack2/Rack', 'Rack2Free/Rack', 'Rack2/Rack')]
    candidates += ['/opt/Rack2Free/Rack', '/opt/Rack2/Rack']
    return next((p for p in candidates if p and Path(p).is_file() and os.access(p, os.X_OK)), 'Rack')


def hydrapy_command():
    # The environment the launcher is installed in comes first: PATH may name another one's hydrapy.
    try:
        if importlib.util.find_spec('hydrapy'):
            return [sys.executable, '-m', 'hydrapy.cli']
    except (ImportError, ValueError):
        pass
    return [shutil.which('hydrapy') or 'hydrapy']


def defaults():
    return dict(format='gorgon-launch-1',
                workspace=str(xdg('XDG_DATA_HOME', '.local/share') / 'gorgon' / 'session'),
                browser=True, timeout=90,
                studio=dict(command=hydrapy_command(), patch='gorgon-visual.py', backend='auto',
                            size='1280x720', fps=30, native_output='window', args=[]),
                rack=dict(command=[discover_rack()], patch=None, user_dir=None, args=[]),
                capture=dict(prewarm=True),
                audio=dict(backend='pipewire', rate=48000, quantum=1024, device=None,
                           start_services=True, connections=[]))


def _fields(value, allowed, where):
    if not isinstance(value, dict) or value.keys() - set(allowed):
        raise LaunchError(f'Unknown or invalid {where} fields; expected {", ".join(allowed)}')


def _argv(value, name, empty=False):
    if not isinstance(value, list) or (not value and not empty) or any(not isinstance(x, str) or not x or '\0' in x for x in value):
        raise LaunchError(f'{name} must be an array of command arguments (no shell)')


def _path(value, base):
    return (base / Path(value).expanduser()).resolve()


def load(path=None):
    path = Path(path or default_config()).expanduser().resolve()
    config = defaults()
    if path.exists():
        try:
            supplied = json.loads(path.read_text())
        except (OSError, ValueError) as exc:
            raise LaunchError(f'Cannot read {path}: {exc}') from exc
        _fields(supplied, config, 'profile')
        for key in ('studio', 'rack', 'capture', 'audio'):
            value = supplied.get(key, {})
            _fields(value, config[key], key)
            config[key].update(value)
        config.update({k: v for k, v in supplied.items() if k not in ('studio', 'rack', 'capture', 'audio')})
    if config['format'] != 'gorgon-launch-1':
        raise LaunchError('Expected profile format gorgon-launch-1')
    if not isinstance(config['workspace'], str) or not config['workspace']:
        raise LaunchError('workspace must be a directory path')
    if type(config['browser']) is not bool:
        raise LaunchError('browser must be true or false')
    timeout = config['timeout']
    if isinstance(timeout, bool) or not isinstance(timeout, (int, float)) or not math.isfinite(timeout) or not 1 <= timeout <= 300:
        raise LaunchError('timeout must be 1–300 seconds')
    studio, rack, capture, audio = (config[k] for k in ('studio', 'rack', 'capture', 'audio'))
    for name, value in [('studio.command', studio['command']), ('rack.command', rack['command'])]:
        _argv(value, name)
    for name, value in [('studio.args', studio['args']), ('rack.args', rack['args'])]:
        _argv(value, name, empty=True)
    if studio['backend'] not in ('auto', 'cpu', 'gl'):
        raise LaunchError('studio.backend must be auto, cpu or gl')
    if not isinstance(studio['patch'], str) or not studio['patch']:
        raise LaunchError('studio.patch must name a visual patch')
    if not isinstance(studio['size'], str) or not re.fullmatch(r'[1-9][0-9]*x[1-9][0-9]*', studio['size']):
        raise LaunchError('studio.size must be WIDTHxHEIGHT')
    fps = studio['fps']
    if isinstance(fps, bool) or not isinstance(fps, (int, float)) or not math.isfinite(fps) or not 1 <= fps <= 240:
        raise LaunchError('studio.fps must be 1–240')
    if studio['native_output'] is not False and (not isinstance(studio['native_output'], str) or not studio['native_output']):
        raise LaunchError('studio.native_output must be false, window or a monitor name/index')
    if audio['backend'] not in ('pipewire', 'jack'):
        raise LaunchError('audio.backend must be pipewire or jack (an existing native JACK server)')
    for key, low, high in [('rate', 8000, 192000), ('quantum', 16, 8192)]:
        if type(audio[key]) is not int or not low <= audio[key] <= high:
            raise LaunchError(f'audio.{key} must be an integer in {low}–{high}')
    if type(audio['start_services']) is not bool:
        raise LaunchError('audio.start_services must be true or false')
    if type(capture['prewarm']) is not bool:
        raise LaunchError('capture.prewarm must be true or false')
    if audio['device'] is not None and (not isinstance(audio['device'], str) or not audio['device']):
        raise LaunchError('audio.device must be null or a JACK playback device name')
    if not isinstance(audio['connections'], list):
        raise LaunchError('audio.connections must be arrays of [source, destination] port names')
    for pair in audio['connections']:
        if not isinstance(pair, list) or len(pair) != 2 or any(not isinstance(p, str) or ':' not in p for p in pair):
            raise LaunchError('audio.connections must be arrays of [source, destination] port names')
    root = _path(config['workspace'], path.parent)
    patch = _path(studio['patch'], root)
    if not patch.is_relative_to(root):
        raise LaunchError('studio.patch must be inside workspace')
    for key in ('patch', 'user_dir'):
        if rack[key] is not None:
            if not isinstance(rack[key], str) or not rack[key]:
                raise LaunchError(f'rack.{key} must be null or a path')
            rack[key] = str(_path(rack[key], path.parent))
    # Resolve executable paths against the profile, while leaving PATH names intact. Symlinks stay:
    # a virtual environment's bin/python is one, and its target runs outside the environment.
    for app in (studio, rack):
        if '/' in app['command'][0]:
            app['command'][0] = os.path.abspath(path.parent / Path(app['command'][0]).expanduser())
    config['workspace'] = str(root)
    config['_path'] = str(path)
    return config


def run_dir(config):
    base = os.environ.get('XDG_RUNTIME_DIR')
    base = Path(base) if base else xdg('XDG_STATE_HOME', '.local/state')
    key = hashlib.sha256(config['_path'].encode()).hexdigest()[:16]
    return base / 'gorgon' / key


def rack_user_dir(config):
    return Path(config['rack']['user_dir'] or os.environ.get('RACK_USER_DIR') or xdg('XDG_DATA_HOME', '.local/share') / 'Rack2')


def write_private(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_name(path.name + f'.{os.getpid()}.tmp')
    fd = os.open(temp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, 'w') as stream:
        json.dump(data, stream, indent=2, allow_nan=False)
        stream.write('\n')
    os.replace(temp, path)
