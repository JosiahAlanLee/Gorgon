"""Own a pair of app processes, not the desktop audio service or other sessions."""
from __future__ import annotations

import fcntl
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import threading
import time
import urllib.request
from urllib.parse import urlsplit
import uuid
import webbrowser

from . import audio, starter
from .config import LaunchError, rack_user_dir, run_dir, write_private


def _proc_identity(path):
    try:
        text = (path / 'stat').read_text()
        fields = text.rsplit(')', 1)[1].split()
        if fields[0] == 'Z':
            return None
        namespace = os.readlink(path / 'ns/pid')
        if namespace != os.readlink('/proc/self/ns/pid'):
            return None
        line = next((s for s in (path / 'status').read_text().splitlines() if s.startswith('NSpid:')), '')
        pid = int(line.split()[-1]) if line else int(text.split()[0])
        return dict(pid=pid, proc_pid=int(text.split()[0]), namespace=namespace,
                    start=fields[19], boot=Path('/proc/sys/kernel/random/boot_id').read_text().strip())
    except (OSError, ValueError, IndexError):
        return None


def identity(pid, proc_pid=None):
    if type(pid) is not int or pid <= 0:
        return None
    info = _proc_identity(Path('/proc/self') if pid == os.getpid() else Path(f'/proc/{proc_pid or pid}'))
    if info and info['pid'] == pid:
        return info
    # A container may expose host /proc while returning namespaced PIDs. Keep
    # both identities, and never signal a process in a different PID namespace.
    if proc_pid is None and int(Path('/proc/self/stat').read_text().split()[0]) != os.getpid():
        for entry in Path('/proc').iterdir():
            if entry.name.isdigit():
                info = _proc_identity(entry)
                if info and info['pid'] == pid:
                    return info
    return None


def alive(info):
    return bool(isinstance(info, dict) and info and identity(info.get('pid'), info.get('proc_pid')) == info)


def read_state(config):
    try:
        state = json.loads((run_dir(config) / 'state.json').read_text())
        if not isinstance(state, dict):
            return {}
        if not isinstance(state.get('children', {}), dict):
            state['children'] = {}
        return state
    except (OSError, ValueError):
        return {}


def status(config):
    state = read_state(config)
    if alive(state.get('supervisor')):
        return state
    state['phase'] = 'orphaned' if any(alive(p) for p in state.get('children', {}).values()) else 'error' if state.get('phase') == 'error' else 'stopped'
    return state


def executable(command):
    resolved = shutil.which(command[0])
    if not resolved:
        raise LaunchError(f'Executable {command[0]!r} is missing; edit the command in your launch profile')
    # Resolve symlinks: Rack runs with its executable's directory as the working directory,
    # which must be the real install, not the directory holding a link to it.
    return str(Path(resolved).resolve())


def other_racks():
    result = []
    for entry in Path('/proc').iterdir():
        if not entry.name.isdigit():
            continue
        try:
            if entry.stat().st_uid == os.getuid() and (entry / 'exe').resolve().name == 'Rack':
                info = _proc_identity(entry)
                if info:
                    result.append(info['pid'])
        except OSError:
            pass
    return result


def check_files(config):
    if not sys.platform.startswith('linux'):
        raise LaunchError('This session launcher currently supports Linux')
    for name in ('studio', 'rack'):
        executable(config[name]['command'])
    root = Path(config['workspace'])
    if not (root / config['studio']['patch']).is_file():
        raise LaunchError(f'Visual patch is missing: {root / config["studio"]["patch"]}')
    if config['rack']['patch'] and not Path(config['rack']['patch']).is_file():
        raise LaunchError(f'Rack patch is missing: {config["rack"]["patch"]}')
    if (config['browser'] or config['studio']['native_output'] or not _headless(config)) and not (os.environ.get('DISPLAY') or os.environ.get('WAYLAND_DISPLAY')):
        raise LaunchError('No desktop display is available; use a desktop terminal, or disable browser/native_output and set rack.args to ["-h"] for headless use')
    plugins = rack_user_dir(config) / 'plugins-lin-x64'
    installed = any(p.name.startswith('GorgonBridge-') for p in plugins.glob('*.vcvplugin'))
    for manifest in plugins.glob('*/plugin.json'):
        try:
            installed |= json.loads(manifest.read_text()).get('slug') == 'GorgonBridge'
        except (OSError, ValueError):
            pass
    if not installed:
        raise LaunchError(f'GorgonBridge is not installed in {plugins}; install it there or set rack.user_dir to the Rack folder you use')


def discovery(config):
    return json.loads((run_dir(config) / 'studio.json').read_text())


