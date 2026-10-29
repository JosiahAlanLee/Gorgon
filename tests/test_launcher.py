import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

import pytest

from gorgon import audio, launcher, starter
from gorgon.cli import doctor, initialize, main
from gorgon.config import LaunchError, defaults, load, run_dir, write_private

read_default_sink, read_session_clock = audio.default_sink_names, audio.session_clock


@pytest.fixture
def profile(tmp_path, monkeypatch):
    monkeypatch.setenv('XDG_CONFIG_HOME', str(tmp_path / 'config'))
    monkeypatch.setenv('XDG_DATA_HOME', str(tmp_path / 'data'))
    monkeypatch.setenv('XDG_RUNTIME_DIR', str(tmp_path / 'run'))
    monkeypatch.setattr(audio, 'default_sink_names', lambda: [])  # never this host's audio setup
    monkeypatch.setattr(audio, 'session_clock', lambda c, client, graph: dict(rate=graph['rate'], quantum=graph['quantum']))
    path = tmp_path / 'launch.json'
    value = defaults()
    value['browser'] = False
    value['studio'].update(command=[sys.executable], native_output=False, backend='cpu')
    value['rack'].update(command=['/bin/true'], args=['-h'])
    write_private(path, value)
    config = load(path)
    starter.prepare(config)
    run_dir(config).mkdir(parents=True)
    return config


def port(name, flags, connections=(), aliases=()):
    return dict(name=name, flags=flags, connections=list(connections), aliases=list(aliases))


def test_profile_preserves_saved_material_and_rejects_mistakes(profile):
    root = Path(profile['workspace'])
    saved = root / '.hydrapy/tracker.json'
    saved.write_text('{"my_score": true}')
    starter.prepare(profile)
    assert json.loads(saved.read_text()) == {'my_score': True}
    path = Path(profile['_path'])
    for changes in [dict(audio=dict(rate=True)), dict(studio=dict(command='echo bad')),
                    dict(studio=dict(patch='../outside.py')), dict(audo={})]:
        value = defaults()
        value.update(changes)
        write_private(path, value)
        with pytest.raises(LaunchError):
            load(path)


def test_a_virtual_environment_interpreter_is_not_resolved_out_of_its_environment(profile, tmp_path):
    venv = tmp_path / 'venv/bin'
    venv.mkdir(parents=True)
    (venv / 'python').symlink_to(sys.executable)
    path = Path(profile['_path'])
    value = json.loads(path.read_text())
    value['studio']['command'] = [str(venv / 'python'), '-m', 'hydrapy.cli']
    write_private(path, value)
    assert load(path)['studio']['command'][0] == str(venv / 'python')


def test_initialization_is_not_an_overwrite(profile):
    before = Path(profile['_path']).read_bytes()
    with pytest.raises(LaunchError, match='already exists'):
        initialize(profile['_path'])
    assert Path(profile['_path']).read_bytes() == before


def test_managed_rack_archive_retains_edits_and_embedded_samples(profile):
    import io
    import tarfile
    import zstandard
    path = starter.rack_patch(profile, 'HydraPy-01', 'system')
    document = json.loads(path.read_text())
    document['modules'][0]['data']['offset'] = 23.9
    document['modules'][1]['params'] = [dict(id=0, value=.37)]
    sample = b'original-embedded-sample\0' * 128
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode='w') as tar:
        for name, data in [('patch.json', json.dumps(document).encode()), ('modules/3/sample.wav', sample)]:
            member = tarfile.TarInfo(name)
            member.size = len(data)
            tar.addfile(member, io.BytesIO(data))
    path.write_bytes(zstandard.ZstdCompressor().compress(raw.getvalue()))
    starter.rack_patch(profile, 'HydraPy', 'New output')
    with zstandard.ZstdDecompressor().stream_reader(io.BytesIO(path.read_bytes())) as reader:
        with tarfile.open(fileobj=io.BytesIO(reader.read())) as tar:
            updated = json.loads(tar.extractfile('patch.json').read())
            assert tar.extractfile('modules/3/sample.wav').read() == sample
    assert updated['modules'][0]['data']['offset'] == 23.9
    assert updated['modules'][0]['data']['audio']['deviceName'] == 'HydraPy'
    assert updated['modules'][1]['data']['audio']['deviceName'] == 'New output'
    assert updated['modules'][1]['params'] == document['modules'][1]['params']
    assert updated['cables'] == document['cables']


