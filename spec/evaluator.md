# The evaluator: events, conditions and bar counters

**Status: proposed contract, part 2 of 4.** This is A-spec-2 of the
[roadmap](../docs/ROADMAP.md) (§2.A, §4). It defines what a version-3 score
([score-v3.md](score-v3.md)) plays: the event record, `evaluate()` and its
invariants, cell conditions, the bar counter, the seed and FILL. Lane
transforms are A-spec-3 and bus I/O A-spec-4. No implementation accepts version
3 until all four parts have merged and HydraPy implements them. Written against
HydraPy `e5999b2282bd9766b6f7152b4278c3da4e72ed8e` and Gorgon `a293984`.

HydraPy's scheduler and `TrackerCV`, the browser audition synth and the Rack
engine are three implementations of this page. They must agree on every event,
at every beat; the vectors at the end are what HydraPy reproduces before the
generated fixture exists.

## Terms

From [score-v3.md](score-v3.md): timeline, cycle, entry, `R`, `rows`, `L`,
`row`, `lane_row`, `bar_in_pattern`. Lanes are numbered 0–5 in data and shown
as 1–6. Beats are exact rationals; with the rates of part 1 they are binary
fractions, exact in a double, but A-spec-3's `ply 3` makes thirds, so
implementations compare beats exactly.

- **Transport anchor:** where the current transport segment started: a cycle
  beat `a`, and its entry. Every transport change (a seek, Play, a timeline
  change) starts a new segment.
- **Elapsed beats `u`:** beats played since the anchor, `u ≥ 0`. They do not
  wrap and they survive tempo changes, which re-anchor the playhead without
  starting a segment. The playhead is at cycle beat `(a + u) mod C`.
- **Play:** one pass through one chain entry, or one repeat of the looped
  pattern. Plays are numbered from the anchor: play 0 contains `a`.
- **Visit:** a run of consecutive plays of the same pattern, starting no earlier
  than the anchor. In loop mode every repeat belongs to one visit. In chain mode
  `[0, 1, 1, 16]`, the two plays of A02 are one visit, and a chain whose last and
  first entries are the same pattern continues that visit across the wrap.

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
| `fill` | — | FILL is on at the occurrence's beat | `F` |
| `first` | — | `bar_count = 0` | `1ST` |

`"not": true` negates any of them and prefixes `!` to the code (`!F`, `!1:2`).
`not` is optional and defaults to false. Other keys are rejected.

A lane shorter than its bar visits a cell more than once per bar. Every
occurrence is judged separately, with that occurrence's `bar_count`, row and
beat, so `%p` draws afresh on each.

**PRE and NEI**, which depend on earlier results, are deferred to version 3.1
by the user's decision, so `evaluate()` takes no state.

### Chance

`chance` is a 32-bit hash of the score's `seed` and the occurrence. All
arithmetic is unsigned 32-bit, wrapping:

```
lowbias32(x):  x ^= x >> 16;  x *= 0x7feb352d;  x ^= x >> 15;  x *= 0x846ca68b;  x ^= x >> 16

chance(salt, pattern, lane, row, bar_count):
    h = lowbias32(seed ^ salt)
    for v in (pattern, lane, row, bar_count mod 2³²):  h = lowbias32(h ^ v)
    return h
```