def api(info, endpoint, body=None):
    parsed = urlsplit(info['url'])
    if parsed.scheme != 'http' or parsed.hostname != '127.0.0.1' or not parsed.port or parsed.path not in ('', '/'):
        raise LaunchError('Studio advertised an invalid local URL')
    request = urllib.request.Request(info['url'] + '/api/' + endpoint,
                                     headers={'X-Hydrapy-Token': info['token'], 'Content-Type': 'application/json'},
                                     data=None if body is None else json.dumps(body, allow_nan=False).encode())
    with urllib.request.urlopen(request, timeout=3) as response:
        return json.loads(response.read())


def save_score(config):
    try:
        info = discovery(config)
        state = api(info, 'tracker')
        api(info, 'tracker_save', dict(revision=state['revision']))
        api(info, 'tracker_transport', dict(playing=False))
    except (OSError, ValueError, KeyError, LaunchError):
        pass  # Failure cleanup must still stop the processes it owns.


def _terminate(pid, sig):
    try:
        if os.getpgid(pid) == pid:
            os.killpg(pid, sig)
        else:
            os.kill(pid, sig)
    except ProcessLookupError:
        pass


def _headless(config):
    return any(arg in ('-h', '--headless') for arg in config['rack']['args'])


def close_rack_window(pid):
    """Ask the WM to close only our Rack window, preserving Rack's save dialog."""
    if not os.environ.get('DISPLAY') or not shutil.which('wmctrl'):
        return False
    try:
        result = subprocess.run(['wmctrl', '-lp'], capture_output=True, text=True, timeout=3)
        for line in result.stdout.splitlines():
            parts = line.split(None, 4)
            if len(parts) == 5 and parts[2].isdigit() and int(parts[2]) == pid and 'VCV Rack' in parts[4]:
                return subprocess.run(['wmctrl', '-ic', parts[0]], capture_output=True, timeout=3).returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        pass  # Optional WM tools must never interrupt cleanup of owned apps.

    return False


def _monitors():
    monitors = []
    for line in subprocess.run(['xrandr', '--listmonitors'], capture_output=True, text=True, timeout=3).stdout.splitlines()[1:]:
        match = re.search(r'(\d+)/\d+x(\d+)/\d+\+(-?\d+)\+(-?\d+)\s+(\S+)\s*$', line)
        if match:
            w, h, x, y = map(int, match.groups()[:4])
            monitors.append(dict(name=match[5], x=x, y=y, w=w, h=h, primary='*' in line.split()[1]))
    return monitors


def _windows(pid):
    windows = []
    for line in subprocess.run(['wmctrl', '-lpG'], capture_output=True, text=True, timeout=3).stdout.splitlines():
        parts = line.split(None, 8)
        if len(parts) == 9 and parts[2].isdigit() and int(parts[2]) == pid:
            x, y, w, h = map(int, parts[3:7])
            windows.append(dict(id=parts[0], x=x, y=y, w=w, h=h, title=parts[8]))
    return windows


def _monitor_of(window, monitors):
    cx, cy = window['x'] + window['w'] // 2, window['y'] + window['h'] // 2
    return next((m for m in monitors if m['x'] <= cx < m['x'] + m['w'] and m['y'] <= cy < m['y'] + m['h']), None)


def separate_windows(config, studio_pid, rack_pid):
    """Keep the picture and Rack on different monitors. A fullscreen picture keeps the monitor it was given
    and Rack moves off it; a picture window moves to a monitor Rack is not on. Best effort (X11 only)."""
    if not config['studio']['native_output'] or _headless(config) or not os.environ.get('DISPLAY') \
            or not shutil.which('wmctrl') or not shutil.which('xrandr'):
        return None
    try:
        monitors = _monitors()
        if len(monitors) < 2:
            return None
        end = time.monotonic() + 3
        while True:
            pictures = _windows(studio_pid)
            racks = [w for w in _windows(rack_pid) if 'VCV Rack' in w['title']]
            if pictures and racks or time.monotonic() > end:
                break
            time.sleep(.1)
        if not pictures or not racks:
            return None
        picture, rack = max(pictures, key=lambda w: w['w'] * w['h']), racks[0]
        shared = _monitor_of(picture, monitors)
        if shared is None or shared is not _monitor_of(rack, monitors):
            return None
        others = [m for m in monitors if m is not shared]
        target = next((m for m in others if m['primary']), others[0])
        moved, window = ('rack', rack) if config['studio']['native_output'] != 'window' else ('picture', picture)
        state = subprocess.run(['xprop', '-id', window['id'], '_NET_WM_STATE'], capture_output=True, text=True,
                               timeout=3).stdout if shutil.which('xprop') else ''
        maximized = 'MAXIMIZED' in state
        wmctrl = lambda *args: subprocess.run(['wmctrl', '-ir', window['id'], *args], capture_output=True, timeout=3)
        if maximized:
            wmctrl('-b', 'remove,maximized_vert,maximized_horz')
        wmctrl('-e', f"0,{target['x'] + max(0, (target['w'] - window['w']) // 2)},"
                     f"{target['y'] + max(0, (target['h'] - window['h']) // 2)},-1,-1")
        if maximized:
            wmctrl('-b', 'add,maximized_vert,maximized_horz')
        return dict(moved=moved, monitor=target['name'])
    except (OSError, ValueError, subprocess.TimeoutExpired):
        return None


