"""Optional measured flash/gate alignment, independent of a particular musical piece."""
from __future__ import annotations

import json
import subprocess

from .config import LaunchError


# Lag 0 is the monotonic estimate; the measurement only corrects Studio's unmeasured video start-up
# (about +0.05 s on the render box). A window this narrow can't reach a beat alias: a lane that
# plays on the beat correlates as well one beat early or late (0.47 s at 128 BPM), so a wide
# search can lock onto the wrong beat with a confident score.
WINDOW = (-.25, .5)


def align(movie, capture, rate, gate_channel, record_time, first_audio_time, delay_ms=0, window=WINDOW):
    """Correlate positive brightness changes in the centre with a selected gate.

    Needs a patch whose picture visibly flashes at the selected lane's hits.
    Searches lags in ``window`` (seconds from the monotonic estimate) and, among peaks within 10%
    of the best, takes the one nearest the estimate.
    Returns audio time for video frame zero plus diagnostics, never edits sources.
    """
    import numpy as np
    probe = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_entries',
                            'stream=r_frame_rate', '-of', 'json', str(movie)], capture_output=True, text=True, check=True)
    num, den = map(int, json.loads(probe.stdout)['streams'][0]['r_frame_rate'].split('/'))
    fps = num / den
    raw = subprocess.run(['ffmpeg', '-v', 'error', '-i', str(movie), '-vf', 'scale=160:90',
                          '-f', 'rawvideo', '-pix_fmt', 'gray', '-'], capture_output=True, check=True).stdout
    frames = np.frombuffer(raw, np.uint8).reshape(-1, 90, 160)
    if len(frames) < 3:
        raise LaunchError('Measured A/V sync requires at least three video frames')
    centre = frames[:, 22:68, 53:107].mean(axis=(1, 2))
    vis = np.maximum(np.diff(centre, prepend=centre[0]), 0)
    if vis.std() < .01:
        raise LaunchError('No centre flashes detected for measured A/V sync')
    vis = (vis - vis.mean()) / vis.std()
    gate = capture[:, gate_channel - 1]
    threshold = max(1e-5, float(np.max(gate)) * .5)
    edges = np.flatnonzero((gate[1:] > threshold) & (gate[:-1] <= threshold)) + 1
    if len(edges) < 3:
        raise LaunchError('Measured A/V sync requires at least three gate edges')
    events = first_audio_time + edges / rate + delay_ms / 1000
    lags = np.arange(window[0], window[1] + 1e-9, 1 / fps / 4)
    scores = []
    for lag in lags:
        indices = np.round((events - record_time - lag) * fps).astype(int)
        indices = indices[(indices >= 0) & (indices < len(vis))]
        scores.append(float(vis[indices].mean()) if len(indices) >= 3 else -100.)
    scores = np.array(scores)
    top = float(scores.max())
    if top <= 0:
        raise LaunchError('No positive flash/gate correlation found')
    near = np.flatnonzero(scores >= .9 * top)
    best = int(near[np.argmin(np.abs(lags[near]))])     # of the near-ties, the one nearest the estimate
    far = np.abs(lags - lags[best]) > 3 / fps
    runner = float(scores[far].max()) if far.any() else None
    report = dict(method='centre flash/gate correlation', lag_s=float(lags[best]), score=float(scores[best]),
                  runner_up=runner, gate_channel=gate_channel, sound_delay_ms=delay_ms,
                  window_s=[float(window[0]), float(window[1])],
                  ambiguous=runner is not None and runner >= .9 * float(scores[best]))
    return record_time + float(lags[best]) - first_audio_time, report
