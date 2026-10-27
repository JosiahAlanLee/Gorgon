# Score format 3: bus I/O

**Status: contract, part 4 of 4; implemented** in the Rack module
`GorgonBridge` 2.4.0 and HydraPy 0.1.0 at `3c01f32` (the
[`hydrapy.pin`](../hydrapy.pin)). This is A-spec-4 of the
[roadmap](../docs/ROADMAP.md) (§2.A, §4). It defines the **bus layer**: FILL and
six lane mutes that Rack sets live from a new BUS IN, stamped by Rack and merged
into what every implementation plays. It also defines the transport endpoint
fields that carry the layer, and BUS OUT's hit mask for version 3. All four
parts have merged: HydraPy's Studio accepts windows and holds the lease, and the
Rack module reads BUS IN and sends them. Written against HydraPy
`e5999b2282bd9766b6f7152b4278c3da4e72ed8e` and Gorgon `6f30050` (A-spec-3,
[transforms.md](transforms.md)).

The user decided the bus's role on 2026-10-26 (roadmap §5.4). Mutes stay on the
bus, and BUS IN and BUS OUT share one word layout. A bus input is high for on,
does nothing while disconnected, and merges with the score by OR. It works in
both bus modes. The bit map belongs to the Rack module. Stamps, Rack's record,
windows, the lease and folding were this page's proposals, ratified when it merged.

## One word layout

| Bit | BUS OUT (hit-mask source) | BUS IN |
| --- | --- | --- |
| 0–5 | lane `n` has a hit that fires, on bit `n − 1` | mute lane `n`, on bit `n − 1` |
| 6 | FILL in force | FILL |
| 7 | 0, reserved for "entry start" | reserved, ignored |

With one layout, a word that leaves on BUS OUT and comes back through a Lookup,
a FUN or a Register means the same lanes on the way in, and the FILL state
Gorgon publishes on bit 6 is the bit it reads. "Mute lane 3 after lane 1 hits"
is one FUN away (V12). The old default, FILL on bit 0 and lanes on bits 1–6,
would have moved every lane by one bit on the way back.

