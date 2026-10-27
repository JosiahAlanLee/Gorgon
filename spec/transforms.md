# Score format 3: lane transforms

**Status: proposed contract, part 3 of 4.** This is A-spec-3 of the
[roadmap](../docs/ROADMAP.md) (§2.A, §4). It defines a lane's `transforms`:
an ordered list of typed nodes that change which cell a lane plays on a row,
how many times, and whether it sounds. They run inside `evaluate()`
([evaluator.md](evaluator.md)); bus I/O is A-spec-4. No implementation accepts
version 3 until all four parts have merged and HydraPy implements them. Written
against HydraPy `e5999b2282bd9766b6f7152b4278c3da4e72ed8e` and Gorgon `6995cf4`.

The five node types are an original subset with familiar names. No Strudel
code is imported ([DRUM_MACHINE.md](../docs/DRUM_MACHINE.md)'s position).
Generators such as Euclidean fills or a mini-notation string are editor tools
that write cells; they are not score fields.

## Shape

Each lane of a pattern ([score-v3.md](score-v3.md)) has `transforms`, a list of
at most eight nodes applied in order. It is required in version 3; `[]` means
none.

| `kind` | Fields | Effect | Shown as |
| --- | --- | --- | --- |
| `rot` | `k`, integer −127 to 127 | The cell written at lane row `i` plays at lane row `(i + k) mod len` | `rot k` |
| `rev` | — | The cell written at lane row `i` plays at lane row `len − 1 − i` | `rev` |
| `ply` | `n`, integer 2–4 | Each event becomes `n` events, evenly spaced within its row: a ratchet | `ply n` |
| `degrade` | `p`, integer 1–99 | Each event is kept with probability `p` percent, in the same sense as `%p` | `degrade p` |
| `every` | `a`, `b`, integers, 1 ≤ a ≤ b ≤ 8; `inner`, a node | Applies `inner` on bars where `bar_count mod b = a − 1`, the `a:b` condition's test | `every a:b (inner)` |

```json
"transforms": [{"kind": "rot", "k": 1},
               {"kind": "every", "a": 2, "b": 2, "inner": {"kind": "rev"}}]
```

Other keys are rejected. `every` holds one node and nests to depth 3: an
`every` may hold an `every` that holds a leaf. Across a lane's list, including
inside every `every`, the product of all the `ply` counts is at most 4 (one
`ply 2`, `3` or `4`, or two `ply 2`), so every event lasts a whole number of
ticks (below). The schema checks the shapes and depth; `validate()` checks the
product and `a ≤ b`.

## Evaluation

Transforms are judged per occurrence, as conditions are. For lane `n` on
pattern row `row` of a play, with that row's `bar_count`
([evaluator.md](evaluator.md#bar-counter)):

1. **Active nodes.** Walk the list in order. A leaf is active. An `every` whose
   test holds for `bar_count` contributes its `inner`, walked the same way; one
   whose test fails contributes nothing. The result is a flat list of active
   leaves.
2. **Which cell.** `rot` and `rev` act on the lane's `len` cells as a sequence,
   in list order. The cell that plays at `lane_row` is found by undoing them
   from the last to the first, starting from `c = lane_row`: for `rot k`,
   `c ← (c − k) mod len`; for `rev`, `c ← len − 1 − c`. `mod` is the
   non-negative remainder: in C++ and JavaScript, `((x % len) + len) % len`. If cell `c` is not
   present (null or `vel` 0), the lane has no events on this row.
3. **Events.** A present cell gives one event covering the row: sub-step 0 of
   1. Then `ply` and `degrade` act in list order:
   - `ply n` replaces each event, sub-step `s` of `m`, with `n` events, sub-steps
     `s · n + q` of `m · n` for `q = 0 … n − 1`. Each copy carries its parent's
     note, velocity, locks and condition, and, if the parent was dropped, the
     draw that dropped it, which becomes the copy's `why`.
   - `degrade p` drops each event not already dropped unless
     `chance(0x64656772 + d, pattern, lane, row, bar_count, s) mod 100 < p`,
     where `s` is the event's sub-step at that point and `d` the node's number.
     Nodes are numbered 0, 1, 2 … in the order they appear in the lane's list,
     counting each `every` and then its `inner`, so two `degrade` nodes draw
     independently: in `[rot 1, every 1:1 (degrade 50)]` the `degrade` is node 2,
     at JSON Pointer `/1/inner`. `0x64656772` is "degr"; `chance` is
     [evaluator.md](evaluator.md#chance)'s.
4. **Candidates.** Sub-step `s` of `m` starts at `row start + s · W / m` and
   lasts `W / m`, where `W` is the row's length in ticks.

`rot` and `rev` move whole rows. They never reorder the sub-steps in a row, so
`rev` does not reverse a ratchet. Since they only choose which cell plays, and
every draw belongs to the occurrence (the row and bar where it is heard, not
the cell it came from), their position relative to `ply` and `degrade` does not
matter. The order of `ply` and `degrade` does: `degrade` before `ply` draws once
per row, so a ratchet plays whole or not at all; after `ply` it draws once per
sub-step (V8).

**Conditions are judged once per row.** A cell's condition is evaluated at the
row's start with sub-step 0, as for a cell without transforms, and every
sub-step `ply` makes from it shares the result. `%50` on a plied cell plays the
whole ratchet or none of it, and `F` takes the FILL state at the row's start,
so a FILL change in the middle of a plied row applies from the next row (V7).

**Bars.** `every` reads the bar of the row being played. On a lane whose `len`
does not divide `bar`, a transform can switch on in the middle of the lane's
cycle; it applies to every occurrence within a qualifying bar, as a condition
does (V9).

**Mutes.** A muted lane's events are still returned, with `fires: false` and
`why: "muted"` ([evaluator.md](evaluator.md#evaluate)).

## Candidates

Transforms add to the candidate record:

| Field | Value |
| --- | --- |
| `id` | `"j.lane.row.s"` for sub-step `s` of a plied row (`m > 1`); `"j.lane.row"` otherwise |
| `at`, `duration` | as in step 4 |
| `origin.cell` | the lane row of the cell that plays: `lane_row`, unless `rot` or `rev` moved it |
| `origin.sub`, `origin.of` | `s` and `m`; 0 and 1 for a whole row |
| `origin.via` | the active leaves in order, as JSON Pointers into the lane's `transforms`: `["/0", "/1/inner"]` |
| `why` | for a dropped event, `"degrade /1: chance 2303062892 mod 100 = 92"`: the node's JSON Pointer, as in `via`, and its draw |

A candidate fires when its lane is not muted, its condition holds and no
`degrade` dropped it; `why` names the first of these that fails.

## Ticks

A row is `W = 1920 / R` ticks ([evaluator.md](evaluator.md#ticks)). With the
`ply` product at most 4, the shortest event at each rate is:

| `rate` | ¼ | ½ | 1 | 2 | 4 | 8 |
| --- | --- | --- | --- | --- | --- | --- |
| `W` | 1920 | 960 | 480 | 240 | 120 | 60 |
| `W / 3` | 640 | 320 | 160 | 80 | 40 | 20 |
| `W / 4` | 480 | 240 | 120 | 60 | 30 | 15 |

Two `ply 4` would make `60 / 16 = 3.75` at rate 8, which is why the product is
capped rather than each `ply`.

## Vectors

As in [evaluator.md](evaluator.md#vectors): rate 1 (`W = 480`), bar 16, cells
`C-4` at velocity 100 unless noted, seed 0, FILL off and no mutes unless
stated, lanes numbered 0–5, `at` in elapsed ticks, anchor tick 0, ✓ fires,
✗ does not.

**V7 — `1:3`, `%50` and `ply 4`.** V2's score (loop A01, bars 1, seed `0x1234`;
lane 1 has `1:3` on row 4 and `%50` on row 12) with `[{"kind": "ply", "n": 4}]`
on lane 1. Each row gives four candidates, `duration` 120, at the row's start
plus 0, 120, 240 and 360, with ids `j.1.row.0` to `j.1.row.3`. The condition is
judged once per row, so the four share V2's result.

| Play | Row | Sub-steps at | Condition | `why` of all four | All four |
| --- | --- | --- | --- | --- | --- |
| 0 | 4 | 1920–2280 | `1:3` | `bar 0 mod 3 = 0` | ✓ |
| 0 | 12 | 5760–6120 | `%50` | `chance 1936593078 mod 100 = 78` | ✗ |
| 1 | 4 | 9600–9960 | `1:3` | `bar 1 mod 3 = 1` | ✗ |
| 1 | 12 | 13440–13800 | `%50` | `chance 1284988505 mod 100 = 5` | ✓ |
| 2 | 4 | 17280–17640 | `1:3` | `bar 2 mod 3 = 2` | ✗ |
| 2 | 12 | 21120–21480 | `%50` | `chance 1732901356 mod 100 = 56` | ✗ |

**V8 — `degrade` before and after `ply`.** Loop A01, bars 1, seed `0x1234`.
Lanes 2 and 3 each have cells on rows 0 and 8. Lane 2 is `[ply 4, degrade 50]`
(the `degrade` is node 1, salt `0x64656773`) and draws per sub-step; lane 3 is
`[degrade 50, ply 4]` (node 0, salt `0x64656772`) and draws once per row. The
draws are `chance(salt, 0, lane, row, bar, s)`, with `s` = 0 for lane 3. Sub-steps
are at the row's start plus 0, 120, 240 and 360; rows 0 and 8 of play 0 start
at 0 and 3840, of play 1 at 7680 and 11520.

| Play | Row | Lane 2 draws, sub-steps 0–3 (mod 100) | Lane 2 | Lane 3 draw (mod 100) | Lane 3 |
| --- | --- | --- | --- | --- | --- |
| 0 | 0 | 2286100646 (46), 385726237 (37), 370582005 (5), 246513302 (2) | ✓✓✓✓ | 3038833604 (4) | ✓✓✓✓ |
| 0 | 8 | 2303062892 (92), 565417095 (95), 146505197 (97), 3613849973 (73) | ✗✗✗✗ | 3396531612 (12) | ✓✓✓✓ |
| 1 | 0 | 922554248 (48), 517400578 (78), 1910746187 (87), 2456862593 (93) | ✓✗✗✗ | 4023513783 (83) | ✗✗✗✗ |
| 1 | 8 | 2572734288 (88), 1657527929 (29), 3140429349 (49), 2465367129 (29) | ✗✓✓✓ | 1919793396 (96) | ✗✗✗✗ |

Every candidate's `origin.via` is `["/0", "/1"]`, and a dropped one's `why`
names its draw: `"degrade /1: chance 2303062892 mod 100 = 92"` for `0.2.8.0`.
Lane 3's copies share their row's draw, so all four of `1.3.0.0`–`1.3.0.3` have
`"degrade /0: chance 4023513783 mod 100 = 83"`.

**V9 — `rot`, `rev` and `every` on a short lane.** Loop A01, bars 2 (rows
0–31). Lane 0 has `len` 6, cells 0–2 `C-4`, `D-4` and `E-4`, cell 5 `G-4`, and
`[rot 1, every 2:2 (rev)]`. In bar 0 only `rot 1` is active and the lane plays
`[G, C, D, E, –, –]`: lane row 0 takes cell `(0 − 1) mod 6 = 5`. In bar 1 `rev`
follows it and the lane plays `[–, –, E, D, C, G]`. The lane keeps its own
6-row cycle, so `rev` switches on at row 16, in the middle of the cycle that
began at row 12.

| Rows | `at` | bar_count | `via` | `lane_row` → `cell` | Notes |
| --- | --- | --- | --- | --- | --- |
| 0, 1, 2, 3 | 0, 480, 960, 1440 | 0 | `/0` | 0, 1, 2, 3 → 5, 0, 1, 2 | G C D E |
| 6, 7, 8, 9 | 2880, 3360, 3840, 4320 | 0 | `/0` | 0, 1, 2, 3 → 5, 0, 1, 2 | G C D E |
| 12, 13, 14, 15 | 5760, 6240, 6720, 7200 | 0 | `/0` | 0, 1, 2, 3 → 5, 0, 1, 2 | G C D E |
| 16, 17 | 7680, 8160 | 1 | `/0`, `/1/inner` | 4, 5 → 0, 5 | C G |
| 20, 21, 22, 23 | 9600, 10080, 10560, 11040 | 1 | `/0`, `/1/inner` | 2, 3, 4, 5 → 2, 1, 0, 5 | E D C G |
| 26, 27, 28, 29 | 12480, 12960, 13440, 13920 | 1 | `/0`, `/1/inner` | 2, 3, 4, 5 → 2, 1, 0, 5 | E D C G |

All fire. Over `[0, 12 · 1920)` in V7, `[0, 8 · 1920)` in V8 and `[0, 8 · 1920)`
in V9, the set is the same when the interval is split anywhere (invariant 2).

## Not in this part

Euclidean and mini-notation generators are editor tools. Per-lane rates,
transforms that move events between rows by less than a row (swing, nudge),
transforms that read other lanes, and PRE/NEI are not in version 3. The roadmap's
tree shape, `origin.via` and the lane-indexed record are what leave room for
hierarchical composition and stream routing later without a format change.
