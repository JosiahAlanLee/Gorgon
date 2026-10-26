# Signal semantics

What a follower outputs at every beat, given a score and a playhead. The
reference implementation is `TrackerCV.render()` in HydraPy's `tracker_cv.py`;
the Rack engine (`rack/src/engine/GorgonEngine.hpp`) and the browser audition
synth implement the same rules. The fixture checks the first two agree to the
sample.

Terms from [score.md](score.md): beat, cycle, pattern index, row, row phase.

## State

`moving` = Studio `playing` **and** tracker `enabled`. Nothing pulses unless
`moving` is true.

A **transport change** is a change of `transport_revision`, or `moving`
becoming true (start or resume). At a transport change the follower:

- starts a reset pulse of `reset_ms` (5 ms), latched for its full length even if
  the playhead has already moved past it;
- clears every lane's held pitch and held velocity to 0;
- if it runs its own clock (the Rack module), jumps to the playhead, however
  small the change: only differences without a `transport_revision` change are
  drift to be trimmed.

A `revision` change, a controls change or a repeated snapshot is not a transport
change and does none of this.

## Cells

For lane `n` at the playhead's cell:

- **present**: the cell is not null and `vel > 0`. `AIR` with `vel > 0` is present.
- **on** = present and `moving` and lane `n` not muted.

While a lane is muted or the cell is silent, nothing below updates for that lane.

## Signals

| Signal | Value |
| --- | --- |
| `clock` | high while `frac(beat · ppqn) < 0.5` and `moving`. `ppqn` default 4 (sixteenths). |
| `reset` | high during the transport-change pulse, and while `(beat − cycle start) · 60 / bpm < reset_ms`, and `moving`. So every cycle wrap produces a pulse. |
| `run` | high while `moving`. |
| `gate n` | high while **on** and `row phase < gate`. `gate` default 0.5 (half a row). |
| `pitch n` | held. Updated to the cell's pitch whenever **on** and the note is pitched; `AIR` leaves it. `volts = (midi + key − 2 − pitch_zero) / 12`. `pitch_zero` default 60 (C4 = 0 V); 48 (C3) is the alternative. |
| `vel n` | held. Updated to the cell's velocity whenever **on**. Output `vel / 127 · high`. |
| `z`, `roll`, `feedback` | the lane's interpolated value at the fractional row (score.md), mapped linearly from the lane range to its volt range. Hold while paused or disabled. |

"Held" values survive pauses and mutes and are cleared only at a transport change.

## Levels

| | Direct CV (HydraPy) | Rack module |
| --- | --- | --- |
| `high` (clock, reset, run, gates, vel) | 5 V | 10 V default, 5 V selectable |
| `z`, `feedback` | 0–5 V | 0–`high` |
| `roll` | −5–5 V | −5–5 V, independent of `high` |
| `pitch n` | 1 V/oct | 1 V/oct |

## Timing

Every sample is evaluated from the playhead model in
[bridge-api.md](bridge-api.md), not from a video frame, so edges land on exact
samples. An `offset` (milliseconds) shifts the evaluation time: positive sends
voltages earlier than the picture, negative later. Followers may expose `ppqn`,
`gate` and `pitch_zero` as options; the defaults above are what the fixture
uses.

## Fixture

`rack/tests/fixture.txt` (header `GORGON_FIXTURE_2`) holds, for eight random
scores, the signals at a few thousand chosen beats as `TrackerCV` computes them,
including row and pulse boundaries, each key, `AIR`, zero velocity and mutes.
Regenerate it with the pinned HydraPy whenever this page changes:

```sh
python rack/tools/make_fixture.py > rack/tests/fixture.txt
```
