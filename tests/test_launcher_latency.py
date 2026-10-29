import pytest

np = pytest.importorskip('numpy')
from gorgon.config import LaunchError
from gorgon.latency import measure


@pytest.mark.parametrize('gate_level', [.5, 1, 10])
def test_measured_offsets_and_silence(gate_level):
    sr = 48000
    audio = np.zeros((sr * 5, 16))
    delays = [0, 5, 10, 15, 20, 24]
    for lane, delay in enumerate(delays):
        for second in (1, 2, 3, 4):
            edge = second * sr
            audio[edge:edge + 480, 2 + lane * 2] = gate_level
            start = edge + delay * 48
            audio[start:start + 1200, 3 + lane * 2] = 1
    report = measure(audio, sr)
    for actual, delay in zip(report['laneOffsetMs'], delays):
        assert actual == pytest.approx(24 - delay, abs=.1)
    with pytest.raises(LaunchError, match='Lane 1'):
        measure(np.zeros_like(audio), sr)


def test_probe_is_a_valid_six_lane_score():
    tracker = pytest.importorskip('hydrapy.tracker_v3')
    from gorgon.latency import probe_score
    document = tracker.validate(probe_score())
    assert len(document['patterns']) == 128
    assert tracker.geometry(document['patterns'][0])[0] == 480
