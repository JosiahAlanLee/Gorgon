# Pattern timing proposal

**Status: proposal, not implemented.** The current score schema and players still
support formats 1 and 2 with fixed sixteenth-note rows. This document proposes
the next timing contract; it does not change those formats or their fixtures.
Written against HydraPy `0bac5401f2e7bd84da3c17e8363e9436e9163003` and Gorgon
`2ad926e92ef2b8958e0ff0ff7ee40582b884c60e`.

## First scope

Give each of the four patterns its own **pattern rate**, while retaining its
64 rows, four audio lanes and existing visual automation/scene lanes. The
transport BPM remains quarter notes per minute. Rate changes how many rows fit
into one quarter-note beat; it does not multiply the master tempo.

| Pattern rate | Rows per quarter note | One row | 64-row span in quarter-note beats | Bars in 4/4 |
| --- | --- | --- | --- | --- |
| ¼× | 1 | Quarter note | 64 | 16 |
| ½× | 2 | Eighth note | 32 | 8 |
| 1× | 4 | Sixteenth note | 16 | 4 |
| 2× | 8 | Thirty-second note | 8 | 2 |
| 4× | 16 | Sixty-fourth note | 4 | 1 |
| 8× | 32 | 128th note | 2 | ½ |

A faster rate provides denser placement and a shorter pattern, not more events
or more rows. A slower rate gives the same rows a longer span. Entry Advance
continues to control the cursor only. Note duration, ties, retrigger counts,
microtiming, triplet grids and independent per-track rates are separate changes.

Starting with binary rates also keeps every pattern length an integral number
of quarter-note beats. A triplet grid with exactly 64 rows needs further rules
for partial-beat loop boundaries and the master clock's phase.

## Proposed score representation

Use a new format, provisionally `hydrapy-tracker-3`, so existing players reject
scores whose timing they cannot perform. Version 3 retains version 2's four
sound defaults and per-hit locks and requires a `rate` on every pattern:

```json
{"rate": 2}
```

The allowed numeric values are exactly `0.25`, `0.5`, `1`, `2`, `4`, `8`.
Booleans, strings, null, non-finite values and other rates are rejected. Arrays
still contain all 64 cells, and automation/scene rows remain integers 0–63.

Versions 1 and 2 always play at 1× and retain their accepted data and playback
semantics. Their unknown metadata remains metadata: a player must not interpret
a `rate` key in an old-format document as permission to change its timing.
The editor upgrades explicitly on the first rate edit, initializes all other
patterns to 1×, and supplies unspecified sound defaults if upgrading version 1.
Undo restores the previous format, sound defaults and pattern metadata. Merely
opening or saving an old score does not upgrade it.

The new format and validation rules must be added to the normative score schema
in the contract PR before implementation lands. No new format is accepted by
the schema in this proposal PR.

## One conversion between beats and rows

For pattern `p`, let `a[p]` be its rate, `R[p]` its rows per quarter-note beat,
`L[p]` its length in beats and `S[p]` its start in the song:

```text
R[p] = 4 * a[p]
L[p] = 64 / R[p]
S[0] = 0
S[p] = sum(L[q] for q < p)
song_length = sum(L[p] for p in 0..3)
```

All public beat values remain quarter notes. When playing the full song,
normalize the playhead into `[0, song_length)` and choose the unique pattern
whose half-open interval `[S[p], S[p] + L[p])` contains it. When looping pattern
`p`, normalize into that pattern's interval instead. Within that pattern:

```text
row_position = (beat - S[p]) * R[p]
row = floor(row_position)
row_phase = row_position - row
beat_for_row(p, r) = S[p] + r / R[p]
```

The end of a pattern belongs to the next pattern, or wraps at the end of the
playing cycle. Floating-point boundary handling must not expose a row outside
0–63. Every implementation uses these conversions rather than independent
hard-coded divisions by 16 or multiplications by 4.

