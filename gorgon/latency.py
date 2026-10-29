"""Gate-to-envelope latency estimation for a recorded six-lane probe."""
from __future__ import annotations

from .config import LaunchError


def probe_score():
    # Separate hits by one second, with a quiet second before the first hit.
    lanes = []
    for _ in range(6):
        cells = [None] * 32
        for row in (4, 8, 12, 16, 20, 24, 28):
            cells[row] = dict(note='C-4', vel=100)
        lanes.append(dict(len=32, cells=cells, transforms=[]))
    return dict(format='hydrapy-tracker-3', version=1, bpm=60, key=2, sound=False,
                loop=True, pattern=0, scene=0, modes=dict(program='o0', aux='o1'),
                mutes=[False] * 6, chain=[0], seed=0,
                lanes=[dict(name=f'Lane {i + 1}') for i in range(6)],
                sounds=[dict(machine=None, params={}) for _ in range(6)],
                bindings=dict(z='', roll='', feedback=''),
                patterns=[dict(name='Latency probe', rate=1, bar=16, bars=2,
                               lanes=lanes, keys=dict(z=[], roll=[], feedback=[]),
                               easing=dict(z='hold', roll='hold', feedback='hold'), scenes=[])] + [None] * 127)


def measure(samples, rate, pairs=None):
    """Channels are samples x channels; default: mix 1/2, gate/audio pairs 3–14.

    Return delays to align to the slowest measured lane, avoiding speculative advances.
    Refuse silent, clipped-window or insufficient probes instead of inventing a latency.
    """
    import numpy as np
    pairs = pairs or [(2 + 2 * lane, 3 + 2 * lane) for lane in range(6)]
    results = []
    for gate_channel, sound_channel in pairs:
        gate, sound = samples[:, gate_channel], samples[:, sound_channel]
        if not np.isfinite(gate).all() or not np.isfinite(sound).all():
            raise LaunchError(f'Lane {len(results) + 1}: probe contains nonfinite samples')
        threshold = max(1e-5, float(np.max(gate)) * .5)
        edges = np.flatnonzero((gate[1:] > threshold) & (gate[:-1] <= threshold)) + 1
        rms_window = max(1, round(rate * .002))
        smooth = max(1, round(rate * .001))
        envelope = np.sqrt(np.convolve(sound.astype(float) ** 2, np.ones(rms_window) / rms_window, 'full')[:len(sound)])
        rise = np.diff(np.convolve(envelope, np.ones(smooth) / smooth, 'full')[:len(sound)])
        latencies = []
        for edge in edges:
            end = edge + round(.150 * rate)
            if end >= len(rise):
                continue
            segment = rise[edge:end]
            peak = int(np.argmax(segment))
            if envelope[edge:end].max() < 1e-5 or segment[peak] < 1e-7 or peak >= len(segment) - smooth:
                continue
            latencies.append(1000 * peak / rate)
        if len(latencies) < 3:
            raise LaunchError(f'Lane {len(results) + 1}: need at least three audible isolated gate/audio pairs')
        results.append(dict(lane=len(results) + 1, hits=len(latencies),
                            median_ms=float(np.median(latencies)),
                            p10_ms=float(np.percentile(latencies, 10)), p90_ms=float(np.percentile(latencies, 90))))
    target = max(row['median_ms'] for row in results)
    offsets = [round(target - row['median_ms'], 3) for row in results]
    if max(offsets) > 100:
        raise LaunchError('Measured spread exceeds the module\'s 100 ms delay limit')
    return dict(lanes=results, target_ms=target, laneOffsetMs=offsets)
