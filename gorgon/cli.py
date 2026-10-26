"""The installed `gorgon` entry point."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import sys

from . import audio, launcher, starter
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
    result = load(path)
    starter.prepare(result)
    return result


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
    # Diagnostic only: never call ensure_server or start systemd units here.
    graph = check('Existing JACK/PipeWire server', lambda: audio.probe(config))
    if graph:
        check('Playback device', lambda: audio.playback_device(config, graph))
    return dict(config=config['_path'], workspace=config['workspace'], rack_user=str(rack_user_dir(config)),
                ok=all(c['ok'] for c in checks), checks=checks)


def main(argv=None):
    parser = argparse.ArgumentParser(description='Start the Gorgon tracker, HydraPy visuals and VCV Rack together on Linux')
    parser.add_argument('--config', type=Path, default=default_config(), help='launch profile (default: ~/.config/gorgon/launch.json)')
    sub = parser.add_subparsers(dest='command', required=True, metavar='{init,start,stop,status,doctor}')
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