def stop(config, force=False):
    state = read_state(config)
    supervisor = state.get('supervisor')
    if alive(supervisor):
        requested = time.monotonic()
        os.kill(supervisor['pid'], signal.SIGUSR1 if force else signal.SIGTERM)
        end = time.monotonic() + 55
        while alive(supervisor) and time.monotonic() < end:
            latest = read_state(config)
            if latest.get('phase') == 'ready' and latest.get('stop_attempt', 0) >= requested and latest.get('notice'):
                raise LaunchError(latest['notice'])
            time.sleep(.1)
        if alive(supervisor):
            raise LaunchError(f'Shutdown is still running; inspect {run_dir(config) / "launcher.log"}')
    else:
        # Recover only verified children of a launcher that was killed itself.
        for name, child in reversed(list(state.get('children', {}).items())):
            if not alive(child):
                continue
            if name == 'rack' and not force:
                if close_rack_window(child['pid']):
                    end = time.monotonic() + 5
                    while alive(child) and time.monotonic() < end:
                        time.sleep(.1)
                if alive(child):
                    raise LaunchError('Save and close the orphaned Rack window, or use gorgon stop --force')
                continue
            _terminate(child['pid'], signal.SIGINT if name == 'studio' else signal.SIGTERM)
            end = time.monotonic() + 8
            while alive(child) and time.monotonic() < end:
                time.sleep(.1)
            if alive(child):
                _terminate(child['pid'], signal.SIGKILL)
    return status(config)


def open_editor(config):
    info = discovery(config)
    # The token is a fragment, never printed in status or the launcher's output.
    webbrowser.open(info['url'] + '/tracker/#' + info['token'])


def start(config, foreground=False):
    if not sys.platform.startswith('linux'):
        raise LaunchError('gorgon start currently supports Linux')
    directory = run_dir(config)
    directory.mkdir(parents=True, exist_ok=True, mode=0o700)
    os.chmod(directory, 0o700)
    fd = os.open(directory / 'session.lock', os.O_RDWR | os.O_CREAT, 0o600)
    try:
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            state = status(config)
            if state.get('phase') == 'ready' and config['browser']:
                open_editor(config)
            return state  # Starting twice never creates a second stack.
        if status(config)['phase'] == 'orphaned':
            raise LaunchError('A previous launcher left app processes running; use gorgon stop before starting again')
        nonce = uuid.uuid4().hex
        if foreground:
            return supervise(config, fd, nonce)
        logfd = os.open(directory / 'launcher.log', os.O_WRONLY | os.O_APPEND | os.O_CREAT, 0o600)
        with os.fdopen(logfd, 'ab', buffering=0) as log:
            process = subprocess.Popen([sys.executable, '-m', 'gorgon.cli', '--config', config['_path'], '_run',
                                        '--lock-fd', str(fd), '--session-id', nonce,
                                        *(['--no-browser'] if not config['browser'] else [])],
                                       pass_fds=(fd,), stdin=subprocess.DEVNULL, stdout=log, stderr=log,
                                       start_new_session=True)
        end = time.monotonic() + config['timeout'] + 5
        while time.monotonic() < end:
            state = read_state(config)
            if state.get('session_id') == nonce and state.get('phase') in ('ready', 'error', 'stopped'):
                if state['phase'] != 'ready':
                    raise LaunchError(state.get('error') or f'Session stopped during startup; see {directory}')
                if config['browser']:
                    open_editor(config)
                return state
            if process.poll() is not None:
                raise LaunchError(f'Launcher exited before readiness; see {directory / "launcher.log"}')
            time.sleep(.1)
        process.send_signal(signal.SIGTERM)
        raise LaunchError(f'Startup timed out; inspect {directory}')
    finally:
        # Closing our copy keeps the inherited lock held by the supervisor.
        os.close(fd)