`prob` uses salt `0x70726f62` ("prob"). `pattern` is the index 0–127, `lane`
0–5, and `row` the pattern row, not `lane_row`, so a short lane's repeats draw
independently (the roadmap's sketch used `lane_row`). A-spec-3's `degrade` uses
the same function with its own salt. In JavaScript, `x *= c` is
`x = Math.imul(x, c) >>> 0`.

Check values: `lowbias32(1) = 1753845952`, `lowbias32(0xffffffff) = 1734902346`,
and with seed `0x1234`, pattern 0, lane 1, row 12, bars 0–5, `chance` is
2199053738, 1337028887, 3820821168, 2469589051, 2251187919 and 1747857906.

### Seed

`seed` is a top-level integer, 0 to 2³² − 1, and is required in version 3. The
same seed and score give the same results on every implementation, on every
play. Changing it is an ordinary edit, not a transport change.

## FILL

FILL is transport state owned by Studio, not part of the score. It changes
from a beat, not from when a follower hears about it: Studio keeps the
transitions of the current segment as `[{at, on}…]`, with `at` in elapsed
beats, sorted, the first at 0 holding the state at the anchor. FILL is `on` at
`u` if the last transition with `at ≤ u` is on, so a transition exactly on an
occurrence's beat applies to it. A new segment starts with the state that was
in force when it began.

Rack applies a FILL change it makes itself at once, stamped with its own `u`,
and Studio adopts the stamp, so every implementation agrees on which bar the
change took effect in even when Studio hears it a poll later. A-spec-4 defines
how Rack sends it (BUS IN and the transport endpoint). This page defines only
the state and its effect.

## `evaluate()`

```
evaluate(score, transport, [u0, u1)) → candidates
transport = {beat: a, fill: [{at, on}…]}
```

It returns one **candidate** for every occurrence in `u0 ≤ at < u1` of a cell
that is present: not null, `vel > 0`, in a lane the score does not mute. An
occurrence before the anchor (`at < 0`) is never returned. Candidates are
sorted by `at`, then lane, then A-spec-3's sub-step.

| Field | Value |
| --- | --- |
| `id` | `"j.lane.row"` (A-spec-3 appends `.s` for a sub-step): play, lane and pattern row |
| `at` | elapsed beat of the occurrence, `play start + row / R[p]`, exact |
| `lane` | 0–5 |
| `duration` | `1 / R[p]` beats, one row; followers apply their gate fraction |
| `note`, `vel`, `locks` | the cell's, unresolved (sound-locks.md resolves locks) |
| `fires` | whether the condition holds; true without one |
| `origin` | `{play, entry, pattern, row, lane_row, bar, cond}`; `entry` is null when looping, `cond` the code or null |

Followers play the candidates that fire. The ones that don't are returned so a
trace can say why (below).

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
for gates, pitch and velocity only on occurrences that fire. `TrackerCV` and the
Rack engine therefore take the anchor, `u` and FILL as well as the beat.

## Transport changes

Every transport change has an **effective beat**: the elapsed beat, on the
segment it ends, at which the next segment starts. In this part every change is
immediate, so its effective beat is the moment it is made. A queued change
(Elektron's switch at the end of the current pattern) is later the same record
with a later effective beat, needing no format change. It takes the same
`{at, …}` shape as FILL, and as mutes in A-spec-4.

## Playhead fields

For a version-3 document, `GET /api/bridge` and the `/api/tracker_transport`
reply add these to the playhead ([bridge-api.md](bridge-api.md)). Older clients
ignore them, as the bridge API requires.

| Field | Meaning |
| --- | --- |
| `start` | The transport anchor: `{beat, entry}`, its cycle beat and chain entry, `entry` null when looping |
| `elapsed` | Elapsed beats `u` at `anchor_time` |
| `entry` | The chain entry at `anchor_beat`, null when looping |
| `fill` | The current segment's FILL transitions, `[{at, on}…]` |
| `pending` | A transport change not yet in effect, `{at, beat, entry}`, or absent. Always absent in this part |

A follower computes `u(now) = elapsed + (clock(now) − anchor_time) · bpm / 60`
while playing and enabled, as it computes the beat. The `entry` fields save
followers and traces from recomputing it from `S[]` against a chain that may
have just changed.

## Trace

Each implementation can write one line of JSON per candidate. HydraPy writes
JSONL; the Rack module keeps a bounded ring that its menu can dump in the same
format. The fields of the candidate are required, with `at` as an exact string
(`"7/4"`); `why` gives the deciding value (`"bar 2 mod 3 = 2"`,
`"chance 1337028887 mod 100 = 87"`, `"fill on"`). Fields an implementation adds
start with `x_`: for example `x_dispatched`, when it sent the event, and
`x_observed`, when a destination reported it (Machinemodule counts missed
blocks and the sound-link packet carries the producer frame). Lines are
ordered as the candidates are.

```json
{"id": "1.1.12", "at": "7", "lane": 1, "duration": "1/4", "note": "C-4", "vel": 100,
 "fires": false, "why": "chance 1337028887 mod 100 = 87",
 "origin": {"play": 1, "entry": null, "pattern": 0, "row": 12, "lane_row": 12, "bar": 1, "cond": "%50"}}
```

## Vectors

Hand-checked. Every pattern is rate 1 (four rows a beat) unless noted, at
bar 16, with lanes of the pattern's length except where stated; only the cells
listed are present, each `C-4` at velocity 100. `at` is elapsed beats; ✓ fires,
✗ does not.

**V1 — a 7-row lane against a 16-row bar.** Loop A01, bars 2 (32 rows,
`L = 8`), anchor beat 0. Lane 0 has `len` 7 and `1:2` on cell 0. The cell
occurs on rows 0, 7, 14, 21 and 28 of every play.

| `at` | id | row | bar_count | |
| --- | --- | --- | --- | --- |
| 0 | 0.0.0 | 0 | 0 | ✓ |
| 7/4 | 0.0.7 | 7 | 0 | ✓ |
| 7/2 | 0.0.14 | 14 | 0 | ✓ |
| 21/4 | 0.0.21 | 21 | 1 | ✗ |
| 7 | 0.0.28 | 28 | 1 | ✗ |
| 8 | 1.0.0 | 0 | 2 | ✓ |
| 39/4 | 1.0.7 | 7 | 2 | ✓ |
| 23/2 | 1.0.14 | 14 | 2 | ✓ |
| 53/4 | 1.0.21 | 21 | 3 | ✗ |
| 15 | 1.0.28 | 28 | 3 | ✗ |

**V2 — `1:3` and `%50`.** Loop A01, bars 1 (`L = 4`), seed `0x1234`, anchor
beat 0. Lane 1 has `1:3` on row 4 and `%50` on row 12. Each play is one bar.

| `at` | id | bar_count | condition | deciding value | |
| --- | --- | --- | --- | --- | --- |
| 1 | 0.1.4 | 0 | `1:3` | 0 mod 3 = 0 | ✓ |
| 3 | 0.1.12 | 0 | `%50` | 2199053738 mod 100 = 38 | ✓ |
| 5 | 1.1.4 | 1 | `1:3` | 1 | ✗ |
| 7 | 1.1.12 | 1 | `%50` | 1337028887 → 87 | ✗ |
| 9 | 2.1.4 | 2 | `1:3` | 2 | ✗ |
| 11 | 2.1.12 | 2 | `%50` | 3820821168 → 68 | ✗ |
| 13 | 3.1.4 | 3 | `1:3` | 0 | ✓ |
| 15 | 3.1.12 | 3 | `%50` | 2469589051 → 51 | ✗ |
| 17 | 4.1.4 | 4 | `1:3` | 1 | ✗ |
| 19 | 4.1.12 | 4 | `%50` | 2251187919 → 19 | ✓ |
| 21 | 5.1.4 | 5 | `1:3` | 2 | ✗ |
| 23 | 5.1.12 | 5 | `%50` | 1747857906 → 6 | ✓ |

The roadmap's `1:3` plus `%` plus `ply 4` combination needs `ply`, so its
vector belongs to A-spec-3.

**V3 — a seek mid-bar.** Loop A01, bars 2 (`L = 8`), anchor at beat 3 (row 12).
Lane 2 has `1ST` on rows 0 and 14; lane 3 has `!1ST` on row 20. Row 0 of play 0
is before the anchor and is not returned. Play 1 starts at `at` 5, at bar 2.

| `at` | id | row | bar_count | condition | |
| --- | --- | --- | --- | --- | --- |
| 1/2 | 0.2.14 | 14 | 0 | `1ST` | ✓ |
| 2 | 0.3.20 | 20 | 1 | `!1ST` | ✓ |
| 5 | 1.2.0 | 0 | 2 | `1ST` | ✗ |
| 17/2 | 1.2.14 | 14 | 2 | `1ST` | ✗ |
| 10 | 1.3.20 | 20 | 3 | `!1ST` | ✓ |
| 13 | 2.2.0 | 0 | 4 | `1ST` | ✗ |

**V4 — FILL arriving mid-bar.** Loop A01, bars 1 (`L = 4`), anchor beat 0, FILL
transitions `[{at 0, off}, {at 5/2, on}, {at 6, off}]`. Lane 4 has `F` and lane
5 `!F`, each on rows 8 and 12. The transition at 6 lands on an occurrence and
applies to it.

| `at` | row | `F` (lane 4) | `!F` (lane 5) |
| --- | --- | --- | --- |
| 2 | 8 | ✗ | ✓ |
| 3 | 12 | ✓ | ✗ |
| 6 | 8 | ✗ | ✓ |
| 7 | 12 | ✗ | ✓ |

**V5 — two plays, one visit.** Chain `[0, 1, 1, 16]` from score-v3.md's
examples (A02 is rate 2, bars 2; the cycle is 40 beats), anchor beat 0. A02 has
`1:4` on lane 0 row 0 and `2:4` on lane 1 row 16 (its second bar). The second
play of A02 continues the visit, so the conditions fire once per pass through
the chain. If each play were its own visit, both would fire on every play.

| `at` | id | entry | row | bar_count | condition | |
| --- | --- | --- | --- | --- | --- | --- |
| 16 | 1.0.0 | 1 | 0 | 0 | `1:4` | ✓ |
| 18 | 1.1.16 | 1 | 16 | 1 | `2:4` | ✓ |
| 20 | 2.0.0 | 2 | 0 | 2 | `1:4` | ✗ |
| 22 | 2.1.16 | 2 | 16 | 3 | `2:4` | ✗ |
| 56 | 5.0.0 | 1 | 0 | 0 | `1:4` | ✓ |
| 58 | 5.1.16 | 1 | 16 | 1 | `2:4` | ✓ |
| 60 | 6.0.0 | 2 | 0 | 2 | `1:4` | ✗ |
| 62 | 6.1.16 | 2 | 16 | 3 | `2:4` | ✗ |

Evaluated over `[0, 16)` in V1, `[0, 24)` in V2 and `[0, 80)` in V5, each set
is the same when the interval is split anywhere, including on an occurrence's
own beat (invariant 2).

## Not in this part

Lane transforms, `ply` and `degrade` are A-spec-3. BUS IN, FILL and mutes from
Rack, the transport endpoint's new fields, the wider BUS OUT hit mask and BUS
OUT bit 7 reserved for "entry start" are A-spec-4. The Rack sound link's six
lanes are C1, and the trace in Rack is G2's implementation.
