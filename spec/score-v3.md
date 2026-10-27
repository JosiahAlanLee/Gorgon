# Score format 3: lanes and timing

**Status: contract, part 1 of 4; implemented** in the Rack module
`GorgonBridge` 2.4.0 and HydraPy 0.1.0 at `3c01f32` (the
[`hydrapy.pin`](../hydrapy.pin)). This is A-spec-1 of the
[roadmap](../docs/ROADMAP.md) (§2.A, §4). It defines the shape and timeline of
format `hydrapy-tracker-3`. Cell conditions and the evaluator (A-spec-2), lane
transforms (A-spec-3) and bus I/O (A-spec-4) extend the same format in their own
pages. All four have merged, and both implementations accept version 3 beside
versions 1 and 2 ([score.md](score.md)). Written against HydraPy
`e5999b2282bd9766b6f7152b4278c3da4e72ed8e` and Gorgon `9a2cb23`; lane
`transforms` added by [transforms.md](transforms.md) (A-spec-3).

Its five design proposals (the chain, the looped beat origin, immediate
re-anchoring, lanes restarting with their pattern, empty-pattern defaults) were
ratified on 2026-10-26 in the review of #41, with the amendments recorded here
and in [evaluator.md](evaluator.md) (A-spec-2).

It carries [pattern-timing.md](pattern-timing.md)'s rates, beat/row conversion
and transport-discontinuity rules into version 3, and replaces that proposal's
fixed 64 rows and four lanes.

## Decisions this page encodes

Ratified by the user on 2026-10-26 (roadmap §5):

- **Six lanes, because the Monomachine has six tracks.** Six is validated by
  count, not fixed by the shape: lane-indexed values are lists, so a later
  format can change the count without changing the model.
- **128 patterns, in eight banks of sixteen** (A–H, 1–16), replacing the
  roadmap's default of four. This needs a playing order, so version 3 adds a
  **chain** (below), ratified with the rest of this page.

## Shape

```
format     "hydrapy-tracker-3"
bpm, key, sound, modes, bindings    as version 2 (score.md)
pattern    0–127                    the selected pattern
loop       bool                     true: the selected pattern repeats; false: the chain plays
chain      [0–127 …]                1–256 pattern indices, the song order; repeats allowed
seed       0 – 2³² − 1              drives every probability (evaluator.md)
mutes      [bool ×6]                one per lane
lanes      [{name} ×6]              display names, 0–24 characters
sounds     [sound ×6]               as version 2's sounds (sound-locks.md), one per lane
patterns   [pattern | null ×128]    index = bank · 16 + number − 1
```

A pattern:

```
name       1–40 characters
rate       0.25 | 0.5 | 1 | 2 | 4 | 8    rows per quarter note = 4 · rate
bar        1–32                          rows per bar
bars       1–8                           bars per pattern; rows = bar · bars ≤ 128
lanes      [{len, cells, transforms} ×6] one per lane
             len        1–rows            the lane repeats every len rows
             cells      [cell ×len]       null or {note, vel, locks?, cond?}; cond in evaluator.md
             transforms [node …]          at most 8, applied in order; [] for none (transforms.md)
keys       {z, roll, feedback}            as version 2; r 0–(rows − 1)
easing     {z, roll, feedback}            as version 2
scenes     [{r, name, fade}…]             optional, as version 2; r 0–(rows − 1), fade 0–rows
```

- **Codes.** Pattern `i` is shown as bank `"ABCDEFGH"[⌊i / 16⌋]` and number
  `i mod 16 + 1`: index 0 is `A01`, 17 is `B02`, 127 is `H16`.
- **Empty patterns.** A `null` entry is an empty pattern: rate 1, bar 16,
  bars 4, every lane `len` 64 with all cells null and no transforms, no keys,
  `smooth` easing on all three lanes, no scenes, named by its code. It plays as four bars
  of silence at rate 1, with each automation lane at its default. An editor writes the object on the first
  edit, and that object must equal these defaults exactly; only the edit itself
  changes it. Keeping empty entries in the chain arithmetic keeps `S[]`
  independent of content.
- **Cells, notes, velocity, locks and automation** keep their version-2
  meanings ([score.md](score.md), [sound-locks.md](sound-locks.md)). A lane's
  `sounds` entry resolves its hits as a version-2 lane's does.
- **Unknown keys** at the top level and inside a pattern are tolerated and
  preserved, as in versions 1 and 2. Non-finite numbers are rejected everywhere.
- **Validation that a schema cannot express**, for `validate()` and every
  reader: `bar · bars ≤ 128`; each lane's `len ≤ rows` and `cells` length equal
  to `len`; key and scene rows below `rows`; scene `fade ≤ rows`.

