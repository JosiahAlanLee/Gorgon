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
- holds every gate that was high on the sample before the change low for
  `retrigger_ms` (1 ms), so a hit the change lands on rises again instead of
  continuing the gate it interrupted;
- starts a **landing gate** when the playhead landed inside the gate window of
  its row and the follower hears the change while the playhead is still in that
  row (see below), so a change that reaches it late still plays the hit it
  landed on;
- clears every lane's held pitch and held velocity to 0;
- if it runs its own clock (the Rack module), jumps to the playhead, however
  small the change: only differences without a `transport_revision` change are
  drift to be trimmed.

A `revision` change, a controls change or a repeated snapshot is not a transport
change and does none of this.

### Landing gate

The **landing** is the beat the change starts from: the playhead's `anchor_beat`
after a `transport_revision` change, or, when only `moving` became true, the
beat where the playhead stood. Let `landing phase` be its row phase. If `landing
phase < gate`, and the follower's first sample after hearing the change is in
the same pattern and row, the landing gate lasts

    min(gate − landing phase, 1 − row phase) rows − retrigger_ms

from that sample, where `row phase` is that sample's and `retrigger_ms` is 1 ms,
so the gate falls before the next row starts. While it lasts and the playhead
stays in the landing row, `gate n` is high wherever lane `n` is **on**, except
during a retrigger. It ends early at the next transport change or when `moving`
becomes false.

For a follower that hears the change at once, the landing gate ends within the
phase-based gate and changes nothing. One that hears it later, up to the end of
the row, still plays the hit, starting late, for the length it would have had or
until 1 ms before the row ends; without the landing gate a change heard after
the window closed (54 ms at 138 BPM with the default `gate`) lost that row's
hits. A landing at or after `gate` in its row (resuming late in a row, say) adds
nothing: that hit had already ended. The hit still starts as late as the
change reached the follower, which for the Rack module is up to one bridge poll
(1/30 s) plus a round trip.

In version 3 the landing's candidate is the one whose span contains the
landing. `evaluate()` returns it with `at ≤ 0`, and its landing phase is
`−at / duration`; the rule above then decides, unchanged
([evaluator.md](evaluator.md#evaluate), vector V6).

## Cells

For lane `n` at the playhead's cell:

- **present**: the cell is not null and `vel > 0`. `AIR` with `vel > 0` is present.
  In version 3 a cell is present only on candidates that fire
  ([evaluator.md](evaluator.md#evaluate)): lane not muted by the score or the bus
  layer ([bus-io.md](bus-io.md)), condition holding, not dropped by a
  transform. "Muted" below is then judged once, at the candidate's `at`, not on
  every sample, so a mute that arrives while a hit sounds doesn't cut its gate. That needs the transport anchor, elapsed ticks and FILL
  as well as the beat. "Row" in the gate window and the landing gate means the
  candidate's span, `[at, at + duration)`, so each `ply` sub-step
  ([transforms.md](transforms.md)) gets its own gate.
- **on** = present and `moving` and lane `n` not muted.

While a lane is muted or the cell is silent, nothing below updates for that lane.

## Signals

| Signal | Value |
| --- | --- |
| `clock` | high while `frac(beat · ppqn) < 0.5` and `moving`. `ppqn` default 4 (sixteenths). In version 3, `beat` here is `b = (start tick + u) / TICKS`: the segment's start tick plus the elapsed ticks `u`, in beats, never wrapped at the cycle. It lines up with the cycle beat at every start, resume and seek and then keeps its period through every cycle wrap, so a cycle that isn't a whole number of clock periods (3 rows at rate 8 is 180 ticks, under half a 4 PPQN period) neither cuts a pulse short, merges two, nor holds `clock` high. The two rules coincide wherever a cycle is a whole number of beats, so versions 1 and 2, whose cycles are, are unaffected. |
| `reset` | high during the transport-change pulse, and while `(beat − cycle start) · 60 / bpm < reset_ms`, and `moving`. So every cycle wrap produces a pulse. |
| `run` | high while `moving`. |
| `gate n` | high while **on** and `row phase < gate`, and during a landing gate; low during a retrigger (above). `gate` default 0.5 (half a row). |
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

Version 3's signals are in `rack/tests/fixture3.txt` (`make_fixture.py --v3`,
[evaluator.md](evaluator.md#fixture)): per score, a stream of playheads and
`TrackerCV`'s output for it, which the Rack engine replays and matches on every
sample.
