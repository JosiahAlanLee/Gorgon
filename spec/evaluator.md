# The evaluator: events, conditions and bar counters

**Status: contract, part 2 of 4; not yet implemented.** This is A-spec-2 of the
[roadmap](../docs/ROADMAP.md) (§2.A, §4). It defines what a version-3 score
([score-v3.md](score-v3.md)) plays: the event record, `evaluate()` and its
invariants, cell conditions, the bar counter, the seed and FILL. Lane
transforms are [transforms.md](transforms.md) (A-spec-3) and bus I/O
[bus-io.md](bus-io.md) (A-spec-4).
No implementation accepts version 3 until all four parts have merged and HydraPy
implements them. Written against HydraPy
`e5999b2282bd9766b6f7152b4278c3da4e72ed8e` and Gorgon `a293984`.

Its six proposals were ratified on 2026-10-26 in the review of #42, with
amendments carried in A-spec-3: time in integer ticks; mutes returned as
candidates that don't fire; the anchor's row returned for the landing gate;
late FILL transitions; a fifth value in `chance`; the trace's segment fields;
and the version-3 fixture.

HydraPy's scheduler and `TrackerCV`, the browser audition synth and the Rack
engine are three implementations of this page. They must agree on every event,
at every tick; the vectors at the end are what HydraPy reproduces before the
generated fixture exists.

## Terms

From [score-v3.md](score-v3.md): timeline, cycle, entry, `R`, `rows`, `L`,
`row`, `lane_row`, `bar_in_pattern`. Lanes are numbered 0–5 in data and shown
as 1–6.

- **Transport anchor:** where the current transport segment started: a cycle
  tick `a`, and its entry. Every transport change (a seek, Play, a timeline
  change) starts a new segment.
- **Elapsed ticks `u`:** ticks played since the anchor. They do not wrap and
  they survive tempo changes, which re-anchor the playhead without starting a
  segment. The playhead is at cycle tick `(a + u) mod C`.
- **Play:** one pass through one chain entry, or one repeat of the looped
  pattern. Plays are numbered from the anchor: play 0 contains `a`.
- **Visit:** a run of consecutive plays of the same pattern index, beginning
  with the anchor's play at the earliest. In loop mode every repeat belongs to one visit. In chain mode
  `[0, 1, 1, 16]`, the two plays of A02 are one visit, and a chain whose last and
  first entries are the same pattern continues that visit across the wrap.

## Ticks

Every musical time on this page is an integer count of **ticks, 1920 per
quarter note**: `at`, `u`, `duration`, the anchor, FILL's `at`, effective beats
and interval bounds. A row is `W[p] = 1920 / R[p]` ticks (1920 at rate ¼ down to
60 at rate 8), and `S[]`, `L[]` and `C` are counted in ticks the same way.
Rates and [`ply`](transforms.md#ticks) can put 1 to 128 events in a quarter note,
and 1920 is a multiple of 384, the least common multiple of every count they
can make, so every event starts and ends on a tick. Exactness is integer
comparison. A 64-bit integer does not wrap in any session, and a JavaScript
number holds ticks exactly for over 40,000 years at 200 BPM.

- **Anchors** are integer cycle ticks, `0 ≤ a < C`. Studio rounds a seek's beat
  to the nearest tick, `⌊beat · 1920 + ½⌋ mod C`, and anchors Play at the tick
  nearest the playhead. A Play that lands on the anchor's own tick (after a
  pause right after a seek, or after whole cycles) continues the segment
  instead: a new one would have the same `transport_revision` and `start`, the
  key that tells segments apart, while `elapsed` restarted.
- **Elapsed ticks at the playhead** are an integer. Studio re-anchors for a tempo
  change on a tick: `elapsed` is a whole tick and `anchor_time` the clock time
  at which the playhead reached it.