## Time

All public beat values are quarter notes, as before. For pattern `p`:

```
R[p]    = 4 · rate[p]          rows per quarter note
rows[p] = bar[p] · bars[p]
L[p]    = rows[p] / R[p]       length in beats
```

### The timeline

The playhead is a beat on a **timeline**, wrapped into its **cycle**:

- **`loop` true:** the timeline is the selected pattern alone. The cycle is
  `[0, L[pattern])`, and beat 0 is the pattern's first row.
- **`loop` false:** the timeline is the chain. Entry `i` plays pattern
  `chain[i]` over `[S[i], S[i] + L[chain[i]])`, with `S[0] = 0` and
  `S[i] = S[i − 1] + L[chain[i − 1]]`. The cycle is `[0, C)`, `C = S[n]` for a
  chain of `n` entries.

Within a cycle the playhead's beat `b` lies in exactly one entry (in loop mode,
the single entry `S = 0`). The end of an entry belongs to the next, or wraps at
the end of the cycle. With `p` that entry's pattern:

```
row_position  = (b − S) · R[p]
row           = min(⌊row_position⌋, rows[p] − 1)
row_phase     = row_position − ⌊row_position⌋
lane_row[n]   = row mod len[p][n]
beat_for_row  = S + r / R[p]        the beat of row r in that entry
bar_in_pattern = ⌊row / bar[p]⌋
```

