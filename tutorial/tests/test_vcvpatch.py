import json

import pytest

from gtut.vcvpatch import Patch, PatchError


def test_names_prefixes_and_indices(catalog, tmp_path):
    p = Patch(catalog)
    g = p.add('GorgonBridge', 'Gorgon', x=0)
    v = p.add('VastishLab', 'Vastish', gap=2)
    a = p.add('Core', 'AudioInterface2', x=0, y=1)
    p.cable(g, 'Pitch', v, 'pitch').cable(g, 'Gates 1', v, 'Gate')
    p.cable(v, 'Left audio', a, '#0').cable(v, '#1', a, 1)
    j = json.loads(p.save(tmp_path / 'x.vcv').read_text())
    assert [m['pos'] for m in j['modules']] == [[0, 0], [14, 0], [0, 1], [108, 0]]  # Director far right
    assert j['cables'][0] == dict(id=1, outputModuleId=1, outputId=12, inputModuleId=2, inputId=0)
    assert j['cables'][1]['outputId'] == 11  # 'Gates 1' is the polyphonic gates, not Gate 1
    assert p.explain()[0] == 'Gorgon.Pitch, lanes 1–4  ->  Vastish.Pitch, 1 V/oct'


def test_exact_name_beats_prefix(catalog):
    p = Patch(catalog)
    g = p.add('GorgonBridge', 'Gorgon', x=0)
    v = p.add('VastishLab', 'Vastish')
    p.cable(g, 'Gate 1', v, 'Gate')
    assert p.cables[0]['outputId'] == 4


def test_errors_list_what_exists(catalog):
    p = Patch(catalog)
    g = p.add('GorgonBridge', 'Gorgon', x=0)
    v = p.add('VastishLab', 'Vastish')
    with pytest.raises(PatchError, match='ambiguous'):
        p.cable(g, 'Gate', v, 'Gate')
    with pytest.raises(PatchError, match="have \\['Pitch"):
        p.cable(g, 'Clock', v, 'Nope')
    with pytest.raises(PatchError, match='outside'):
        p.set(g, 'Timing offset', 300)
    p.cable(g, 'Clock', v, 'Gate')
    with pytest.raises(PatchError, match='already has a cable'):
        p.cable(g, 'Run', v, 'Gate')


def test_scaled_params(catalog):
    p = Patch(catalog)
    g = p.add('GorgonBridge', 'Gorgon', x=0)
    p.set(g, 'Timing offset', 0.4, scaled=True)
    assert p.to_json()['modules'][0]['params'][2]['value'] == pytest.approx(-20)