- **Followers** compute a real `u(now)` from the clock (below) and use it for
  signal phases. `evaluate()` takes integer bounds: a follower that has
  evaluated up to tick `t` asks for `[t, ⌊u(now)⌋ + 1)`. Rounding the clock
  changes when a candidate is sent, never which candidates exist or their `at`.
- **Live changes** (FILL from Studio's key or from Rack) are stamped with the
  first tick their maker has not yet evaluated, so its own outputs and the
  record agree.

The bridge's `beat` and `anchor_beat` stay floating-point quarter notes, so
version-1/2 clients are unaffected.

## Bar counter

For play `j` of pattern `p`, let `v` be the number of earlier plays in its
visit. At row `row`:

```
bar_count = v · bars[p] + bar_in_pattern      bar_in_pattern = ⌊row / bar[p]⌋
```

It is a pure function of the anchor and `u`. Lanes still restart with every
play (score-v3.md); only the counter spans the visit. So writing a 2-bar
pattern twice into the chain lets `1:4` span both plays. A seek never changes
which bar a row is in, but it starts a new visit, so the counter restarts from
the anchor's play: a seek to row 40 of a 16-row-bar pattern is at bar 2, and
the next play of that visit starts at bar `bars[p]`.

## Conditions

A cell may have `cond`, a typed object. It fires on an occurrence when its
condition holds; a cell without `cond` always fires.

| `kind` | Fields | Holds when | Code |
| --- | --- | --- | --- |
| `every` | `a`, `b`, integers, 1 ≤ a ≤ b ≤ 8 | `bar_count mod b = a − 1` | `a:b` |
| `prob` | `p`, integer 1–99 | `chance mod 100 < p` | `%p` |
| `fill` | — | FILL is on at the occurrence's tick | `F` |
| `first` | — | `bar_count = 0` | `1ST` |

`"not": true` negates any of them and prefixes `!` to the code (`!F`, `!1:2`).
`not` is optional and defaults to false. Other keys are rejected.

A lane shorter than its bar visits a cell more than once per bar. Every
occurrence is judged separately, with that occurrence's `bar_count`, row and
tick, so `%p` draws afresh on each. A condition is judged once per row: the
sub-steps that [`ply`](transforms.md) makes share it.

**PRE and NEI**, which depend on earlier results, are deferred to version 3.1
by the user's decision, so `evaluate()` takes no state.

### Chance

`chance` is a 32-bit hash of the score's `seed` and the occurrence. All
arithmetic is unsigned 32-bit, wrapping:

```
lowbias32(x):  x ^= x >> 16;  x *= 0x7feb352d;  x ^= x >> 15;  x *= 0x846ca68b;  x ^= x >> 16

chance(salt, pattern, lane, row, bar_count, sub):
    h = lowbias32(seed ^ salt)
    for v in (pattern, lane, row, bar_count mod 2³², sub):  h = lowbias32(h ^ v)
    return h
```

`prob` uses salt `0x70726f62` ("prob") and `sub` 0. `pattern` is the index
0–127, `lane` 0–5, and `row` the pattern row, not `lane_row`, so a short lane's
repeats draw independently (the roadmap's sketch used `lane_row`). `sub` is the
`ply` sub-step, which only `degrade` uses; it draws from the same function with
its own salt ([transforms.md](transforms.md#evaluation)). In JavaScript,
`x *= c` is `x = Math.imul(x, c) >>> 0`.

`chance mod 100` is very slightly uneven, and that is intended: 2³² mod 100 =
96, so residues 0–95 each occur once more in 2³² than 96–99, and `%p` holds with
a probability at most 9 × 10⁻¹⁰ above `p / 100`. Do not replace the modulo with
rejection sampling or a multiply-shift; either changes every draw.

Check values: `lowbias32(1) = 1753845952`, `lowbias32(0xffffffff) = 1734902346`,
and with seed `0x1234`, salt "prob", pattern 0, lane 1, row 12, `sub` 0 and
bars 0–5, `chance` is 1936593078, 1284988505, 1732901356, 3891962147, 2080199420
and 1655901407.

### Seed

`seed` is a top-level integer, 0 to 2³² − 1, and is required in version 3. The
same seed and score give the same results on every implementation, on every
play. Changing it is an ordinary edit, not a transport change.

## FILL

FILL is transport state owned by Studio, not part of the score. It changes
from a tick, not from when a follower hears about it: Studio keeps the
transitions of the current segment as `[{at, on}…]`, with `at` in elapsed
ticks, sorted, the first at 0 holding the state at the anchor, so the list is
never empty; of two at the same `at`, the later one counts. A list is `on` at
`u` if the last transition with `at ≤ u` is on,
so a transition exactly on an occurrence's tick applies to it; before the
anchor (`u < 0`, the anchor's row below) it has the anchor's state. A new
segment starts with the state that was in force when it began.

Rack's FILL comes from its BUS IN and lives in a second list with the same
rules, in the **bus layer** with the bus mutes ([bus-io.md](bus-io.md)). FILL
is in force at `u` when Studio's list or the bus layer's is on. Rack applies a
change it makes at once, stamped with its own `u`, and Studio adopts the stamp,
so every implementation agrees on which bar the change took effect in even when
Studio hears it a poll later.

**Late transitions.** Because Rack stamps its own changes, Studio can receive
transitions earlier than ticks it has already evaluated. It merges them into
its lists in order of `at` (a bus-layer window replaces everything from its
`from` on, [bus-io.md](bus-io.md#bus-in)), replays nothing it has already sent,
and writes a trace line for each change to its record (below). The candidates Studio
evaluated between the stamp and the receipt were judged under the old state.
That window, about one poll, is the accepted cost; from then on every
implementation holds the same record.

## `evaluate()`

```
evaluate(score, transport, [u0, u1)) → candidates
transport = {tick: a, fill: [{at, on}…], bus: {fill: [{at, on}…], mutes: [[{at, on}…] ×6]}}
```

The anchor's entry is the one containing cycle tick `a`
([score-v3.md](score-v3.md#the-timeline)); the playhead's `start.entry` reports
it.

It returns one **candidate** for every occurrence of a present cell (not null,
`vel > 0`) with `u0 ≤ at < u1` that ends after the anchor, `at + duration > 0`.
That is every occurrence from the anchor on, and those of the **anchor's row**,
which start before it with a negative `at`; earlier rows are never returned.
When the anchor is on a row boundary the anchor's row starts at 0. The bounds
are integers and may be negative, so a follower that hears of a segment as it
starts begins its first query at or before the anchor row's start,
`−((a − S) mod W)` for the anchor's entry start `S`. One that joins a segment
late, connecting in the middle of it, begins at its own current tick. Candidates are sorted by `at`, then lane.

A follower's landing gate ([cv-semantics.md](cv-semantics.md#landing-gate))
decides whether to play an anchor-row candidate: the gate fraction is the
follower's setting, so the evaluator returns the occurrence and leaves the
decision to it.

| Field | Value |
| --- | --- |
| `id` | `"j.lane.row"`, play, lane and pattern row, with `.s` appended for a [`ply`](transforms.md#candidates) sub-step |
| `at` | elapsed tick of the occurrence, `play start + row · W[p]` plus the sub-step's offset |
| `lane` | 0–5 |
| `duration` | `W[p]` ticks, one row, divided by the `ply` count; followers apply their gate fraction |
| `note`, `vel`, `locks` | the cell's, unresolved ([sound-locks.md](sound-locks.md) resolves locks); `locks` is null when the cell has none |
| `fires` | whether it plays (below) |
| `why` | the deciding value (below), or null |
| `origin` | `{play, entry, pattern, row, lane_row, cell, sub, of, bar, cond, via}`; `bar` is `bar_count`, `entry` null when looping, `cond` the code or null; `cell`, `sub`, `of` and `via` are [transforms.md](transforms.md#candidates)'s |

A candidate **fires** when its lane is not muted at its `at`, by the score or
by the bus layer, its condition holds and no transform dropped it. `why` names
the first of these that fails, in that order; for a candidate that fires it is
the condition's deciding value, or null without a condition:

| Reason | `why` |
| --- | --- |
| muted lane | `"muted (score)"`, `"muted (bus)"`; the score first when both hold |
| `every` | `"bar 2 mod 3 = 2"` |
| `prob` | `"chance 1284988505 mod 100 = 5"` |
| `fill` | `"fill on"`, `"fill off"` |
| `first` | `"bar 0"`, `"bar 3"` |
| `degrade` | `"degrade /1: chance 2303062892 mod 100 = 92"`, naming the node by its JSON Pointer as in `via` |

Followers play the candidates that fire. The others are returned so a trace can
say why: a muted lane's cells are candidates too, which tells "muted" from "no
cell". The bus layer's mutes have FILL's `{at, on}` shape ([bus-io.md](bus-io.md)).

Invariants every implementation keeps:

1. **Pure:** the same score, transport and interval give the same candidates.
   No wall clock, no state, no randomness except `chance`.
2. **Partitions:** evaluating `[u0, u1)` and then `[u1, u2)` returns exactly the
   candidates of `[u0, u2)`, with no duplicate at the shared boundary and none
   missing.
3. **Stable identity:** a candidate's `id` and fields depend only on the score,
   the transport and the occurrence, never on the interval that found it.
4. **Bars by position:** `bar_in_pattern` depends only on the row.

`cv-semantics.md` applies to version 3 with one change: a cell is **present**
for gates, pitch and velocity only on candidates that fire. Held pitch and
velocity don't update on a candidate that doesn't fire, so a muted lane behaves
as it does in versions 1 and 2. `TrackerCV` and the Rack engine therefore take
the anchor, `u` and FILL as well as the beat.

## Transport changes

Every transport change has an **effective beat**: the elapsed tick, on the
segment it ends, at which the next segment starts. In this part every change is
immediate, so its effective beat is the moment it is made. A queued change
(Elektron's switch at the end of the current pattern) is later the same record
with a later effective beat, needing no format change. It takes the same
`{at, …}` shape as FILL and the bus layer.

## Playhead fields

For a version-3 document, `GET /api/bridge` and the `/api/tracker_transport`
reply add these to the playhead ([bridge-api.md](bridge-api.md)). Older clients
ignore them, as the bridge API requires. Times are integer ticks.

| Field | Meaning |
| --- | --- |
| `start` | The transport anchor: `{tick, entry}`, its cycle tick and chain entry, `entry` null when looping |
| `elapsed` | Elapsed ticks `u` at `anchor_time`, an integer |
| `entry` | The chain entry at `anchor_beat`, null when looping |
| `fill` | The current segment's FILL transitions, `[{at, on}…]` |
| `bus` | The bus layer, `{fill, mutes}`, in the same shape ([bus-io.md](bus-io.md#the-bus-layer)) |
| `pending` | A transport change not yet in effect, `{at, tick, entry}`, or absent. Always absent in this part |

A follower computes `u(now) = elapsed + (clock(now) − anchor_time) · bpm / 60 · 1920`
while playing and enabled, as it computes the beat. The `entry` fields save
followers and traces from recomputing it from `S[]` against a chain that may
have just changed.

## Trace

Each implementation can write one line of JSON per candidate. HydraPy writes
JSONL; the Rack module keeps a bounded ring that its menu can dump in the same
format. A candidate line has the candidate's fields, all required, and the
segment's `transport_revision` and `start`. `id` is relative to the anchor, so
`transport_revision` separates segments; Play starts a segment without changing
it, and `start` separates those. Fields an implementation adds start with `x_`:
for example `x_dispatched`, when it sent the event, and `x_observed`, when a
destination reported it (Machinemodule counts missed blocks and the sound-link
packet carries the producer frame). Lines are ordered as the candidates are.

```json
{"transport_revision": 3, "start": {"tick": 0, "entry": null},
 "id": "1.1.12", "at": 13440, "lane": 1, "duration": 480, "note": "C-4", "vel": 100, "locks": null,
 "fires": true, "why": "chance 1284988505 mod 100 = 5",
 "origin": {"play": 1, "entry": null, "pattern": 0, "row": 12, "lane_row": 12, "cell": 12,
            "sub": 0, "of": 1, "bar": 1, "cond": "%50", "via": []}}
```

A change an implementation makes to its record of FILL or the bus layer is a
line of its own, with `received`, the first tick it had not yet evaluated when
the change arrived. `received > at` is a late insert, and `[at, received)` is
the interval that was judged under the old state. A bus-layer change names
`fill` or a lane's `mute`; a window Studio drops or refuses, and the end of a
lease, are lines too:

```json
{"transport_revision": 3, "start": {"tick": 0, "entry": null},
 "fill": {"at": 4800, "on": true}, "received": 4867}
{"transport_revision": 3, "start": {"tick": 0, "entry": null},
 "bus": {"mute": {"lane": 2, "at": 1921, "on": true}}, "received": 1990}
{"transport_revision": 3, "start": {"tick": 0, "entry": null},
 "bus": {"fill": {"at": 7680, "on": false}}, "received": 7702}
{"transport_revision": 3, "start": {"tick": 0, "entry": null},
 "bus": {"window": "dropped", "reason": "stale segment", "from": 0}, "received": 7710}
{"transport_revision": 3, "start": {"tick": 0, "entry": null},
 "bus": {"lease": "ended", "sender": "4f1c9a7e2b6d0835"}, "received": 19230}
```

## Fixture

The page is what the fixture checks, so version 3 gets its own:
`rack/tests/fixture3.txt`, header `GORGON_FIXTURE_3`, generated by
`rack/tools/make_fixture.py` from the pinned HydraPy. The version-2 fixture
stays as it is for the legacy comparison: versions 1 and 2 keep
[score.md](score.md)'s rules. A version-3 fixture holds, for random scores:

- **the score**, as its JSON document on one line, so the Rack engine's
  version-3 reader is under test too, and the `seed` with it;
- **segments**, each with its anchor `{tick, entry}`, `transport_revision` and
  FILL transitions;
- **candidates** of each segment over a stated `[u0, u1)`, as trace lines
  without `x_` fields, which the engine's `evaluate()` must reproduce exactly;
- **signals**: the playheads a follower is sent while Studio's tracker plays
  the score (it joins the first segment late; there are seeks, two of them
  landing inside a hit that fires and heard after its gate window, FILL, bus
  windows, a pause and resume, and a tempo change), and what `TrackerCV`
  renders from them at 48 kHz in 10 ms blocks: every sample where clock,
  reset, run, a gate, a velocity or a pitch changes, and the automation lanes
  every 50 ms. A follower that replays the playheads, computing `u` as
  `TrackerCV` does, must match on every sample.

Replaying a stream, rather than sampling chosen beats as version 2's fixture
does, also checks the state that fixture leaves to unit tests: reset pulses,
retriggers, landing gates and held values, across segments.

## Vectors

Hand-checked. Every pattern is rate 1 (`W` = 480 ticks) unless noted, at
bar 16, with lanes of the pattern's length except where stated; only the cells
listed are present, each `C-4` at velocity 100. Unless stated, the seed is 0,
FILL is off throughout (`[{at 0, off}]`) and no lane is muted. Lanes are
numbered as in data, 0–5. `at` is elapsed ticks (1920 a beat); ✓ fires, ✗ does
not.

**V1 — a 7-row lane against a 16-row bar.** Loop A01, bars 2 (32 rows,
`L` = 15360), anchor tick 0. Lane 0 has `len` 7 and `1:2` on cell 0. The cell
occurs on rows 0, 7, 14, 21 and 28 of every play.

| `at` | id | row | bar_count | |
| --- | --- | --- | --- | --- |
| 0 | 0.0.0 | 0 | 0 | ✓ |
| 3360 | 0.0.7 | 7 | 0 | ✓ |
| 6720 | 0.0.14 | 14 | 0 | ✓ |
| 10080 | 0.0.21 | 21 | 1 | ✗ |
| 13440 | 0.0.28 | 28 | 1 | ✗ |
| 15360 | 1.0.0 | 0 | 2 | ✓ |
| 18720 | 1.0.7 | 7 | 2 | ✓ |
| 22080 | 1.0.14 | 14 | 2 | ✓ |
| 25440 | 1.0.21 | 21 | 3 | ✗ |
| 28800 | 1.0.28 | 28 | 3 | ✗ |

**V2 — `1:3` and `%50`.** Loop A01, bars 1 (`L` = 7680), seed `0x1234`, anchor
tick 0. Lane 1 has `1:3` on row 4 and `%50` on row 12. Each play is one bar.

| `at` | id | bar_count | condition | `why` | |
| --- | --- | --- | --- | --- | --- |
| 1920 | 0.1.4 | 0 | `1:3` | `bar 0 mod 3 = 0` | ✓ |
| 5760 | 0.1.12 | 0 | `%50` | `chance 1936593078 mod 100 = 78` | ✗ |
| 9600 | 1.1.4 | 1 | `1:3` | `bar 1 mod 3 = 1` | ✗ |
| 13440 | 1.1.12 | 1 | `%50` | `chance 1284988505 mod 100 = 5` | ✓ |
| 17280 | 2.1.4 | 2 | `1:3` | `bar 2 mod 3 = 2` | ✗ |
| 21120 | 2.1.12 | 2 | `%50` | `chance 1732901356 mod 100 = 56` | ✗ |
| 24960 | 3.1.4 | 3 | `1:3` | `bar 3 mod 3 = 0` | ✓ |
| 28800 | 3.1.12 | 3 | `%50` | `chance 3891962147 mod 100 = 47` | ✓ |
| 32640 | 4.1.4 | 4 | `1:3` | `bar 4 mod 3 = 1` | ✗ |
| 36480 | 4.1.12 | 4 | `%50` | `chance 2080199420 mod 100 = 20` | ✓ |
| 40320 | 5.1.4 | 5 | `1:3` | `bar 5 mod 3 = 2` | ✗ |
| 44160 | 5.1.12 | 5 | `%50` | `chance 1655901407 mod 100 = 7` | ✓ |

With `ply 4` on the lane this is [transforms.md](transforms.md#vectors)'s V7.

**V3 — a seek mid-bar.** Loop A01, bars 2 (`L` = 15360), anchor tick 5760
(beat 3, row 12, on a row boundary). Lane 2 has `1ST` on rows 0 and 14; lane 3
has `!1ST` on row 20. Row 0 of play 0 is before the anchor's row and is not
returned. Play 1 starts at `at` 9600, at bar 2.

| `at` | id | row | bar_count | condition | |
| --- | --- | --- | --- | --- | --- |
| 960 | 0.2.14 | 14 | 0 | `1ST` | ✓ |
| 3840 | 0.3.20 | 20 | 1 | `!1ST` | ✓ |
| 9600 | 1.2.0 | 0 | 2 | `1ST` | ✗ |
| 16320 | 1.2.14 | 14 | 2 | `1ST` | ✗ |
| 19200 | 1.3.20 | 20 | 3 | `!1ST` | ✓ |
| 24960 | 2.2.0 | 0 | 4 | `1ST` | ✗ |

**V4 — FILL arriving mid-bar.** Loop A01, bars 1 (`L` = 7680), anchor tick 0,
FILL transitions `[{at 0, off}, {at 4800, on}, {at 11520, off}]`. Lane 4 has
`F` and lane 5 `!F`, each on rows 8 and 12. The transition at 11520 lands on an
occurrence and applies to it.

| `at` | row | `F` (lane 4) | `!F` (lane 5) |
| --- | --- | --- | --- |
| 3840 | 8 | ✗ | ✓ |
| 5760 | 12 | ✓ | ✗ |
| 11520 | 8 | ✗ | ✓ |
| 13440 | 12 | ✗ | ✓ |

**V5 — two plays, one visit.** Chain `[0, 1, 1, 16]` from score-v3.md's
examples (A02 is rate 2, bars 2, `W` = 240; the cycle is 76800 ticks), anchor
tick 0. A02 has `1:4` on lane 0 row 0 and `2:4` on lane 1 row 16 (its second
bar). The second play of A02 continues the visit, so the conditions fire once
per pass through the chain. If each play were its own visit, both would fire on
every play.

| `at` | id | entry | row | bar_count | condition | |
| --- | --- | --- | --- | --- | --- | --- |
| 30720 | 1.0.0 | 1 | 0 | 0 | `1:4` | ✓ |
| 34560 | 1.1.16 | 1 | 16 | 1 | `2:4` | ✓ |
| 38400 | 2.0.0 | 2 | 0 | 2 | `1:4` | ✗ |
| 42240 | 2.1.16 | 2 | 16 | 3 | `2:4` | ✗ |
| 107520 | 5.0.0 | 1 | 0 | 0 | `1:4` | ✓ |
| 111360 | 5.1.16 | 1 | 16 | 1 | `2:4` | ✓ |
| 115200 | 6.0.0 | 2 | 0 | 2 | `1:4` | ✗ |
| 119040 | 6.1.16 | 2 | 16 | 3 | `2:4` | ✗ |

**V6 — the anchor's row and a muted lane.** Loop A01, bars 1, anchor tick 2040:
row 4 (which starts at tick 1920) at row phase ¼. Lane 0 has cells on rows 3, 4
and 5; lane 1 a cell on row 4 with `ply 4`; lane 2 is muted, with a cell on row
4 and `1ST` on row 8. Play 0 starts at `at` −2040 and play 1 at 5640; over
`[−1920, 5640)`, play 0 gives:

| `at` | id | `duration` | | `why` |
| --- | --- | --- | --- | --- |
| −120 | 0.0.4 | 480 | ✓ | |
| −120 | 0.2.4 | 480 | ✗ | `muted (score)` |
| 0 | 0.1.4.1 | 120 | ✓ | |
| 120 | 0.1.4.2 | 120 | ✓ | |
| 240 | 0.1.4.3 | 120 | ✓ | |
| 360 | 0.0.5 | 480 | ✓ | |
| 1800 | 0.2.8 | 480 | ✗ | `muted (score)`, though its `1ST` holds |

Not returned: lane 0's row 3, which ended at −120, and lane 1's sub-step 0, at
−120 with `duration` 120, which ends exactly at the anchor. With the default
gate ½, a follower's landing gate plays `0.0.4` from the anchor: its landing
phase is 120 / 480 = ¼ < ½, so the gate lasts (½ − ¼) · 480 = 120 ticks less
`retrigger_ms`. Lane 1's sub-steps play normally, each for 60 ticks. With the
anchor at tick 2280 (phase ¾) instead, `0.0.4` is still returned, at −360, and
the landing gate adds nothing.

Evaluated over `[0, 16 · 1920)` in V1, `[0, 24 · 1920)` in V2, `[−1920, 16 · 1920)`
in V3, `[0, 8 · 1920)` in V4, `[0, 80 · 1920)` in V5 and `[−1920, 5640)` in V6,
each set is the same when the interval is split anywhere, including on an
occurrence's own tick (invariant 2).

## Not in this part

Lane transforms, `ply` and `degrade` are [transforms.md](transforms.md)
(A-spec-3). BUS IN, the bus layer, the transport endpoint's new fields and the
wider BUS OUT hit mask, with bit 7 reserved for "entry start", are
[bus-io.md](bus-io.md) (A-spec-4). The Rack sound link's six lanes are C1, and
the trace in Rack is G2's implementation.