The `min` keeps floating-point rounding at an entry's end from exposing a row
outside the pattern. Every implementation uses these conversions rather than
dividing by 16 or multiplying by 4. The bar counter that conditions read, built
on `bar_in_pattern` and the chain, is in [evaluator.md](evaluator.md). The
evaluator does this arithmetic in integer ticks, 1920 per quarter note, where a
row is `1920 / R[p]` ticks and every row boundary is exact
([evaluator.md](evaluator.md#ticks)); signals and automation still read the
real-valued beat.

### Lanes

Lane `n`'s cell for the playhead is `cells[lane_row[n]]`. A lane shorter than
its pattern repeats within it, so lanes of different lengths drift against the
bar (polymeter). A lane restarts with its pattern: `lane_row` is 0 wherever
`row` is 0. A lane longer than the pattern is invalid (`len ≤ rows`).

This is Elektron's per-track length with the master length equal to the
pattern length, their default. It does not exclude the free-running case
(master length infinite, a 7-row hat that never resets): that is also stateless,
`lane_row = (v · rows + row) mod len` with `v` the earlier plays in the visit
([evaluator.md](evaluator.md)), and can be a per-lane flag later without changing
the timeline.

### Examples

These patterns and chain are used throughout:

| Index | Code | rate | bar | bars | rows | R | L (beats) |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 0 | A01 | 1 | 16 | 4 | 64 | 4 | 16 |
| 1 | A02 | 2 | 16 | 2 | 32 | 8 | 4 |
| 16 | B01 | 0.5 | 8 | 4 | 32 | 2 | 16 |

`chain = [0, 1, 1, 16]`: starts `S = [0, 16, 20, 24]`, cycle `C = 40`.

- Beat 21.5 is in entry 2 (`S = 20`, A02): row position `1.5 · 8 = 12`, row 12,
  phase 0, bar 0. Beat 17.5 is the same row of entry 1.
- Beat 30 is in entry 3 (B01): row position `6 · 2 = 12`, row 12, bar 1.
  Beat 41 wraps to beat 1, row 4 of A01.
- In A01, a lane with `len` 7 plays its cells 0–6 on rows 0–6, 7–13, …, 56–62,
  then cell 0 on row 63 and again on row 0 of the next pass.
- `loop` true with `pattern` 1: the cycle is `[0, 4)`. Beat 5.25 wraps to 1.25:
  row 10, phase 0.
- Rate 8 at 200 BPM: a row is `60 / 200 / 32` s, about 9.4 ms, and the
  longest pattern (128 rows) is 4 beats.

## Signals, automation and scenes

- **Signals** follow [cv-semantics.md](cv-semantics.md) with lanes `n` = 1–6,
  `row` and `row_phase` from the timeline above, and the cell taken at
  `lane_row[n]`. Gate length stays a fraction of the current row, so it scales
  with rate. `reset` pulses at every cycle wrap: the end of the looped pattern,
  or of the chain. Clock PPQN stays pulses per quarter note and is independent
  of rate; the Rack module must offer 32 PPQN so the fastest row grid can be
  clocked once per row. Six gate, pitch and velocity outputs on the Rack module,
  and two more direct CV gates in HydraPy, are those implementations' changes.
- **Automation** evaluates at the fractional `row_position` with version 2's
  interpolation. Keys stay per pattern: the last key of one entry does not
  interpolate into the next entry, including the same pattern repeated.
- **Scenes** are collected along the cycle in order, once per entry, so a
  pattern that appears twice in the chain contributes its changes twice. A
  `fade` is measured in rows of the pattern containing its change and converted
  to beats with that pattern's rate; it is then clipped at the next change, as
  in version 2, even if the next change is in a differently paced entry.

## Transport

These change the timeline, so each is a **transport change**: it increments
`transport_revision` once, and every follower resets as it does for a seek
([cv-semantics.md](cv-semantics.md)). None replays the notes it crosses.

| Change while playing | New anchor |
| --- | --- |
| `loop` false → true | The selected pattern. If it is the pattern playing, keep the elapsed beats within it; otherwise its row 0. |
| `loop` true → false | The first chain entry that plays the looped pattern, keeping the elapsed beats within it; if none, beat 0. |
| `pattern`, while `loop` is true | The new pattern, keeping the elapsed beats, wrapped into its length. |
| `chain`, while `loop` is false | The same entry index if it still plays the same pattern, keeping the elapsed beats; otherwise beat 0. |
| `rate`, `bar` or `bars` of the playing pattern | The same entry, keeping the elapsed quarter-note beats, wrapped into the new length (pattern-timing.md's rule). |
| `rate`, `bar` or `bars` of another pattern in the chain, while `loop` is false | The same entry, keeping the elapsed beats. The edit moves every later entry and the cycle's length `C`, and followers place the playhead at `(a + u) mod C`, so once the playhead has wrapped it would jump without a new segment. |

Every transport change has an **effective beat**, by default the moment it is
made; in this part every change is immediate. Queued switching later is the
same record with a later effective beat ([evaluator.md](evaluator.md)).

Passing from one chain entry to the next, editing cells, keys, scenes or a
lane's `len`, and editing a pattern the timeline doesn't play are ordinary
edits: the timeline is unchanged. Reapplying a value
that is already set is not a change. Undo and redo follow the same rules.
Queued pattern changes (switching at the end of the current pattern, as
Elektron machines do) are a later feature.

`POST /api/tracker_transport`'s `beat` ranges over the current cycle, `[0, C)`
or `[0, L[pattern])`, instead of 0–64 ([bridge-api.md](bridge-api.md)).

## Versions 1 and 2

Version 1 and 2 scores load, play and save unchanged under [score.md](score.md);
opening or saving never upgrades them. They map onto version 3 as:

- patterns 0–3 → `A01`–`A04`, each rate 1, bar 16, bars 4; patterns 4–127 null
- `chain` `[0, 1, 2, 3]`; `pattern` and `loop` unchanged
- audio lane `n` → `lanes[n]` with `len` 64, the same 64 cells and no
  transforms; lanes 5 and 6 empty, `len` 64
- `mutes` gains two `false`; `sounds` gains two `{"machine": null, "params": {}}`
  (version 1 gets six)
- `lanes` names `Lane 1`–`Lane 6`; `seed` 0; no cell conditions; keys, easing,
  scenes and other fields as they were

Under this mapping the chain plays exactly the version-1/2 song: entry `i` is
`[16i, 16i + 16)`. A looped pattern plays the same rows, but version 3 counts
its beats from the pattern's start, where versions 1 and 2 count from the
song's. So a looped pattern at beat `b` is at `b − 16 · pattern`.

The editor upgrades a score to version 3 explicitly, on the first edit that
needs it (a fifth or sixth lane, a pattern beyond `A04`, a rate, bar, bars,
lane length, chain, condition or transform edit), and undo restores the old
format. Upgrading while a looped pattern is playing is a transport change,
since its beat origin moves.

## Not in this part

Cell conditions, the event record, `evaluate()`, seed, bar counters and
fill-from-beat are [evaluator.md](evaluator.md) (A-spec-2); lane transforms are
[transforms.md](transforms.md) (A-spec-3); Rack's BUS IN, the wider BUS OUT hit
mask (with bit 7 reserved for "entry start") and the bus layer of FILL and
mutes are [bus-io.md](bus-io.md) (A-spec-4). The Rack display's beat is now
cycle-relative, which the Rack manual must say in Phase 3. The Rack sound
link's six lanes are C1. Triplet grids, ties, per-lane rates and cells placed
between rows are not planned for version 3; `ply`'s sub-steps are the only
events shorter than a row.
