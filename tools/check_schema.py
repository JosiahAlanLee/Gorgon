#!/usr/bin/env python3
"""Check spec/score.schema.json against HydraPy's reference validator.

Every score HydraPy accepts must satisfy the schema: the default score, the
tracker's demo score, a score with scene changes, and generated random scores.
And where the schema can express a rule, it must reject what validate() rejects.
Requires HydraPy and jsonschema installed.
"""
import copy
import json
import random
import sys
from pathlib import Path

from jsonschema import Draft202012Validator

from hydrapy.studio_tracker import Tracker, validate

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'rack' / 'tools'))
import make_fixture  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
schema = json.loads((ROOT / 'spec' / 'score.schema.json').read_text())
Draft202012Validator.check_schema(schema)
checker = Draft202012Validator(schema)

accepted = [Tracker().document]
with_scenes = copy.deepcopy(Tracker().document)
with_scenes['patterns'][0]['scenes'] = [{'r': 0, 'name': 'drift', 'fade': 0}, {'r': 16, 'name': 'fracture', 'fade': 8}]
accepted.append(validate(with_scenes))
rng = random.Random(7)
accepted += [make_fixture.random_score(rng) for _ in range(200)]

# Version-2 controls are sparse; missing values are deliberately not zero-filled.
for document in list(accepted):
    d = copy.deepcopy(document)
    d['format'] = 'hydrapy-tracker-2'
    d['sounds'] = [{'machine': None, 'params': {}} for _ in range(4)]
    d['sounds'][0] = {'machine': 16, 'params': {'p2': 50}}
    d['patterns'][0]['audio'][0][0] = {'note': 'C-4', 'vel': 90, 'locks': {'machine': 48, 'p1': 0, 'p8': 127}}
    accepted.append(validate(d))

failures = 0
for index, document in enumerate(accepted):
    errors = list(checker.iter_errors(document))
    if errors:
        failures += 1
        print(f'accepted score {index} fails the schema: {errors[0].message[:160]}')


def mutate(label, change):
    global failures
    document = copy.deepcopy(Tracker().document)
    change(document)
    schema_rejects = bool(list(checker.iter_errors(document)))
    try:
        validate(document)
        validator_rejects = False
    except ValueError:
        validator_rejects = True
    if schema_rejects != validator_rejects:
        failures += 1
        print(f'{label}: schema rejects={schema_rejects}, validate() rejects={validator_rejects}')


mutate('bpm 300', lambda d: d.__setitem__('bpm', 300))
mutate('key 3', lambda d: d.__setitem__('key', 3))
mutate('key false', lambda d: d.__setitem__('key', False))
mutate('key true', lambda d: d.__setitem__('key', True))
mutate('pattern 4', lambda d: d.__setitem__('pattern', 4))
mutate('note H4', lambda d: d['patterns'][0]['audio'][0].__setitem__(0, {'note': 'H4', 'vel': 10}))
mutate('note B#4', lambda d: d['patterns'][0]['audio'][0].__setitem__(0, {'note': 'B#4', 'vel': 10}))
mutate('note A9', lambda d: d['patterns'][0]['audio'][0].__setitem__(0, {'note': 'A9', 'vel': 10}))
mutate('note G#9', lambda d: d['patterns'][0]['audio'][0].__setitem__(0, {'note': 'G#9', 'vel': 10}))
mutate('note G9 ok', lambda d: d['patterns'][0]['audio'][0].__setitem__(0, {'note': 'G9', 'vel': 10}))
mutate('note F#9 ok', lambda d: d['patterns'][0]['audio'][0].__setitem__(0, {'note': 'F#9', 'vel': 10}))
mutate('vel 200', lambda d: d['patterns'][0]['audio'][0].__setitem__(0, {'note': 'C4', 'vel': 200}))
mutate('roll 90', lambda d: d['patterns'][0]['keys']['roll'].__setitem__(0, {'r': 0, 'v': 90}))
mutate('feedback 1', lambda d: d['patterns'][0]['keys']['feedback'].__setitem__(0, {'r': 0, 'v': 1}))
mutate('easing bounce', lambda d: d['patterns'][0]['easing'].__setitem__('z', 'bounce'))
mutate('scene name with space', lambda d: d['patterns'][0].__setitem__('scenes', [{'r': 0, 'name': 'has space', 'fade': 0}]))
mutate('scene extra key', lambda d: d['patterns'][0].__setitem__('scenes', [{'r': 0, 'name': 'a', 'fade': 0, 'x': 1}]))
mutate('scene fade 65', lambda d: d['patterns'][0].__setitem__('scenes', [{'r': 0, 'name': 'a', 'fade': 65}]))
mutate('program o4', lambda d: d['modes'].__setitem__('program', 'o4'))
mutate('three mutes', lambda d: d.__setitem__('mutes', [False, False, False]))
mutate('binding too long', lambda d: d['bindings'].__setitem__('z', 'x' * 121))

def sound_change(d, locks):
    d['format'] = 'hydrapy-tracker-2'
    d['sounds'] = [{'machine': None, 'params': {}} for _ in range(4)]
    d['patterns'][0]['audio'][0][0] = {'note': 'C-4', 'vel': 90, 'locks': locks}


mutate('v1 sounds rejected', lambda d: d.__setitem__('sounds', [{'machine': None, 'params': {}}] * 4))
mutate('v1 locks rejected', lambda d: d['patterns'][0]['audio'][0].__setitem__(0, {'note': 'C-4', 'vel': 90, 'locks': {}}))
for locks in [None, [], {'p1': True}, {'p1': 128}, {'p1': 1.5}, {'p0': 0}, {'machine': None}, {'machine': 192}, {'unknown': 1}]:
    mutate('invalid locks '+repr(locks), lambda d, value=locks: sound_change(d, value))

print(f'{len(accepted)} accepted scores checked, {failures} failure(s)')
sys.exit(1 if failures else 0)
