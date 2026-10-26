"""Opt-in: real PipeWire, real Studio and Rack Free, without physical devices."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

import pytest

from gorgon import audio, launcher, starter
from gorgon.config import defaults, load, run_dir, write_private

pytestmark = pytest.mark.skipif(not os.environ.get('GORGON_RACK_EXECUTABLE'), reason='requires Rack Free and PipeWire')


def until(callback, timeout=15):
    end = time.monotonic() + timeout
    last = None
    while time.monotonic() < end:
        try:
            last = callback()
            if last:
                return last
        except Exception as exc:
            last = exc
        time.sleep(.1)
    raise AssertionError(f'condition did not become ready: {last}')


def test_start_restore_audio_idempotency_stop_and_relaunch(tmp_path, monkeypatch):
    import numpy as np
    runtime = tmp_path / 'runtime'
    runtime.mkdir(mode=0o700)
    conf = tmp_path / 'pipewire-config'
    source_conf = Path(os.environ.get('GORGON_PIPEWIRE_CONFIG', '/usr/share/pipewire'))
    shutil.copytree(source_conf, conf)
    for key, value in dict(XDG_RUNTIME_DIR=str(runtime), PIPEWIRE_RUNTIME_DIR=str(runtime),
                           PIPEWIRE_CONFIG_DIR=str(conf), XDG_DATA_HOME=str(tmp_path / 'data'),
                           XDG_CONFIG_HOME=str(tmp_path / 'config')).items():
        monkeypatch.setenv(key, value)
    monkeypatch.delenv('PIPEWIRE_REMOTE', raising=False)
    with open(tmp_path / 'pipewire.log', 'wb') as log:
        server = subprocess.Popen(['pipewire'], stdout=log, stderr=log)
    config_path = tmp_path / 'launch.json'
    config = defaults()
    config.update(browser=False, timeout=40)
    config['studio'].update(command=[sys.executable, '-m', 'hydrapy.cli'], backend='cpu',
                            size='96x54', fps=20, native_output=False)
    config['rack'].update(command=[os.environ['GORGON_RACK_EXECUTABLE']], args=['-h'], user_dir=str(tmp_path / 'rack-user'))
    config['audio']['start_services'] = False
    write_private(config_path, config)
    config = load(config_path)
    starter.prepare(config)
    plugin = Path(os.environ['GORGON_PLUGIN_DIRECTORY'])
    destination = Path(config['rack']['user_dir']) / 'plugins-lin-x64/GorgonBridge'
    destination.mkdir(parents=True)
    for name in ('plugin.json', 'plugin.so', 'res', 'LICENSE'):
        source = plugin / name
        if source.is_dir():
            shutil.copytree(source, destination / name)
        elif source.is_file():
            shutil.copy2(source, destination / name)
    def command(action, *args, check=True):
        result = subprocess.run([sys.executable, '-m', 'gorgon', '--config', str(config_path), action, *args],
                                capture_output=True, text=True, timeout=65)
        if check:
            assert result.returncode == 0, result.stdout + result.stderr + '\n' + str(launcher.read_state(config))
        return result
    foreign = receiver = output = None
    try:
        until(lambda: audio.probe(config))
        import jack
        # A long-running desktop server reuses node IDs, so a new client can sort before an existing
        # device. Free some IDs below the output's, as there, for Studio's and Rack's clients to take.
        placeholders = [jack.Client(f'gorgon_test_placeholder{i}', no_start_server=True) for i in range(8)]
        for client in placeholders:
            client.outports.register('out')
            client.activate()
        output = jack.Client('Gorgon Test Output', no_start_server=True)
        for name in ('playback_FL', 'playback_FR'):
            output.inports.register(name)
        output.activate()
        for client in placeholders:
            client.deactivate()
            client.close()
        graph = until(lambda: any(p['name'].startswith('Gorgon Test Output:') for p in audio.probe(config)['ports'])
                      and audio.probe(config))
        sinks = [p['name'].split(':', 1)[0] for p in graph['ports'] if p['flags'] & 1 and 'gorgon' in p['name'].lower()]
        assert sinks, graph
        value = json.loads(config_path.read_text())
        value['audio']['device'] = sinks[0]
        write_private(config_path, value)
        config = load(config_path)
        foreign = jack.Client('HydraPy', no_start_server=True)
        for name in ('optical_L', 'optical_R', 'breaks_L', 'breaks_R'):
            foreign.outports.register(name)
        foreign.activate()
        saved = Path(config['workspace']) / '.hydrapy/tracker.json'
        score = json.loads(saved.read_text())
        score['bpm'] = 111
        saved.write_text(json.dumps(score))
        command('start')
        state = launcher.status(config)
        assert state['phase'] == 'ready'
        assert state['audio']['studio_ports'][0].split(':')[0] != foreign.name
        assert launcher.api(launcher.discovery(config), 'tracker')['document']['bpm'] == 111
        children = state['children']
        command('start')
        assert launcher.status(config)['children'] == children
        receiver = jack.Client('gorgon_test_receiver', no_start_server=True)
        ports = [receiver.inports.register('capture_' + str(i)) for i in range(2)]
        captured = []
        @receiver.set_process_callback
        def capture(frames):
            if len(captured) < 2000:
                captured.append(np.column_stack([p.get_array() for p in ports]))
        receiver.activate()
        graph = audio.probe(config)
        rack_outputs = [p for p in audio.rack_ports(graph) if p['flags'] & 2]
        assert len(rack_outputs) == 2
        for source, destination in zip(rack_outputs, ports):
            receiver.connect(source['name'], destination.name)
        info = launcher.discovery(config)
        launcher.api(info, 'tracker_transport', dict(enabled=True, playing=True, beat=0))
        until(lambda: captured and np.max(np.abs(np.concatenate(captured))) > .0001, timeout=8)
        peak = float(np.max(np.abs(np.concatenate(captured))))
        snapshot = launcher.api(info, 'tracker')
        snapshot['document']['bpm'] = 123
        launcher.api(info, 'tracker_score', dict(document=snapshot['document'], revision=snapshot['revision']))
        command('stop')
        assert server.poll() is None
        assert all(not launcher.alive(p) for p in children.values())
        assert json.loads(saved.read_text())['bpm'] == 123
        assert any(p['name'].startswith(foreign.name + ':') for p in audio.probe(config)['ports'])
        receiver.deactivate()
        receiver.close()
        receiver = None
        # Name changes on restart: repair only the managed Rack capture inputs.
        foreign.deactivate()
        foreign.close()
        foreign = None
        command('start')
        assert launcher.api(launcher.discovery(config), 'tracker')['document']['bpm'] == 123
        command('stop')
        assert server.poll() is None
        print(f'real PipeWire → Studio → Gorgon/Rack → JACK capture: peak {peak:.6f}; restore/relaunch/ownership passed')
    finally:
        command('stop', check=False)
        for client in (receiver, foreign, output):
            if client is not None:
                client.deactivate()
                client.close()
        server.terminate()
        server.wait(timeout=5)