def test_doctor_is_read_only_and_does_not_choose_arbitrary_hardware(profile, monkeypatch):
    monkeypatch.setattr(launcher, 'check_files', lambda c: None)
    graph = dict(rate=48000, quantum=256, ports=[port('Card A:playback_1', 5), port('Card B:playback_1', 5)])
    monkeypatch.setattr(audio, 'probe', lambda c: graph)
    monkeypatch.setattr(audio.shutil, 'which', lambda _: None)
    monkeypatch.setattr(audio, 'ensure_server', lambda _: pytest.fail('doctor must never start services'))
    result = doctor(profile)
    assert not result['ok']
    assert 'Card A' in result['checks'][-1]['detail'] and 'Card B' in result['checks'][-1]['detail']
    assert not (run_dir(profile) / 'state.json').exists()


def test_the_default_sink_comes_from_pipewire_metadata(profile, monkeypatch):
    graph = dict(rate=48000, quantum=1024, ports=[port('Card A:playback_1', 5), port('Card B:playback_1', 5)])
    dump = [dict(id=36, type='PipeWire:Interface:Metadata', props={'metadata.name': 'default'},
                 metadata=[dict(subject=0, key='default.audio.sink', value=dict(name='alsa_output.card-b'))]),
            dict(id=51, type='PipeWire:Interface:Node', info=dict(props={'node.name': 'alsa_output.card-b', 'node.description': 'Card B'}))]
    monkeypatch.setattr(audio, 'default_sink_names', read_default_sink)
    monkeypatch.setattr(audio.shutil, 'which', lambda name: '/usr/bin/' + name)
    monkeypatch.setattr(audio.subprocess, 'run', lambda command, **_: subprocess.CompletedProcess(command, 0, json.dumps(dump), ''))
    assert audio.playback_device(profile, graph) == 'Card B'


def test_the_ready_quantum_is_the_one_studios_client_runs_at(profile, monkeypatch):
    graph = dict(rate=48000, quantum=1024, ports=[])
    top = """S   ID  QUANT   RATE    WAIT    BUSY   W/Q   B/Q  ERR FORMAT           NAME
C  137      0      0    ---     ---   ---   ---     0                  HydraPy
S   ID  QUANT   RATE    WAIT    BUSY   W/Q   B/Q  ERR FORMAT           NAME
R  199    256  48000   2.3ms   0.5us  0.43  0.00    0     F32P 2 48000 gorgon-check-out
R  140    512  44100  11.0us   0.2us  0.00  0.00    0                   + HydraPy-01
R  137    256  48000  12.5us   2.3ms  0.00  0.43    0                   + HydraPy
"""
    monkeypatch.setattr(audio, 'session_clock', read_session_clock)
    monkeypatch.setattr(audio.shutil, 'which', lambda name: '/usr/bin/' + name)
    monkeypatch.setattr(audio.subprocess, 'run', lambda command, **_: subprocess.CompletedProcess(command, 0, top, ''))
    assert audio.session_clock(profile, 'HydraPy', graph) == dict(rate=48000, quantum=256)
    assert audio.session_clock(profile, 'HydraPy-01', graph) == dict(rate=44100, quantum=512)
    assert audio.session_clock(profile, 'Elsewhere', graph) == dict(rate=48000, quantum=1024)
    profile['audio']['backend'] = 'jack'
    assert audio.session_clock(profile, 'HydraPy', graph) == dict(rate=48000, quantum=1024)


