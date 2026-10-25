import os

import pytest

from gtut import audio, config

LDCONFIG = """\t3 libs found in cache `/etc/ld.so.cache'
\tlibjack.so.0 (libc6,x86-64) => /usr/lib/x86_64-linux-gnu/pipewire-0.3/jack/libjack.so.0
\tlibjack.so.0 (libc6,x86-64) => /lib/x86_64-linux-gnu/libjack.so.0
\tlibjack.so (libc6,x86-64) => /usr/lib/x86_64-linux-gnu/pipewire-0.3/jack/libjack.so
\tlibjackserver.so.0 (libc6,x86-64) => /usr/lib/x86_64-linux-gnu/pipewire-0.3/jack/libjackserver.so.0
\tlibjackserver.so.0 (libc6,x86-64) => /lib/x86_64-linux-gnu/libjackserver.so.0
"""


def cfg_with(tmp_path, **audio_over):
    p = tmp_path / 'config.yaml'
    lines = ['paths:', f'  work: {tmp_path / "work"}', 'audio:'] + [f'  {k}: {v}' for k, v in audio_over.items()]
    p.write_text('\n'.join(lines) + '\n')
    return config.load(p)


def test_real_libjack_skips_pipewire():
    assert audio.real_jack_libs(LDCONFIG) == {'libjack.so.0': '/lib/x86_64-linux-gnu/libjack.so.0',
                                              'libjackserver.so.0': '/lib/x86_64-linux-gnu/libjackserver.so.0'}
    assert audio.real_libjack(LDCONFIG.replace('/lib/x86_64-linux-gnu/libjack.so.0', '/x/pipewire/libjack.so.0')) is None


def test_jackd_mode_links_only_jack_libraries_first(tmp_path, monkeypatch):
    lib, server = tmp_path / 'libjack.so.0.0.28', tmp_path / 'libjackserver.so.0.0.28'
    lib.write_bytes(b''); server.write_bytes(b'')
    monkeypatch.setattr(audio, 'real_jack_libs', lambda text=None: {'libjack.so.0': str(lib), 'libjackserver.so.0': str(server)})
    cfg = cfg_with(tmp_path, server='jackd')
    env = audio.jack_env(cfg, {'LD_LIBRARY_PATH': '/opt/rack:/opt/x'})
    d = env['LD_LIBRARY_PATH'].split(':')
    assert d[1:] == ['/opt/rack', '/opt/x']
    assert sorted(os.listdir(d[0])) == ['libjack.so.0', 'libjackserver.so.0']
    assert os.readlink(os.path.join(d[0], 'libjack.so.0')) == str(lib)
    assert env['JACK_DEFAULT_SERVER'] == 'gtut'
    again = audio.jack_env(cfg, env)  # idempotent: no second copy of the directory
    assert again['LD_LIBRARY_PATH'] == env['LD_LIBRARY_PATH']


@pytest.mark.parametrize('over', [dict(server='pipewire'), dict(server='jackd', libjack='system')])
def test_loader_left_alone(tmp_path, monkeypatch, over):
    monkeypatch.setattr(audio, 'real_jack_libs', lambda text=None: {'libjack.so.0': '/lib/x86_64-linux-gnu/libjack.so.0'})
    env = audio.jack_env(cfg_with(tmp_path, **over), {'LD_LIBRARY_PATH': '/opt/rack'})
    assert env['LD_LIBRARY_PATH'] == '/opt/rack'


def test_stage_refuses_a_foreign_server():
    audio.check_own_server(['system:capture_1', 'system:playback_1'])
    audio.check_own_server(['(no ports)'])
    with pytest.raises(RuntimeError, match='not its own'):
        audio.check_own_server(['system:playback_1', 'Digi 003 Rack Multichannel:playback_FL'])
