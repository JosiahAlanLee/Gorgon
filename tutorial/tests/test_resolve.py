import pytest

from gtut.rack import RackError, find_module, resolve

LAYOUT = {'modules': [
    {'id': 7, 'model': 'Vastish', 'name': 'Vastish', 'grid': [30, 0], 'box': [0, 0, 1, 1], 'ports': [], 'params': []},
    {'id': 3, 'model': 'Gorgon', 'name': 'Gorgon', 'grid': [0, 0], 'box': [10, 20, 180, 380],
     'ports': [{'type': 'output', 'index': 0, 'name': 'Clock', 'centre': [32, 266], 'cables': 0},
               {'type': 'output', 'index': 4, 'name': 'Gate 1', 'centre': [40, 300], 'cables': 0},
               {'type': 'output', 'index': 11, 'name': 'Gates 1–4 (polyphonic)', 'centre': [160, 334], 'cables': 0},
               {'type': 'input', 'index': 2, 'name': 'CV 1 to a Gorgon control', 'centre': [40, 220], 'cables': 0}],
     'params': [{'index': 2, 'name': 'Timing offset', 'centre': [168, 150]}]},
    {'id': 9, 'model': 'Split', 'name': 'Split', 'grid': [12, 0], 'box': [0, 0, 1, 1], 'ports': [], 'params': []},
    {'id': 10, 'model': 'Split', 'name': 'Split', 'grid': [16, 0], 'box': [0, 0, 1, 1], 'ports': [], 'params': []},
]}


def test_modules_in_reading_order():
    assert find_module(LAYOUT, 'Split')['id'] == 9
    assert find_module(LAYOUT, 'Split#2')['id'] == 10
    with pytest.raises(RackError, match='only 2'):
        find_module(LAYOUT, 'Split#3')


def test_ports_and_params():
    assert resolve(LAYOUT, 'Gorgon.out.Clock')[1]['index'] == 0
    assert resolve(LAYOUT, 'Gorgon.out.Gate 1')[1]['index'] == 4
    assert resolve(LAYOUT, 'Gorgon.out.Gates')[1]['index'] == 11
    assert resolve(LAYOUT, 'Gorgon.out#11')[1]['index'] == 11
    assert resolve(LAYOUT, 'Gorgon.in.CV 1')[2] == 'input'
    assert resolve(LAYOUT, 'Gorgon.param.Timing')[1]['index'] == 2
    mod, item, kind = resolve(LAYOUT, 'Gorgon')
    assert mod['id'] == 3 and item is None


def test_ambiguity_is_an_error():
    with pytest.raises(RackError, match='ambiguous'):
        resolve(LAYOUT, 'Gorgon.out.Gate')