@pytest.mark.parametrize('native_output', ['DP-5', 'window'])
def test_the_picture_and_rack_end_up_on_different_monitors(profile, monkeypatch, native_output):
    fullscreen = native_output != 'window'
    picture = '1920 0 1920 1080' if fullscreen else '1970 87 1280 720'
    listing = {('xrandr', '--listmonitors'): 'Monitors: 2\n 0: +*HDMI-0 1920/477x1080/268+0+0  HDMI-0\n'
                                              ' 1: +DP-5 1920/510x1080/290+1920+0  DP-5\n',
               ('wmctrl', '-lpG'): f'0x0300000b  0 100 {picture} host HydraPy Program\n'
                                   '0x0500000c  0 200 1920 27 1920 1053 host VCV Rack Free 2.6.6 - rig.vcv\n'
                                   '0x0600000d  0 300 0 0 800 600 host Unrelated\n',
               ('xprop', '-id'): '_NET_WM_STATE(ATOM) = _NET_WM_STATE_MAXIMIZED_VERT, _NET_WM_STATE_MAXIMIZED_HORZ'}
    moves = []
    def run(command, **_):
        if command[:2] == ['wmctrl', '-ir']:
            moves.append(command[2:])
        return subprocess.CompletedProcess(command, 0, listing.get(tuple(command[:2]), ''), '')
    monkeypatch.setenv('DISPLAY', ':1')
    monkeypatch.setattr(launcher.shutil, 'which', lambda name: '/usr/bin/' + name)
    monkeypatch.setattr(launcher.subprocess, 'run', run)
    profile['studio']['native_output'] = native_output
    profile['rack']['args'] = []
    result = launcher.separate_windows(profile, 100, 200)
    # A fullscreen picture keeps its monitor and maximized Rack moves; a picture window moves itself.
    if fullscreen:
        assert result == dict(moved='rack', monitor='HDMI-0')
        assert moves == [['0x0500000c', '-b', 'remove,maximized_vert,maximized_horz'], ['0x0500000c', '-e', '0,0,13,-1,-1'],
                         ['0x0500000c', '-b', 'add,maximized_vert,maximized_horz']]
    else:
        assert result == dict(moved='picture', monitor='HDMI-0')
        assert moves[1] == ['0x0300000b', '-e', '0,320,180,-1,-1']
    moves.clear()
    listing[('wmctrl', '-lpG')] = listing[('wmctrl', '-lpG')].replace('200 1920 27', '200 0 27')
    assert launcher.separate_windows(profile, 100, 200) is None and moves == []


def test_stale_pid_is_never_signalled(profile, monkeypatch):
    current = dict(pid=12345, proc_pid=12345, namespace='pid:[test]', start='200', boot='test')
    stale = dict(current, start='199')
    monkeypatch.setattr(launcher, 'identity', lambda pid, proc_pid=None: current if pid == 12345 else None)
    monkeypatch.setattr(launcher.os, 'kill', lambda *args: pytest.fail('must not signal a recycled PID'))
    monkeypatch.setattr(launcher, '_terminate', lambda *args: pytest.fail('must not terminate a recycled PID'))
    write_private(run_dir(profile) / 'state.json', dict(supervisor=stale, children={'rack': stale}))
    assert launcher.stop(profile)['phase'] == 'stopped'


def test_managed_capture_reconnects_only_its_own_listening_inputs(profile, monkeypatch):
    sources = ['HydraPy-01:' + p for p in ('optical_L', 'optical_R', 'breaks_L', 'breaks_R')]
    ports = [port('system:playback_1', 5), port('system:playback_2', 5),
             port('VCV Rack:out_1', 2, ['system:playback_1']), port('VCV Rack:out_2', 2, ['system:playback_2']),
             # Audio 2's own hardware capture inputs must not be selected.
             port('VCV Rack:in_1', 1), port('VCV Rack:in_2', 1)]
    ports += [port(f'VCV Rack-01:in_{i+1}', 1, [f'HydraPy:old_{i}']) for i in range(4)]
    ports += [port('Unrelated:input_1', 1, ['Microphone:capture_1'])]
    operations = []
    def probe(_, changes=()):
        operations.extend(changes)
        return dict(rate=48000, quantum=256, ports=ports)
    monkeypatch.setattr(audio, 'probe', probe)
    assert launcher._routed(profile, sources, 'system') is None
    assert len(operations) == 8
    assert all(op[2].startswith('VCV Rack-01:') for op in operations)
    assert {op[1] for op in operations if op[0] == 'connect'} == set(sources)