For rates `[1, 2, 0.5, 4]`, the pattern lengths are `[16, 8, 32, 4]`, starts
`[0, 16, 24, 56]`, and song length 60 beats. Pattern 2's row 8 is at beat 17;
pattern 3's row 4 is at beat 26. Four patterns at 1× reproduce today's 64-beat
song exactly.

## Sound, visual automation and clocks

- A note/velocity cell and its sound locks produce one event at `beat_for_row`.
  Rests, velocity zero, mutes, fresh-default resolution and linked Rack packet
  ownership retain the sound-lock contract. Rate is not a machine parameter lock.
- Automation keys and scene changes remain attached to the same pattern rows
  as the notes. Automation evaluates at the fractional `row_position`, retaining
  hold/linear/smooth interpolation. A faster pattern advances those keys faster.
- A scene fade remains measured in rows. Convert its length to beats using the
  rate of the pattern containing its starting scene event, then apply the
  existing clipping at the next scene event. Crossing into a differently paced
  pattern does not change a fade that has already started.
- Renderer frame rate and the patch's elapsed-seconds time do not change.
  A patch's tracker beat/row signals follow the musical playhead. Existing
  row-bound visual automation therefore shares the new timing; this does not
  add conditional visual triggers or a separate visual sequencer.
- CLOCK PPQN remains independent of pattern rate. Existing Clock settings
  continue to count pulses per quarter note. RESET/RUN semantics and voltage
  levels are retained, with reset windows measured in seconds/master beats.
- Gate length remains a fraction of the current row, so its duration changes
  with pattern rate. It is not a tie or a per-note duration. A drum's decay or
  release can still be controlled by its sound parameters.
- ByteBus still samples its selected source at CLOCK edges. Faster note grids
  do not implicitly accelerate its strobe or guarantee delivery of every hit
  at a slower PPQN. The implementation must offer 32 PPQN in Rack's Clock menu
  so the fastest proposed row grid can be sampled once per row. Default PPQN
  and direct CV port maps remain unchanged.

## Editing and transport

Selecting an edit pattern remains separate from seeking playback. Seeking to a
row, launching a pattern, loop boundaries, recorded/exported duration and the
browser scheduler all use the conversions above. The editor shows both the
rate and resulting span; beat/bar markings derive from musical beats rather
than fixed groups of four/sixteen rows.

Changing a pattern rate is a deliberate transport discontinuity. Capture the
playing pattern and its elapsed quarter-note beats within that pattern before
the edit, then re-anchor to that same pattern in the new score, wrapping the
elapsed offset into its new length. This preserves pattern identity and
quarter-beat phase; it does not preserve the old row number when its rate
changes. It increments the score and transport revisions once. Reapplying the
same rate is not a discontinuity. The same policy applies to undo/redo.

All existing players/followers reset or cancel queued work as they do for a
seek. They do not replay notes crossed by a timing edit or play old scheduled
notes late. An ordinary passage between differently paced patterns is not a
seek and does not create an extra transport revision/reset. The existing reset
pulse at a playing-cycle wrap remains governed by the CV contract.

## Implementation and acceptance sequence

1. Finish the normative format/schema, beat/row and CV contract in a spec-only
   PR. Include current-format examples and the mixed-rate boundary cases.
2. Implement HydraPy's validator, shared timing helpers, editor, browser
   scheduler, direct CV, scene/automation evaluation and export duration. Test
   legacy score preservation, upgrade/undo and live rate-edit discontinuities.
3. After HydraPy merges, implement Rack's parser/evaluator/follower/display,
   move its HydraPy pin and regenerate the conformance fixture together. Add
   rate cases to the fixture while retaining the legacy comparisons.

Required cross-implementation checks include every rate at 60/120/200 BPM;
row starts and values just before/after boundaries; mixed-rate songs; looping
all four selectable patterns; delayed bridge updates; tempo changes; pause,
seek and rate edits; scene fades across pattern boundaries; held CV values;
ByteBus sampling; and gate/velocity/pitch/lock tuples through the Rack receiver.
The audio and visual row positions must agree during playback and export.

This proposal defines the first timing feature. Conditional/algorithmic event
selection, per-track cycles and diffusion integration follow separately.
