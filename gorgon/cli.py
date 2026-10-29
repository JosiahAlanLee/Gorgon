"""The installed `gorgon` entry point."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import sys

from . import audio, launcher, starter, capture
from .config import LaunchError, default_config, defaults, load, rack_user_dir, run_dir, write_private


def initialize(path, args=None):
    path = Path(path or default_config()).expanduser().resolve()
    if path.exists():
        raise LaunchError(f'Configuration already exists: {path}; edit it or use --config for another profile')
    config = defaults()
    if args:
        if args.workspace:
            config['workspace'] = str(Path(args.workspace).expanduser().resolve())
        if args.hydrapy:
            config['studio']['command'] = [os.path.abspath(Path(args.hydrapy).expanduser())]
        if args.visual_patch:
            config['studio']['patch'] = args.visual_patch
        if args.rack:
            config['rack']['command'] = [str(Path(args.rack).expanduser().resolve())]
        if args.rack_patch:
            config['rack']['patch'] = str(Path(args.rack_patch).expanduser().resolve())
        if args.rack_user:
            config['rack']['user_dir'] = str(Path(args.rack_user).expanduser().resolve())
        if args.audio_device:
            config['audio']['device'] = args.audio_device
        if args.monitor:
            config['studio']['native_output'] = args.monitor
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, 'w') as stream:
        json.dump(config, stream, indent=2)
        stream.write('\n')
    try:
        result = load(path)
    except (LaunchError, OSError, ValueError):
        # Only remove the profile this call just created; invalid init is retryable.
        path.unlink()
        raise
    starter.prepare(result)
    return result


def capture_tools():
    """What render/live/measure-latency need, reported without failing doctor for start/stop users."""
    import importlib.util
    import shutil
    missing = [name for name in ('jack', 'numpy', 'soundfile') if importlib.util.find_spec(name) is None]
    missing += [tool for tool in ('jackd', 'xvfb-run', 'ffmpeg', 'ffprobe') if not shutil.which(tool)]
    return 'all present' if not missing else "missing: " + ', '.join(missing) + " (pip install 'gorgon-live[capture]'; jackd, xvfb-run and ffmpeg from the system)"


def doctor(config):
    checks = []
    def check(name, callback):
        try:
            detail = callback()
            checks.append(dict(check=name, ok=True, detail=detail))
            return detail
        except Exception as exc:
            checks.append(dict(check=name, ok=False, detail=str(exc)))
    check('Apps, visual patch, desktop and Gorgon plugin', lambda: launcher.check_files(config) or 'available')
    check('Capture tools (render, live, measure-latency only)', capture_tools)
    # Diagnostic only: never call ensure_server or start systemd units here.
    graph = check('Existing JACK/PipeWire server', lambda: audio.probe(config))
    if graph:
        check('Playback device', lambda: audio.playback_device(config, graph))
    return dict(config=config['_path'], workspace=config['workspace'], rack_user=str(rack_user_dir(config)),
                ok=all(c['ok'] for c in checks), checks=checks)


def main(argv=None):
    parser = argparse.ArgumentParser(description='Start the Gorgon tracker, HydraPy visuals and VCV Rack together on Linux')
    parser.add_argument('--config', type=Path, default=default_config(), help='launch profile (default: ~/.config/gorgon/launch.json)')
    sub = parser.add_subparsers(dest='command', required=True, metavar='{init,start,stop,status,doctor,render,live,measure-latency}')
    init = sub.add_parser('init', help='one-time profile setup; existing profiles are never overwritten')
    for option in ('workspace', 'hydrapy', 'visual-patch', 'rack', 'rack-patch', 'rack-user', 'audio-device', 'monitor'):
        init.add_argument('--' + option)
    start = sub.add_parser('start', help='start the configured rig and open the tracker')
    start.add_argument('--foreground', action='store_true', help='supervise in this terminal; Ctrl+C stops the apps')
    start.add_argument('--no-browser', action='store_true')
    stop = sub.add_parser('stop', help='close the managed session; Rack can ask you to save')
    stop.add_argument('--force', action='store_true', help='terminate owned apps even if Rack has unsaved edits')
    status = sub.add_parser('status', help='show the managed process state')
    status.add_argument('--json', action='store_true')
    doctor_parser = sub.add_parser('doctor', help='read-only checks and actual JACK port names')
    doctor_parser.add_argument('--json', action='store_true')
    for name in ('render', 'live', 'measure-latency'):
        command = sub.add_parser(name, help='isolated Rack/Studio capture' if name != 'measure-latency' else 'measure six gate-to-voice latencies on private JACK')
        if name != 'measure-latency':
            command.add_argument('score', type=Path, help='tracker score JSON')
        else:
            command.add_argument('--score', type=Path, help='take lane sound defaults from this score')
        command.add_argument('--output', type=Path, required=True, help='new output directory (must not exist)')
        command.add_argument('--seconds', type=float, default=10 if name == 'measure-latency' else 40)
        command.add_argument('--warmup', type=float, default=6)
        command.add_argument('--jack-timeout', type=int, default=10000, help='private JACK client timeout in ms')
        command.add_argument('--plugin-dir', action='append', default=[], help='built Rack plugin directory to install in this run')
        command.add_argument('--source', action='append', default=[], help='explicit JACK capture source; repeat sixteen times')
        command.add_argument('--link', nargs=2, action='append', default=[], metavar=('SOURCE', 'DESTINATION'))
        command.add_argument('--optical-gain', type=float, default=.5)
        command.add_argument('--lead-blocks', type=int, default=2)
        command.add_argument('--size', help='offscreen WIDTHxHEIGHT')
        command.add_argument('--fps', type=float)
        command.add_argument('--sync-gate-channel', type=int, help='measure A/V sync from centre flashes and this capture gate channel (1–16)')
        command.add_argument('--sync-delay-ms', type=float, default=0, help='measured sound latency after the sync gate')
        command.add_argument('--av-offset-ms', type=float, default=0, help='additional video/audio alignment correction')
        if name == 'live':
            command.add_argument('--record', action='store_true', required=True)
            command.add_argument('--native-output', required=True, metavar='MONITOR')
            command.add_argument('--audio-device', help='JACK playback client name (overrides profile audio.device)')
            command.add_argument('--hardware', action='store_true', help='explicitly permit the existing server to reach real devices')
            command.add_argument('--jack-server', help='private dummy server name, or existing server with --hardware')
        elif name == 'render':
            command.add_argument('--jack-server', help='private dummy server name')
    internal = sub.add_parser('_run')
    internal.add_argument('--lock-fd', type=int, required=True)
    internal.add_argument('--session-id', required=True)
    internal.add_argument('--no-browser', action='store_true')
    args = parser.parse_args(argv)
    try:
        if args.command == 'init':
            config = initialize(args.config, args)
            print(f'Profile: {config["_path"]}\nNext: gorgon doctor, then gorgon start')
            return 0
        if args.command == 'start' and not args.config.exists():
            initialize(args.config)
        config = load(args.config)
        if args.command in ('render', 'live', 'measure-latency'):
            return capture.invoke(config, vars(args))
        if args.command == '_run':
            if args.no_browser:
                config['browser'] = False
            # The inherited lock must really be this profile's lock, not an arbitrary fd.
            expected = (run_dir(config) / 'session.lock').stat()
            actual = os.fstat(args.lock_fd)
            if (expected.st_dev, expected.st_ino) != (actual.st_dev, actual.st_ino):
                raise LaunchError('Invalid launcher lock')
            state = launcher.supervise(config, args.lock_fd, args.session_id)
            return 1 if state['phase'] == 'error' else 0
        if args.command == 'start':
            if args.no_browser:
                config['browser'] = False
            state = launcher.start(config, args.foreground)
            print(f'Gorgon: {state.get("phase", "starting")}')
            if state.get('phase') == 'ready' and config['browser']:
                print('Tracker opened in your browser.')
            if state.get('audio'):
                print(f"JACK: {state['audio']['rate']} Hz / {state['audio']['quantum']} frames")
            if state.get('windows'):
                moved = state['windows']
                print(f"Moved Rack to {moved['monitor']}, off the picture's monitor." if moved['moved'] == 'rack' else
                      f"Moved the picture window to {moved['monitor']}, off Rack's monitor.")
            print(f'Logs: {run_dir(config)}')
            return 1 if state.get('phase') == 'error' else 0
        if args.command == 'stop':
            launcher.stop(config, force=args.force)
            print('Gorgon stopped. PipeWire and other applications remain running.')
            return 0
        if args.command == 'status':
            state = launcher.status(config)
            print(json.dumps(state, indent=2) if args.json else f'Gorgon: {state["phase"]}\nLogs: {run_dir(config)}')
            return 0 if state['phase'] in ('ready', 'starting') else 1
        if args.command == 'doctor':
            result = doctor(config)
            if args.json:
                print(json.dumps(result, indent=2))
            else:
                print(f'Profile: {result["config"]}')
                for check in result['checks']:
                    print(f"{'OK' if check['ok'] else 'FAIL'}: {check['check']}")
                    detail = check['detail']
                    if isinstance(detail, dict):
                        print(f"  {detail['rate']} Hz / {detail['quantum']} frames")
                        for port in detail['ports']:
                            print('  ' + port['name'])
                    else:
                        print('  ' + str(detail))
            return 0 if result['ok'] else 1
    except (LaunchError, OSError, ValueError) as exc:
        print(f'gorgon: {exc}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