def test_explicit_routes_wait_for_ports_and_verify_connections(profile, monkeypatch):
    graph = dict(rate=48000, quantum=256, ports=[])
    calls = []
    monkeypatch.setattr(audio, 'probe', lambda c, ops=(): calls.append(list(ops)) or graph)
    operations = [('connect', 'source:out', 'dest:in')]
    assert launcher._connections_ready(profile, operations) is None
    assert calls == [[]]
    graph['ports'] = [port('source:out', 2), port('dest:in', 1)]
    assert launcher._connections_ready(profile, operations) is None
    assert calls[-1] == operations
    graph['ports'][0]['connections'] = ['dest:in']
    assert launcher._connections_ready(profile, operations) is True


@pytest.mark.parametrize('rack_fails', [False, True])
def test_readiness_order_and_failure_cleanup(profile, monkeypatch, rack_fails):
    children, killed, phases, calls = {}, [], [], []
    sources = ['HydraPy:' + name for name in ('optical_L', 'optical_R', 'breaks_L', 'breaks_R')]
    graph = dict(rate=48000, quantum=256, ports=[port('system:playback_1', 5), port('system:playback_2', 5)])
    class Process:
        def __init__(self, command, **kwargs):
            self.name = 'studio' if 'studio' in command else 'rack'
            self.pid = 100001 + len(children)
            self.returncode = 7 if self.name == 'rack' and rack_fails else None
            import io
            self.stdin = io.BytesIO() if self.name == 'rack' else None
            children[self.name] = self
            calls.append(('spawn', self.name))
            if self.name == 'rack':
                assert ('api', 'tracker_score') in calls
                # Rack's own clients sort after every device, so its device IDs stay put.
                assert kwargs['env']['PIPEWIRE_PROPS'] == '{ priority.session = -1 }'
        def poll(self):
            return self.returncode
        def wait(self, timeout=None):
            self.returncode = 0
    monkeypatch.setattr(launcher.subprocess, 'Popen', Process)
    monkeypatch.setattr(launcher, 'check_files', lambda _: None)
    monkeypatch.setattr(launcher, 'other_racks', lambda: [])
    monkeypatch.setattr(audio, 'ensure_server', lambda _: graph)
    monkeypatch.setattr(audio, 'probe', lambda _: graph)
    monkeypatch.setattr(launcher, 'discovery', lambda _: dict(pid=children['studio'].pid, root=profile['workspace'], url='http://127.0.0.1:9999'))
    def api(_, endpoint, body=None, timeout=3):
        assert timeout == (profile['timeout'] if endpoint in ('tracker_score', 'tracker_transport') else 3)
        calls.append(('api', endpoint))
        assert endpoint in ('tracker', 'tracker_score', 'tracker_transport')
        return dict(path='gorgon-visual.py', revision=1, audio=dict(active=True, client='HydraPy', ports=sources))
    monkeypatch.setattr(launcher, 'api', api)
    monkeypatch.setattr(launcher, '_routed', lambda *_: graph)
    monkeypatch.setattr(launcher, 'save_score', lambda _: calls.append(('save', 'studio')))
    monkeypatch.setattr(launcher, '_terminate', lambda pid, sig: killed.append((pid, sig)))
    original_write = launcher.write_private
    def write(path, data):
        original_write(path, data)
        if path.name == 'state.json':
            phases.append(data['phase'])
            if data['phase'] == 'ready':
                signal.getsignal(signal.SIGTERM)(signal.SIGTERM, None)
    monkeypatch.setattr(launcher, 'write_private', write)
    previous = {sig: signal.getsignal(sig) for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGUSR1)}
    try:
        result = launcher.supervise(profile, -1, 'test')
    finally:
        for sig, handler in previous.items():
            signal.signal(sig, handler)
    assert children, result
    assert [c for c in calls if c[0] == 'spawn'] == [('spawn', 'studio'), ('spawn', 'rack')]
    assert (children['studio'].pid, signal.SIGINT) in killed
    assert all(pid in {p.pid for p in children.values()} for pid, _ in killed)
    if rack_fails:
        assert result['phase'] == 'error' and 'rack exited' in result['error']
        assert 'ready' not in phases
    else:
        assert 'ready' in phases and result['phase'] == 'stopped'


