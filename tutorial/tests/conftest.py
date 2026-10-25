import pytest


def model(params=(), ins=(), outs=(), hp=10):
    return dict(name='x', hp=hp,
                params=[dict(index=i, name=n, min=lo, max=hi, default=lo, snap=False) for i, (n, lo, hi) in enumerate(params)],
                inputs=[dict(index=i, name=n) for i, n in enumerate(ins)],
                outputs=[dict(index=i, name=n) for i, n in enumerate(outs)])


@pytest.fixture
def catalog():
    return {
        'GorgonBridge': {'version': '2.1.0', 'models': {'Gorgon': model(
            [('Play / pause Gorgon', 0, 1), ('Back to the start', 0, 1), ('Timing offset', -100, 100)],
            ['Play / pause trigger', 'Reset trigger', 'CV 1 to a Gorgon control', 'CV 2 to a Gorgon control'],
            ['Clock', 'Reset (5 ms trigger)', 'Run', 'Velocity, lanes 1–4', 'Gate 1', 'Gate 2', 'Gate 3', 'Gate 4',
             'Dolly Z lane', 'Roll lane', 'Feedback lane', 'Gates 1–4 (polyphonic)', 'Pitch, lanes 1–4'], 12)}},
        'VastishLab': {'version': '2.3.1', 'models': {'Vastish': model(
            [('Master', 0, 1)], ['Pitch, 1 V/oct', 'Gate, up to eight voices'], ['Left audio', 'Right audio'], 54)}},
        'Core': {'version': '2.6.6', 'models': {'AudioInterface2': model([], ['To 1', 'To 2'], ['From 1', 'From 2'], 6)}},
        'GorgonTutorialProbe': {'version': '2.0.0', 'models': {'Director': model(hp=3)}},
    }
