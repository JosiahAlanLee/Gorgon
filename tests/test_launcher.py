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

read_default_sink = audio.default_sink_names


@pytest.fixture
def profile(tmp_path, monkeypatch):
    monkeypatch.setenv('XDG_CONFIG_HOME', str(tmp_path / 'config'))
    monkeypatch.setenv('XDG_DATA_HOME', str(tmp_path / 'data'))
    monkeypatch.setenv('XDG_RUNTIME_DIR', str(tmp_path / 'run'))
    monkeypatch.setattr(audio, 'default_sink_names', lambda: [])  # never this host's audio setup
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


def test_stale_pid_is_never_signalled(profile):
    process = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(30)'])
    try:
        info = launcher.identity(process.pid)
        assert info
        info['start'] = str(int(info['start']) + 1)
        write_private(run_dir(profile) / 'state.json', dict(supervisor=info, children={}))
        launcher.stop(profile)
        assert process.poll() is None
    finally:
        process.terminate()
        process.wait(timeout=3)


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
    def api(_, endpoint, body=None):
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