def test_cli_help_and_exit_status(capsys):
    with pytest.raises(SystemExit) as exc:
        main(['--help'])
    assert exc.value.code == 0
    assert '_run' not in capsys.readouterr().out
    result = subprocess.run([sys.executable, '-m', 'gorgon', '--config', '/nonexistent/gorgon-test.json', 'status'], capture_output=True)
    assert result.returncode == 1


def test_executable_resolves_a_linked_rack_to_its_install(tmp_path):
    # Rack runs with cwd = its executable's directory: that must be the install, not the link's.
    install = tmp_path / 'Rack install ü'
    install.mkdir()
    real = install / 'Rack'
    real.write_text('#!/bin/sh\n')
    real.chmod(0o755)
    (tmp_path / 'bin').mkdir()
    link = tmp_path / 'bin' / 'Rack'
    link.symlink_to(real)
    assert launcher.executable([str(link)]) == str(real.resolve())


@pytest.mark.parametrize('state', [[], None, 42, {'supervisor': []},
                                    {'children': []}, {'children': {'rack': 'invalid'}}])
def test_malformed_session_state_is_recoverable(profile, state):
    write_private(run_dir(profile) / 'state.json', state)
    assert launcher.status(profile)['phase'] == 'stopped'
    assert launcher.stop(profile)['phase'] == 'stopped'


def test_invalid_init_does_not_leave_a_profile_that_blocks_retry(tmp_path, monkeypatch):
    path = tmp_path / 'profile.json'
    # The normal CLI accepts this argument, but profile validation rejects it.
    assert main(['--config', str(path), 'init', '--visual-patch', '../outside.py']) == 1
    assert not path.exists()
    monkeypatch.setenv('XDG_DATA_HOME', str(tmp_path / 'data'))
    assert main(['--config', str(path), 'init']) == 0


@pytest.mark.parametrize('error', [FileNotFoundError('wmctrl disappeared'),
                                 subprocess.TimeoutExpired('wmctrl', 3)])
def test_window_close_tool_failure_is_best_effort(monkeypatch, error):
    monkeypatch.setenv('DISPLAY', ':1')
    monkeypatch.setattr(launcher.shutil, 'which', lambda _: '/usr/bin/wmctrl')
    def fail(*args, **kwargs):
        raise error
    monkeypatch.setattr(launcher.subprocess, 'run', fail)
    assert launcher.close_rack_window(12345) is False



@pytest.mark.parametrize('timeout', [None, 120])
def test_api_timeout_passed_to_urlopen(monkeypatch, timeout):
    import io

    def urlopen(request, **kwargs):
        assert request.full_url == 'http://127.0.0.1:9999/api/tracker_score'
        assert kwargs['timeout'] == (3 if timeout is None else timeout)
        return io.BytesIO(b'{"revision": 2}')

    monkeypatch.setattr(launcher.urllib.request, 'urlopen', urlopen)
    kwargs = {} if timeout is None else dict(timeout=timeout)
    assert launcher.api(dict(url='http://127.0.0.1:9999', token='test'),
                        'tracker_score', dict(document={}), **kwargs) == dict(revision=2)


@pytest.mark.parametrize('wrapped', [False, True])
@pytest.mark.parametrize('during_read', [False, True])
def test_api_timeout_names_endpoint(monkeypatch, wrapped, during_read):
    import io
    import urllib.error

    error = TimeoutError('timed out')
    if wrapped:
        error = urllib.error.URLError(error)

    class Response(io.BytesIO):
        def read(self):
            raise error

    def urlopen(*args, **kwargs):
        if during_read:
            return Response()
        raise error

    monkeypatch.setattr(launcher.urllib.request, 'urlopen', urlopen)
    with pytest.raises(LaunchError, match=r'/api/tracker_score timed out after 120 seconds'):
        launcher.api(dict(url='http://127.0.0.1:9999', token='test'),
                     'tracker_score', {}, timeout=120)


def test_api_preserves_non_timeout_url_errors(monkeypatch):
    import urllib.error

    error = urllib.error.URLError(ConnectionRefusedError('refused'))
    def urlopen(*args, **kwargs):
        raise error
    monkeypatch.setattr(launcher.urllib.request, 'urlopen', urlopen)
    with pytest.raises(urllib.error.URLError) as exc:
        launcher.api(dict(url='http://127.0.0.1:9999', token='test'), 'tracker')
    assert exc.value is error
