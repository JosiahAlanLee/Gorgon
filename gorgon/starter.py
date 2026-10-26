"""First-run material, created once. Existing scores and Rack patches stay editable."""
from __future__ import annotations

import json
import io
import os
from pathlib import Path
import tarfile

from .config import LaunchError

VISUAL = '''from hydrapy import *
STUDIO = {'title': 'Gorgon'}
z = Param('Dolly Z', .35, min=0, max=1)
roll = Param('Roll', 0, min=-45, max=45)
feedback = Param('Feedback', .25, min=0, max=.9)
set_bpm(138)
shape(8, .35, .012).repeat(3, 3).scale(1 + z * 2).rotate(roll * .01745329252).color(.7, .9, .65).out(o1)
src(o3).scale(1.008).rotate(.003).blend(src(o1), 1 - feedback).out(o3)
src(o1).blend(src(o3), .6).out(o0)
src(o3).edge().out(o2)
render(o0)
'''


def score():
    patterns = []
    for index in range(4):
        audio = [[None] * 64 for _ in range(4)]
        for row in range(0, 64, 2):
            note = 'C-2' if row % 8 == 0 else 'D-2' if row % 8 == 4 else 'F#2'
            audio[0][row] = dict(note=note, vel=100 if row % 4 == 0 else 65)
        patterns.append(dict(name=f'PATTERN {index + 1}', audio=audio,
                             easing=dict(z='smooth', roll='smooth', feedback='smooth'),
                             keys=dict(z=[dict(r=0, v=.35), dict(r=32, v=.65), dict(r=63, v=.35)],
                                       roll=[], feedback=[])))
    return dict(format='hydrapy-tracker-1', version=1, bpm=138, key=2, sound=False,
                loop=True, pattern=0, scene=0, modes=dict(program='o0', aux='o1'),
                mutes=[False]*4, bindings=dict(z='Dolly Z', roll='Roll', feedback='Feedback'), patterns=patterns)


def prepare(config):
    root = Path(config['workspace'])
    root.mkdir(parents=True, exist_ok=True)
    visual = root / config['studio']['patch']
    if config['studio']['patch'] == 'gorgon-visual.py' and not visual.exists():
        visual.write_text(VISUAL)
    state = root / '.hydrapy'
    state.mkdir(exist_ok=True)
    saved = state / 'tracker.json'
    if not saved.exists():
        saved.write_text(json.dumps(score(), indent=2))


def rack_patch(config, client, device):
    if config['rack']['patch']:
        return Path(config['rack']['patch'])
    path = Path(config['workspace']) / 'gorgon-rack.vcv'
    members = None
    if path.exists():
        data = path.read_bytes()
        if data[:4] == b'\x28\xb5\x2f\xfd':
            import zstandard
            with zstandard.ZstdDecompressor().stream_reader(io.BytesIO(data)) as reader:
                archive = reader.read()
            with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
                members = [(m, tar.extractfile(m).read() if m.isfile() else None) for m in tar.getmembers()]
            patch_data = next((content for member, content in members if member.name == 'patch.json'), None)
            if patch_data is None:
                raise LaunchError('Managed Rack archive has no patch.json')
            document = json.loads(patch_data)
        else:
            document = json.loads(data)
        captures = [m for m in document['modules'] if m.get('plugin') == 'GorgonBridge' and m.get('model') == 'Gorgon']
        playback = [m for m in document['modules'] if m.get('plugin') == 'Core' and m.get('model') == 'AudioInterface2']
        if len(captures) != 1 or len(playback) != 1:
            raise LaunchError('The starter\'s audio layout changed; set rack.patch to your saved patch to use its own routing')
        captures[0].setdefault('data', {}).setdefault('audio', {})['deviceName'] = client
        playback[0].setdefault('data', {}).setdefault('audio', {})['deviceName'] = device
    else:
        def audio(name):
            return dict(audio=dict(driver=4, deviceName=name, sampleRate=config['audio']['rate'],
                                   blockSize=config['audio']['quantum'], inputOffset=0, outputOffset=0))
        document = dict(version='2.6.4', modules=[
            dict(id=1, plugin='GorgonBridge', model='Gorgon', version='2.3.1', pos=[0, 0], params=[], data=audio(client)),
            dict(id=2, plugin='Core', model='AudioInterface2', version='2.6.4', pos=[32, 0], params=[], data=audio(device))],
            cables=[dict(id=i, outputModuleId=1, outputId=15+i, inputModuleId=2, inputId=i)
                    for i in range(2)])
    temp = path.with_name(path.name + f'.{os.getpid()}.tmp')
    try:
        encoded = json.dumps(document, indent=2).encode()
        if members is None:
            temp.write_bytes(encoded)
        else:
            import zstandard
            with open(temp, 'wb') as stream, zstandard.ZstdCompressor().stream_writer(stream) as compressed:
                with tarfile.open(fileobj=compressed, mode='w|') as tar:
                    for member, content in members:
                        if member.name == 'patch.json':
                            content = encoded
                            member.size = len(content)
                        tar.addfile(member, io.BytesIO(content) if content is not None else None)
        os.replace(temp, path)
    finally:
        temp.unlink(missing_ok=True)
    return path