The contract is that Rack can set FILL and six lane mutes, and what those mean.
Which bit carries which on BUS IN is the Rack module's: its manual documents the
default above, and its context menu remaps it, as *CV n drives* does for CV 1–4.
So a later use for bit 7 changes the manual, not this page. BUS OUT's layout is
[bytebus-output.md](bytebus-output.md#version-3)'s.

## The bus layer

Beside Studio's own FILL ([evaluator.md](evaluator.md#fill)), the transport has
a second layer:

```
bus = {fill: [{at, on}…], mutes: [[{at, on}…] ×6]}
```

Each list follows FILL's rules: transitions in elapsed ticks of the current
segment, sorted, never empty, the first at 0 holding the state at the anchor,
the later arrival winning a tie, and the state carried into the next segment.
Rack writes the layer; Studio holds it and publishes it to every follower.

**Merge by OR.** At tick `u`:

```
FILL in force      = Studio's FILL(u)  or  bus.fill(u)
lane n is muted    = score mutes[n]    or  bus.mutes[n](u)
```

The bus can turn FILL and mutes on, never off. The editor's mutes are saved
state and the bus is a live overlay. High means on, so an all-zero word does
nothing: that is what a fresh Register or Counter emits and what a missing
cable must mean. A patch that thinks in "active lanes" inverts one hop upstream;
Lookup's Curve shape 32 is an exact inversion.

`evaluate()` reads a mute at each candidate's `at`, and FILL at its row's start,
as before. A mute that arrives while a hit sounds doesn't cut its gate; it
applies from the next candidate. A candidate on a muted lane has `why` `"muted (score)"` or
`"muted (bus)"`, the score first when both hold
([evaluator.md](evaluator.md#evaluate)).

## BUS IN

BUS IN is a new Rack input, ID 6, appended after CV 4; existing IDs are
unchanged. It is a byte-bus input
([BYTE_BUS_SPEC.md](https://github.com/JosiahAlanLee/DSP_Emulation/blob/main/docs/BYTE_BUS_SPEC.md))
read in either mode:

- **Unclocked** (8 channels or fewer): every engine sample is a sample instant.
- **Clocked** (9 or more, CLK on channel 9): the word changes on CLK's rising
  edges.

The record is the same either way; clocking decides when changes can happen.
- A Register latched on Gorgon's own CLOCK changes mutes on rows.
- A Counter's CARRY at modulus 16 changes them on bars.
- Counter → Lookup → BUS IN is per-bar mute automation from a table, as in
  Elektron's song-mode mutes.

**Stamps.** On each engine sample Rack reads BUS IN before it evaluates the
sample. A bit that changed is stamped with the first tick Rack has not yet
evaluated, [evaluator.md](evaluator.md#ticks)'s rule for live changes, and
never below 0; a stamp of 0 sets the state at the anchor. The change applies to
Rack's own outputs from that tick at once. So a change read on the sample where
a candidate starts applies to it; one read a sample later does not (V11). While
stopped, Rack stands at the playhead's paused tick and stamps changes there.
Changing the bit map in the menu is read as a change of every bit it moves.

**Connection.** A disconnected BUS IN has no effect, and Rack sends nothing
while no cable is patched. Connecting is read as a change of each high bit.
Disconnecting, removing the module or closing the patch clears every bit, sent
as an all-clear.

**Rack's record.** Rack keeps a record of the layer for the current segment:
the bus layer's lists, built from its own stamps. When it adopts a new segment,
the record begins at 0 with its bus state at that moment. Between two
consecutive ticks at which `evaluate()` reads a bit, the record keeps only that
bit's last change, and none if the bit ends the interval where it began. Those
read ticks are row starts for FILL, and every `W / 12` ticks from a row start
for a mute: every tick where a `ply` sub-step can start. Nothing reads a bit
between read ticks, so the coalesced record gives exactly the candidates the
raw changes would. An audio-rate signal on BUS IN costs at most one transition
per bit per read tick.

**Windows.** Rack sends its record to Studio as a **window**: every list from a
tick `from` onwards, each beginning with a transition at `from` holding Rack's
state there. Studio replaces its own transitions at or after `from` with the
window's. A window restates rather than adds, so it can be repeated, lost or
reordered behind a newer one without leaving the two records apart:

- `from` is never later than the oldest change of Rack's that no reply has yet
  acknowledged, or than Rack's first unevaluated tick, and never below 0.
- Rack sends a change with its next request, not when the next read tick
  passes. If another change follows in the same read interval, a later window
  replaces it.
- Rack sends at most one request per bridge poll, and at least one a second
  while BUS IN is connected, so any difference heals within a second.
- Studio drops a window whose segment is not current, and traces it. Rack's
  first window in the new segment starts at 0.

**Lease and sender.** A transport has one bus layer, held by one module. Each
Rack module instance makes a random `sender` id, and the layer belongs to the
sender whose window Studio accepted last. If 3 seconds pass without a request
from it, the lease ends: Studio clears the layer, with an all-clear at its own
first unevaluated tick, and another sender may take it. While the lease is
held, Studio refuses windows from any other sender. So a crashed Rack can't
leave lanes muted that the editor has no way to unmute, and two patched BUS INs
can't take turns overwriting each other.

**Who plays which record.** The module holding the layer plays its own record,
at once. Every other follower, including another Rack module, plays the
snapshot's `bus`, a poll or so later.

## Transport endpoint

`POST /api/tracker_transport` ([bridge-api.md](bridge-api.md)) takes three more
fields for a version-3 document:

```json
{"segment": {"transport_revision": 7, "start": {"tick": 5760, "entry": null}},
 "sender": "4f1c9a7e2b6d0835",
 "bus": {"from": 4800,
         "fill": [{"at": 4800, "on": true}],
         "mutes": [[{"at": 4800, "on": false}], [{"at": 4800, "on": false}],
                   [{"at": 4800, "on": false}, {"at": 4801, "on": true}],
                   [{"at": 4800, "on": false}], [{"at": 4800, "on": false}],
                   [{"at": 4800, "on": false}]]}}
```

- `segment` names the segment the window belongs to, by the trace's key: its
  `transport_revision` and `start`.
- `sender` is the module's id, 16 hexadecimal digits.
- `bus` is a window, `{from, fill, mutes}`: seven lists, each starting with a
  transition at `from`.
- All three come together, and never with `beat`, `enabled` or `playing`; such
  a request answers `400`. A request with them is not a seek and does not change
  `transport_revision`.
- Studio applies the window, or drops it if `segment` is not current, or
  answers `400 {"error": "bus held by another module"}`. The reply is otherwise
  the usual snapshot, whose playhead `bus` shows what Studio holds. A reply
  acknowledges every change in the window it answers.

The playhead gains `bus` in the layer's shape: lists starting at 0
([evaluator.md](evaluator.md#playhead-fields)).

**Folding.** A segment can run for hours with bar-rate mute automation. Studio
may fold every list at a tick `F` at least 10 seconds before its first
unevaluated tick: the list becomes one transition at 0 holding its state at `F`,
followed by its transitions after `F`. A follower that joins a segment late
starts evaluating at its own tick ([evaluator.md](evaluator.md#evaluate)), so
nothing anyone plays
changes, and a window starting before `F` still replaces everything from its
`from`.

## BUS OUT

For a version-3 document, BUS OUT's hit-mask source publishes lanes 1–6 on
bits 0–5 and FILL on bit 6, with bit 7 reserved
([bytebus-output.md](bytebus-output.md#version-3)). Version 1 and 2 documents
publish as before.

## Feedback

A patch from BUS OUT through logic back to BUS IN works, and it is causal. The
word comes back at least two samples after the row it describes, one per cable,
and that row's candidates were evaluated when it started. So feedback acts from
the next read: "mute lane 3 whenever lane 1 hits" mutes lane 3 on the row after
each hit of lane 1 (V12). Reacting within the same row needs a condition on
another lane's result, which is NEI, deferred to version 3.1.

## Vectors

As in [evaluator.md](evaluator.md#vectors): loop A01, rate 1 (`W` = 480), bar
16, bars 1, cells `C-4` at velocity 100, seed 0, anchor tick 0, lanes numbered
0–5 as in data, `at` in elapsed ticks, ✓ fires, ✗ does not. Lists not given are
`[{at 0, off}]`.

**V10 — the two layers.** Lane 1 is muted in the score. Studio's FILL is
`[{0, off}, {5760, on}]`. The bus layer has FILL `[{0, off}, {1920, on},
{3840, off}]`, lane 0's mute `[{0, off}, {2880, on}, {4800, off}]` and lane 1's
mute `[{0, on}]`. Lane 0 has cells on rows 4, 8 and 12; lane 1 on row 4; lane 2
has `F` on rows 0, 4, 8 and 12.

| `at` | id | | `why` |
| --- | --- | --- | --- |
| 0 | 0.2.0 | ✗ | `fill off` |
| 1920 | 0.0.4 | ✓ | |
| 1920 | 0.1.4 | ✗ | `muted (score)`, though the bus mutes it too |
| 1920 | 0.2.4 | ✓ | `fill on`, from the bus |
| 3840 | 0.0.8 | ✗ | `muted (bus)` |
| 3840 | 0.2.8 | ✗ | `fill off`: the bus transition at 3840 applies |
| 5760 | 0.0.12 | ✓ | |
| 5760 | 0.2.12 | ✓ | `fill on`, from Studio |

**V11 — stamps.** At 120 BPM and 48 kHz a tick is 12.5 samples, so with the
anchor on sample 0, Rack's `u` on sample `s` is `s / 12.5`. Before evaluating
sample `s`, Rack has evaluated through tick `⌊u(s − 1)⌋`. Lane 2 has a cell on
row 1, at tick 480, and BUS IN bit 2 rises on sample `s`:

| `s` | `u(s)` | Stamp | Row 1's lane-2 hit |
| --- | --- | --- | --- |
| 5988 | 479.04 | 479 | muted |
| 5999 | 479.92 | 480 | muted |
| 6000 | 480 | 480 | muted |
| 6001 | 480.08 | 481 | plays |

**V12 — feedback.** As V11, with Gorgon's CLOCK at 4 PPQN, one edge per row.
BUS OUT's hit-mask source feeds a FUN whose output is bit 0 of its input on bit
2 and 0 on every other bit, which feeds BUS IN. Lane 0 has cells on rows 0, 4, 8 and 12; lane 2 on every row. Row `r`
is published on sample `6000 r` and read two samples later, on a sample where
Rack's `u` is `480 r + 0.16`, so it is stamped `480 r + 1`. Lane 2's bus mute
becomes `[{0, off}, {1, on}, {481, off}, {1921, on}, {2401, off}, {3841, on},
{4321, off}, {5761, on}, {6241, off}]`. Lane 2's hits on rows 1, 5, 9 and 13
(`at` 480, 2400, 4320, 6240) are `muted (bus)`, and its other twelve play,
including those on rows 0, 4, 8 and 12, where lane 0 hits.

Over `[0, 7680)` in V10 and V12, each set is the same when the interval is split
anywhere (invariant 2).

## Not in this part

- **Per-cell external gating**, a condition `X n` that holds when BUS IN bit `n`
  is high, is more powerful than a lane mute. It is the right later feature,
  as a version-3.1 condition kind.
- **A plain FILL gate jack**, as the roadmap sketched, is not added. One gate
  into a Merge on channel 7 (bit 6) and on to BUS IN does the same.
- **BUS IN's panel position** is the Rack module's: above BUS OUT on its 14 HP
  panel, with GATE 5 and 6 ([the manual](../rack/docs/MANUAL.md#bus-in)).
