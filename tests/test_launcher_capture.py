from pathlib import Path

import pytest

from gorgon import capture, cli
from gorgon.config import LaunchError, defaults


def options(**changes):
    result = dict(command='render', seconds=10, warmup=0, jack_timeout=10000, output='/tmp/new-run',
                  plugin_dir=[], source=[], link=[], optical_gain=.5, lead_blocks=2, av_offset_ms=0)
    result.update(changes)
    return result


def profile():
    config = defaults()
    config['rack']['patch'] = '/tmp/patch.vcv'
    return config


def test_private_commands_and_fullscreen():
    config = profile()
    out = Path('/tmp/run with spaces')
    commands = capture.commands(config, options(), out, 'private-name')
    assert commands['jack'] == ['jackd', '--no-realtime', '-t', '10000', '-n', 'private-name',
                                '-d', 'dummy', '-r', '48000', '-p', '1024']
    assert commands['rack'][:5] == ['xvfb-run', '-a', '-n', '147', config['rack']['command'][0]]
    assert str(out / 'capture.vcv') in commands['rack']
    assert '--no-native-output' in commands['studio']
    live = capture.commands(config, options(command='live', native_output='DP-5'), out, 'private-name')
    assert live['studio'][0] != 'xvfb-run'
    assert live['studio'][-2:] == ['--native-output', 'DP-5']


def test_real_server_requires_explicit_opt_in():
    config = profile()
    capture.validate(config, options())
    capture.validate(config, options(jack_server='private_named'))
    with pytest.raises(LaunchError, match='Only live'):
        capture.validate(config, options(hardware=True, jack_server='default'))
    capture.validate(config, options(command='live', native_output='DP-5', hardware=True, jack_server='default'))
    with pytest.raises(LaunchError):
        capture.validate(config, options(seconds=float('nan')))


def test_patch_routing_and_offsets_are_private():
    original = dict(modules=[dict(plugin='GorgonBridge', model='Gorgon', data=dict(laneOffsetMs=[24]*6)),
                             dict(plugin='Core', model='AudioInterface16')])
    prepared = capture.prepare_patch(original, 'studio42', 'gorgon_capture', calibrate=True)
    assert original['modules'][0]['data']['laneOffsetMs'] == [24]*6
    assert prepared['modules'][0]['data']['audio']['deviceName'] == 'studio42'
    assert prepared['modules'][0]['data']['laneOffsetMs'] == [0]*6
    assert prepared['modules'][1]['data']['audio']['deviceName'] == 'gorgon_capture'
    with pytest.raises(LaunchError, match='exactly one'):
        capture.prepare_patch(dict(modules=[]), 'studio', 'capture')


def test_cli_dispatch(tmp_path, monkeypatch):
    calls = []
    monkeypatch.setattr(capture, 'invoke', lambda config, args: calls.append(args) or 0)
    assert cli.main(['--config', str(tmp_path / 'profile.json'), 'render', 'score.json',
                     '--output', str(tmp_path / 'out'), '--seconds', '3', '--link', 'a:out', 'b:in']) == 0
    assert calls[0]['link'] == [['a:out', 'b:in']]
    assert calls[0]['seconds'] == 3
    with pytest.raises(SystemExit):
        cli.main(['live', 'score.json', '--output', str(tmp_path / 'out'), '--native-output', 'DP-5'])


def test_archive_rewrite_keeps_embedded_assets(tmp_path):
    import io
    import json
    import tarfile
    import zstandard
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode='w') as archive:
        for name, data in [('patch.json', b'{"modules": []}'), ('assets/voice.wav', b'example asset')]:
            member = tarfile.TarInfo(name)
            member.size = len(data)
            archive.addfile(member, io.BytesIO(data))
    source, target = tmp_path / 'original.vcv', tmp_path / 'copy.vcv'
    source.write_bytes(zstandard.ZstdCompressor().compress(raw.getvalue()))
    capture.write_patch(source, {'modules': [{'id': 1}]}, target)
    assert capture.read_patch(source) == {'modules': []}
    assert capture.read_patch(target) == {'modules': [{'id': 1}]}
    with zstandard.ZstdDecompressor().stream_reader(io.BytesIO(target.read_bytes())) as reader:
        with tarfile.open(fileobj=io.BytesIO(reader.read())) as archive:
            assert archive.extractfile('assets/voice.wav').read() == b'example asset'


def test_flash_gate_sync_recovers_known_lag(monkeypatch):
    import json
    from types import SimpleNamespace
    np = pytest.importorskip('numpy')
    from gorgon.avsync import align
    fps, rate = 30, 1000
    gate = np.zeros((8000, 1))
    frames = np.zeros((240, 90, 160), dtype=np.uint8)
    for second in (1, 2.3, 3.9, 5.2, 6.8):
        gate[round(second * rate):round(second * rate) + 50, 0] = 1
        frame = round((second - .2) * fps)
        frames[frame, 22:68, 53:107] = 255
    def run(argv, **kwargs):
        if argv[0] == 'ffprobe':
            return SimpleNamespace(stdout=json.dumps({'streams': [{'r_frame_rate': '30/1'}]}))
        return SimpleNamespace(stdout=frames.tobytes())
    monkeypatch.setattr('gorgon.avsync.subprocess.run', run)
    offset, report = align('video.mp4', gate, rate, 1, 100, 100)
    assert offset == pytest.approx(.2, abs=1 / fps)
    assert report['score'] > 0


def test_hardware_pipewire_uses_existing_launcher_wrapper(monkeypatch):
    calls = []
    config = profile()
    monkeypatch.setattr('gorgon.capture.importlib.util.find_spec', lambda name: True)
    monkeypatch.setattr('gorgon.capture.shutil.which', lambda name: '/bin/' + name)
    monkeypatch.setattr('gorgon.audio.ensure_server', lambda cfg: calls.append(('probe', cfg['audio']['start_services'])))
    monkeypatch.setattr('gorgon.capture.subprocess.call', lambda argv, env: calls.append(('spawn', argv)) or 0)
    assert capture.invoke(config, options(command='live', native_output='DP-5', hardware=True)) == 0
    assert calls[0] == ('probe', False)
    assert calls[1][1][:2] == ['pw-jack', __import__('sys').executable]