def _wait(predicate, end, stopping, children, what):
    last = None
    while time.monotonic() < end and not stopping.is_set():
        for name, process in children.items():
            if process.poll() is not None:
                raise LaunchError(f'{name} exited during startup ({process.returncode}); inspect {name}.log')
        try:
            value = predicate()
            if value:
                return value
        except (OSError, ValueError, KeyError) as exc:
            last = exc
        time.sleep(.15)
    if stopping.is_set():
        raise LaunchError('Startup cancelled')
    raise LaunchError(f'Timed out waiting for {what}; last result: {last}')


def _studio_ready(config, process):
    info = discovery(config)
    if info['pid'] != process.pid or Path(info['root']).resolve() != Path(config['workspace']):
        raise LaunchError('The advertised Studio does not belong to this session')
    state = api(info, 'tracker')
    if state.get('error'):
        raise LaunchError('Studio: ' + state['error'])
    sound = state.get('audio', {})
    if sound.get('error'):
        raise LaunchError('Studio JACK audio: ' + sound['error'])
    if state.get('path') and sound.get('active') and len(sound.get('ports', [])) == 4:
        return info, sound


def _routed(config, sources, device, previous=()):
    graph = audio.probe(config)
    ports = [p for p in audio.rack_ports(graph) if p['name'] not in previous]
    outputs = [p for p in ports if p['flags'] & 2]
    destinations = {p['name'] for p in graph['ports'] if p['flags'] & 1 and
                    (p['name'].split(':', 1)[0] == device if device else p['flags'] & 4)}
    if sum(bool(set(p['connections']) & destinations) for p in outputs) < (2 if config['rack']['patch'] is None else 1):
        return None
    if config['rack']['patch'] is None:
        # Only the managed starter has exactly one four-channel listening input.
        groups = {}
        for port in ports:
            groups.setdefault(port['name'].split(':', 1)[0], []).append(port)
        captures = [group for group in groups.values()
                    if sum(bool(p['flags'] & 1) for p in group) == 4 and not any(p['flags'] & 2 for p in group)]
        if len(captures) != 1:
            return None
        inputs = sorted(captures[0], key=lambda p: [int(x) if x.isdigit() else x for x in re.split(r'(\d+)', p['name'])])
        changes = []
        for source, port in zip(sources, inputs):
            changes.extend(('disconnect', old, port['name']) for old in port['connections'] if old != source)
            if source not in port['connections']:
                changes.append(('connect', source, port['name']))
        if changes:
            audio.probe(config, changes)
            return None  # Re-query to verify PipeWire's asynchronous changes.
    return graph


def _connections_ready(config, operations):
    graph = audio.probe(config)
    ports = {p['name']: p for p in graph['ports']}
    if any(source not in ports or destination not in ports for _, source, destination in operations):
        return None
    changes = [op for op in operations if op[2] not in ports[op[1]]['connections']]
    if changes:
        audio.probe(config, changes)
        return None
    return True


def supervise(config, lock_fd, nonce):
    directory = run_dir(config)
    state = dict(phase='starting', session_id=nonce, supervisor=identity(os.getpid()), children={},
                 config=config['_path'], logs=str(directory))
    children = {}
    stopping = threading.Event()
    forced = threading.Event()
    for sig in (signal.SIGTERM, signal.SIGINT):
        signal.signal(sig, lambda *_: stopping.set())
    def force_stop(*_):
        forced.set()
        stopping.set()
    signal.signal(signal.SIGUSR1, force_stop)
    def publish(**values):
        state.update(values)
        write_private(directory / 'state.json', state)
    def spawn(name, command, env, cwd):
        descriptor = os.open(directory / f'{name}.log', os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
        with os.fdopen(descriptor, 'ab', buffering=0) as log:
            process = subprocess.Popen(command, env=env, cwd=cwd, stdin=subprocess.PIPE if name == 'rack' else subprocess.DEVNULL,
                                       stdout=log, stderr=log, start_new_session=True)
        children[name] = process
        state['children'][name] = identity(process.pid)
        publish()
        return process
    shared_fd = None
    failure = None
    try:
        publish()
        # One managed Rack rig at a time, including concurrent different profiles.
        shared_fd = os.open(directory.parent / 'rack.lock', os.O_RDWR | os.O_CREAT, 0o600)
        try:
            fcntl.flock(shared_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise LaunchError('Another managed Gorgon/Rack session is running')
        check_files(config)
        if other_racks():
            raise LaunchError('VCV Rack is already running outside this launcher; close it before starting this session')
        graph = audio.ensure_server(config)
        device = audio.playback_device(config, graph) if config['rack']['patch'] is None or config['audio']['device'] else None
        root = Path(config['workspace'])
        outputs = root / '.hydrapy' / 'outputs.json'
        try:
            settings = json.loads(outputs.read_text())
        except FileNotFoundError:
            settings = {}
        settings['audio'] = dict(settings.get('audio', {}), enabled=True)
        write_private(outputs, settings)
        bridge = directory / 'studio.json'
        bridge.unlink(missing_ok=True)
        env = audio.environment(config)
        env['HYDRAPY_STUDIO_FILE'] = str(bridge)
        s = config['studio']
        command = [*s['command'], 'studio', s['patch'], '--root', str(root), '--tracker', '--no-browser',
                   '--port', '0', '--backend', s['backend'], '--size', s['size'], '--fps', str(s['fps'])]
        command += ['--native-output', s['native_output']] if s['native_output'] else ['--no-native-output']
        end = time.monotonic() + config['timeout']
        studio = spawn('studio', audio.wrap(config, [*command, *s['args']]), env, root)
        info, sound = _wait(lambda: _studio_ready(config, studio), end, stopping, children, 'Studio and its four JACK ports')
        current = api(info, 'tracker')
        saved = root / '.hydrapy' / 'tracker.json'
        if saved.exists():
            api(info, 'tracker_score', dict(document=json.loads(saved.read_text()), revision=current['revision']))
        api(info, 'tracker_transport', dict(playing=False, enabled=True))
        patch = starter.rack_patch(config, sound['client'], device)
        command = list(config['rack']['command'])
        command[0] = executable(command)
        command += ['-u', str(rack_user_dir(config)), *config['rack']['args'], str(patch)]
        previous = {p['name'] for p in audio.probe(config)['ports']}
        spawn('rack', audio.wrap(config, command), audio.rack_environment(config, env), Path(command[0]).parent)
        operations = [('connect', a.replace('{studio}', sound['client']), b.replace('{studio}', sound['client']))
                      for a, b in config['audio']['connections']]
        if operations:
            _wait(lambda: _connections_ready(config, operations), end, stopping, children, 'configured JACK connections')
        graph = _wait(lambda: _routed(config, sound['ports'], device, previous), end, stopping, children, 'Rack JACK playback connections')
        clock = audio.session_clock(config, sound['client'], graph)
        windows = separate_windows(config, studio.pid, children['rack'].pid)
        publish(phase='ready', url=info['url'] + '/tracker/', audio=dict(rate=clock['rate'], quantum=clock['quantum'],
                                                                      studio_ports=sound['ports'], device=device),
                rack_patch=str(patch), windows=windows)
        if config['browser'] and os.getpid() == state['supervisor']['pid'] and '--lock-fd' not in sys.argv:
            open_editor(config)  # Foreground mode; detached mode opens in its caller.
        while True:
            if stopping.wait(.25):
                rack = children['rack']
                if rack.poll() is None and not _headless(config) and not forced.is_set():
                    attempted = time.monotonic()
                    if close_rack_window(rack.pid):
                        try:
                            rack.wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            pass
                    if rack.poll() is None:
                        publish(phase='ready', stop_attempt=attempted,
                                notice='Save and close the Rack window to end the session, or use gorgon stop --force')
                        stopping.clear()
                        continue
                break
            exited = [name for name, process in children.items() if process.poll() is not None]
            if exited:
                if any(children[name].returncode for name in exited):
                    failure = ', '.join(exited) + ' exited; inspect the session logs'
                break
    except Exception as exc:
        failure = str(exc)
    finally:
        if 'studio' in children and children['studio'].poll() is None:
            save_score(config)
        for name, process in reversed(list(children.items())):
            if process.poll() is None:
                if name == 'rack' and _headless(config) and process.stdin:
                    try:
                        process.stdin.write(b'\n')
                        process.stdin.flush()
                    except (OSError, BrokenPipeError):
                        pass
                elif name == 'rack' and not forced.is_set():
                    close_rack_window(process.pid)
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        failure = (failure or 'Studio stopped') + '; save/close the remaining Rack window (gorgon stop --force can terminate it)'
                        continue
                else:
                    _terminate(process.pid, signal.SIGINT)
                try:
                    process.wait(timeout=30 if name == 'studio' else 8)
                except subprocess.TimeoutExpired:
                    _terminate(process.pid, signal.SIGKILL)
                    process.wait(timeout=3)
        publish(phase='error' if failure else 'stopped', **(dict(error=failure) if failure else {}))
        if shared_fd is not None:
            os.close(shared_fd)
    return state
