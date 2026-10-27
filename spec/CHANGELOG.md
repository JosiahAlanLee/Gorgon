# Contract changelog

Newest first. Each entry names the HydraPy commit and Rack module version it
describes.

## 2026-10-27 — the sound link's pitch is C4 = 0 V

HydraPy `3c01f32eb0c5a396d4e854e20d0b0d9b14de27d7` (pin unchanged) · Rack module
`GorgonBridge` 2.4.0 (unreleased).

A sound-link lane's `pitch` is now defined as 1 V/oct with C4 = 0 V, whatever the
Rack module's *Pitch zero* menu says ([sound-locks.md](sound-locks.md#rack-sound-link)).
Before this, it followed that menu, so a receiver couldn't tell C3 from C4. The
Monomachine (roadmap C2b) is pitched and needs the note. Machinemodule ignores
pitch, so nothing changes for it, and the packet layout and version stay the same.
The module's own V/OCT outputs still follow the menu.

## 2026-10-27 — the Rack module's event trace

HydraPy `3c01f32eb0c5a396d4e854e20d0b0d9b14de27d7` (pin unchanged) · Rack module
`GorgonBridge` 2.4.0.

The Rack module keeps [evaluator.md](evaluator.md#trace)'s bounded ring of the
candidates it evaluates and of BUS IN's changes. Its menu saves the ring as
JSONL. It adds one field, `x_frame`, the engine frame the line was written on.
`evaluator_test` checks every fixture3 candidate's line against HydraPy's own,
as JSON.

## 2026-10-27 — the Rack module's BUS IN

HydraPy `3c01f32eb0c5a396d4e854e20d0b0d9b14de27d7` (pin unchanged) · Rack module
`GorgonBridge` 2.4.0.

The Rack module implements [bus-io.md](bus-io.md)'s BUS IN, input 6. It reads
the word before evaluating each sample, unclocked or clocked. It stamps each
change with the first tick it hasn't evaluated and plays its own layer at once.
Rack's record keeps one change per read tick, and windows carry it to Studio:
one per bridge poll while there are changes, and at least one a second while
BUS IN is patched, which keeps the lease. A refusal, `bus held by another
module`, makes the module follow Studio's bus until the lease lapses.
Unpatching BUS IN, removing the module or closing the patch sends an all-clear.
The context menu remaps the bit map, which is saved with the patch.
`engine_test` reproduces vectors V11 and V12 exactly, and the end-to-end test
has a real Studio accept the windows and give the probe the lease.

## 2026-10-27 — score format 3 in the Rack module; sound link version 2

HydraPy `3c01f32eb0c5a396d4e854e20d0b0d9b14de27d7` (pin moved from `a479932`; HydraPy #67 and #68) · Rack module
`GorgonBridge` 2.4.0.

- **The Rack module plays version-3 scores.** `rack/src/engine/EngineV3.hpp`
  plays `evaluate()`'s candidates by [cv-semantics.md](cv-semantics.md)'s rules,
  one sample at a time, on a follower of elapsed ticks. Six lanes reach GATES,
  VEL and V/OCT (four channels for a version-1 or -2 score, as before) and new
  GATE 5 and 6 jacks, outputs 18 and 19. BUS IN, input 6, is on the panel and not
  read yet. The panel is 14 HP. Versions 1 and 2 play exactly as before.
- **fixture3 gains its signals** ([evaluator.md](evaluator.md#fixture)). It was
  to hold signal rows at chosen ticks. It holds, per score, the playheads a
  follower is sent while Studio plays it, and every sample where `TrackerCV`'s
  discrete signals change, with the automation lanes every 50 ms. The engine
  replays the playheads and matches on every sample, so reset pulses,
  retriggers, landing gates and held values are checked across segments too.
  `fixture.txt` is byte-identical at the new pin.
- **BUS OUT's version-3 word**, as [bytebus-output.md](bytebus-output.md#version-3)
  specifies. The menu item is now *BUS OUT source*, beside BUS IN.
- **Sound link version 2** ([sound-locks.md](sound-locks.md#rack-sound-link),
  [sound-link.hpp](sound-link.hpp)): six lanes, and a `Header` by which a
  receiver's buffers declare the version they read. Version 1's layout stays as
  `gorgon_sound::v1`; the module writes whichever the receiver declares.
  Machinemodule 2.2.0 reads version 2.

## 2026-10-27 — score format 3: the Rack module's evaluator and fixture3

HydraPy `a4799320efd7f6dae924f216a14708cbd8851abf` (pin moved from `e5999b2`; HydraPy #63–#65) · Rack module
`GorgonBridge` 2.3.1, unchanged in behaviour.

- `rack/src/engine/ScoreV3.hpp` is the C++ `evaluate()` ([evaluator.md](evaluator.md),
  [transforms.md](transforms.md), [bus-io.md](bus-io.md)). It allocates nothing,
  so the audio thread can call it; `BridgeJson.hpp` reads a version-3 document
  into it on the bridge thread. The module doesn't play version 3 yet: its
  engine, six lanes and BUS IN come next.
- `rack/tests/fixture3.txt`, header `GORGON_FIXTURE_3`, holds HydraPy's
  candidates for random version-3 scores; `evaluator_test` reproduces every
  field of every one. CI regenerates it from the pin like the version-2 fixture,
  which this pin leaves unchanged.

## 2026-10-27 — score format 3: Play on the anchor's own tick

HydraPy `e5999b2282bd9766b6f7152b4278c3da4e72ed8e` (pin unchanged; implemented in
HydraPy #67) · Rack module `GorgonBridge` 2.3.1, unchanged.

A Play that lands on the segment anchor's own tick continues that segment
([evaluator.md](evaluator.md#ticks)). Play doesn't change `transport_revision`, so a
new segment there would share the old one's key, `transport_revision` and `start`,
while `elapsed` restarted: followers would keep their place in the old one.

## 2026-10-27 — score format 3: follow-ups from the HydraPy implementation

HydraPy `e5999b2282bd9766b6f7152b4278c3da4e72ed8e` (pin unchanged; the
implementation is HydraPy #63, #64 and #65) · Rack module `GorgonBridge` 2.3.1,
unchanged. Three gaps the implementation found, and the status table:

- **The empty pattern's easing** is `smooth` on all three lanes, as the editor's
  patterns use. score-v3.md said "the default keys and easing" without naming one.
- **A length change elsewhere in the chain is a transport change** that keeps
  the playing entry and the beats into it. Changing another chained pattern's
  `rate`, `bar` or `bars` moves the later entries and `C`. Followers place the
  playhead at `(a + u) mod C`, so after a wrap the old segment would jump.
- **`fill` on `POST /api/tracker_transport`** is Studio's FILL key for the
  editor. It is stamped at the first tick none of Studio's outputs has
  evaluated, and it is not a seek.
- transforms.md and bus-io.md are marked ratified: the user merged them.

## 2026-10-26 — score format 3, part 4: bus I/O (proposal)

HydraPy `e5999b2282bd9766b6f7152b4278c3da4e72ed8e` · Rack module `GorgonBridge` 2.3.1,
both unchanged. A-spec-4 of the [roadmap](../docs/ROADMAP.md), on top of A-spec-3.

[bus-io.md](bus-io.md) proposes the **bus layer**: FILL and six lane mutes that Rack
sets live from a new BUS IN, input ID 6. The user decided the bus's role (roadmap
§5.4):

- **One word layout:** BUS IN and BUS OUT share it. Lane `n` is on bit `n − 1`
  (hit going out, mute coming in), FILL is on bit 6, and bit 7 is reserved, so
  OUT → logic → IN patches are coherent.
- **Polarity:** high means mute or fill, and an all-zero word does nothing.
- **Disconnected** means no effect.
- **Merge by OR:** the bus can mute but not unmute, and can fill but not unfill.
  `why` says `"muted (score)"` or `"muted (bus)"`.
- **Both bus modes,** with the same transition record.
- **The bit map is the Rack module's,** remappable in its menu.

The page proposes how the layer moves:

- **Stamps:** the first tick Rack has not yet evaluated, read before the sample's
  evaluation, never below 0.
- **Rack's record:** at most one change per bit between two ticks that read it.
- **Windows:** each request restates Rack's record from a tick `from` onwards, and
  Studio replaces its lists from there. A lost, repeated or stale request
  therefore heals within a second. `from` covers every change no reply has yet
  acknowledged.
- **Lease and sender:** one module holds the layer by a random `sender` id. Three
  seconds of silence ends the lease and clears the layer, so a crashed Rack can't
  leave lanes muted.
- **Mutes are judged at each candidate's `at`,** so a mute doesn't cut a sounding
  gate.
- **Folding:** transitions more than 10 seconds old can be folded.
- **Endpoint:** `POST /api/tracker_transport` gains `segment`, `sender` and `bus`,
  never mixed with a seek; the playhead gains `bus`.

[bytebus-output.md](bytebus-output.md) gains the version-3 hit mask: lanes 1–6 on
bits 0–5, FILL on bit 6, and bit 7 reserved for "entry start". Version-1/2 output is
unchanged. [evaluator.md](evaluator.md) takes the bus layer as input and reads
FILL as Studio's or the bus layer's. [cv-semantics.md](cv-semantics.md),
[bridge-api.md](bridge-api.md) and [score-v3.md](score-v3.md) point to the new
page. Vectors V10–V12 cover the two layers, stamping at a sample boundary, and
OUT → FUN → IN feedback, which acts on the next row because it is causal. The
schema, the version-1/2 rules and the fixture are unchanged.

## 2026-10-26 — score format 3, part 3: lane transforms (proposal), and the #42 amendments

HydraPy `e5999b2282bd9766b6f7152b4278c3da4e72ed8e` · Rack module `GorgonBridge` 2.3.1,
both unchanged. A-spec-3 of the [roadmap](../docs/ROADMAP.md).

[transforms.md](transforms.md) proposes lane transforms. Each lane gains a required
`transforms` list, `[]` for none, of at most eight nodes, applied in order:

- **Nodes:** `rot k`, `rev`, `ply n` (a ratchet within the row), `degrade p` and
  `every a:b (inner)`, nesting to depth 3.
- **Ply limit:** the product of a lane's `ply` counts is at most 4.
- **Evaluation:** transforms are judged per occurrence with that row's bar, as
  conditions are. A condition is judged once per row, and `ply`'s sub-steps share
  the result.
- **Degrade:** draws from `chance` with its own salt per node, once per row before
  `ply` and once per sub-step after it.
- **Vectors:** V7–V9, including the roadmap's `1:3` plus `%` plus `ply 4`.

[evaluator.md](evaluator.md) is ratified, with the review of #42's amendments:

- **Ticks:** every musical time is an integer count of ticks, 1920 per quarter
  note. That covers `at`, `u`, `duration`, anchors, FILL's `at`, effective beats
  and the playhead's `start`, `elapsed` and `pending`. The bridge's `beat` stays
  a float.
- **Mutes:** a muted lane's cells are returned as candidates with `fires: false`
  and `why: "muted"`. Candidates gain `why`, with fixed strings.
- **The anchor's row:** `evaluate()` returns occurrences that end after the anchor
  (`at + duration > 0`), so the row containing it comes back with a negative
  `at`. The follower's landing gate decides whether to play it. This keeps
  cv-semantics.md's landing-gate cases working under version 3 (V6).
- **Late FILL transitions** are inserted in order, with nothing replayed, and
  traced.
- **`chance`** folds a fifth value, the `ply` sub-step (0 for a cell's own
  condition). The check values and V2's draws change; nothing implements them
  yet. The mod-100 bias is documented as intended.
- **Trace lines** carry `transport_revision` and `start`, and FILL transitions get
  lines of their own.
- **The version-3 fixture** is defined: `GORGON_FIXTURE_3` in
  `rack/tests/fixture3.txt`, with the version-2 fixture kept.

[score-v3.md](score-v3.md) adds `transforms` to the lane, the empty-pattern defaults
and the version-1/2 mapping. [cv-semantics.md](cv-semantics.md) says that a
version-3 gate window is the candidate's span and how the landing gate finds its
candidate, and [bridge-api.md](bridge-api.md) that the version-3 playhead fields
are ticks. The schema's version-3 lane requires `transforms`. The version-1/2
rules, and so the fixture, are unchanged.

## 2026-10-26 — score format 3, part 2: the evaluator (proposal)

HydraPy `e5999b2282bd9766b6f7152b4278c3da4e72ed8e` · Rack module `GorgonBridge` 2.3.1,
both unchanged. A-spec-2 of the [roadmap](../docs/ROADMAP.md).

[evaluator.md](evaluator.md) defines what a version-3 score plays.

- **Candidates:** `evaluate(score, transport, [u0, u1))` returns one candidate per
  occurrence of a present cell, with a stable id, an exact beat, `fires` and its
  origin. It is pure, and splitting the interval returns the same set.
- **Conditions:** cells gain `cond`, one of `a:b`, `%p`, `F` or `1ST`, each with an
  optional `not`. The roadmap's other two, PRE and NEI, are deferred to 3.1 by the
  user's decision.
- **Bar counter:** counts bars across a *visit*, consecutive plays of the same
  pattern, per the #41 review.
- **Chance:** `%p` draws from a specified 32-bit hash of the new top-level `seed`.
- **FILL:** takes effect from a beat, as the user ratified.
- **Effective beat:** every transport change has one, so queued switching later
  needs no format change.
- **Playhead:** gains `start`, `elapsed`, `entry`, `fill` and `pending` for
  version-3 documents.
- **Trace and vectors:** a JSONL trace format, and five hand-checked vectors.

[score-v3.md](score-v3.md)'s five proposals are now ratified, with the review's
amendments. It notes that lanes restarting with their pattern is the master-length
case, and that the empty-pattern object an editor writes must equal the defaults
exactly. [cv-semantics.md](cv-semantics.md) and [bridge-api.md](bridge-api.md)
point to the new page. The schema's version-3 branch gains `seed` and `cond`; the
version-1/2 rules are unchanged.

When all four parts are written, each implementation must:

- **HydraPy:** implement `evaluate()` and its trace, with property tests of the
  invariants. Feed `TrackerCV`, the scheduler and the browser synth from it.
  Publish the new playhead fields. Its schema check must also cover the
  materialised empty pattern.
- **Rack module:** do the same in the engine, keep the trace ring, and regenerate
  the fixture from HydraPy (Phase 3).

## 2026-10-26 — score format 3, part 1: lanes and timing (proposal)

HydraPy `e5999b2282bd9766b6f7152b4278c3da4e72ed8e` · Rack module `GorgonBridge` 2.3.1,
both unchanged. A-spec-1 of the [roadmap](../docs/ROADMAP.md).

[score-v3.md](score-v3.md) proposes `hydrapy-tracker-3`, and
[score.schema.json](score.schema.json) gains its version-3 branch; the version-1/2
rules are unchanged. The user ratified six lanes (the Monomachine's six tracks)
and 128 patterns in eight banks of sixteen. The format adds:

- six lanes, each with a name, a sound and a mute
- 128 patterns, `null` meaning empty, and a `chain` that sets the song order
  when `loop` is false
- per pattern: `rate` (¼×–8×), `bar` and `bars`, up to 128 rows
- per lane: `len`, so lanes repeat within their pattern (polymeter)
- one timeline, with the beat/row conversion every implementation uses
- the transport changes that re-anchor it
- the mapping from versions 1 and 2, which reproduces their song exactly

It supersedes [pattern-timing.md](pattern-timing.md). Conditions and the evaluator
(A-spec-2), transforms (A-spec-3) and bus I/O (A-spec-4) extend the format next.

When all four are written, each implementation must:

- **HydraPy:** validate version 3. Use the shared timing helpers everywhere a row or
  beat is computed. Edit banks, the chain, rate, bar, bars and lane lengths, and
  upgrade explicitly with undo. Re-anchor on the listed transport changes. The browser
  scheduler and direct CV (two more gates) follow the six lanes.
- **Rack module:** parse version 3, follow the timeline, provide six-lane outputs and
  32 PPQN, and regenerate the fixture from HydraPy (Phase 3).

Until then no implementation accepts version 3, and the fixture is unchanged.

## 2026-10-26 — a transport change keeps the hit it lands on

HydraPy `e5999b2282bd9766b6f7152b4278c3da4e72ed8e` (#60, `TrackerCV`) · Rack module
`GorgonBridge` 2.3.1 (#33).

On the workstation, 8 of 58 Seek-to-row cues lost their landing kick in Rack,
for two reasons, and with OFFSET +24 ms the first downbeat after Play never
fired. [cv-semantics.md](cv-semantics.md) adds two rules for a transport change:

- **Retrigger**: a gate that was high on the sample before it goes low for 1 ms.
  A cue made while the previous kick's gate was still open landed on a gate that
  was already high, so no new edge reached the instruments (4 of the 8; this
  happens to any follower, however soon it hears the change).
- **Landing gate**: when the playhead landed inside its row's gate window and
  the follower is still in that row, the row's gates last as long as they would
  have from the landing, starting at the first available sample. Gates were
  purely phase-based, so a change that reached a follower after the window
  closed lost the hits it landed on (the other 4, and the OFFSET downbeat).

The reset pulse already latches the same way. The fixture's rows are stateless
and do not change.

## 2026-10-26 — pattern-rate timing (proposal)

Proposes per-pattern ¼×–8× rates with a stable master BPM, explicit beat/row
conversion, shared note/visual automation timing and a future version-3 score.
See [pattern-timing.md](pattern-timing.md). This entry is a design proposal:
no schema, player, fixture or pin change is included. The existing formats and
implementations remain the normative contract until the next contract PR.

## 2026-10-26 — per-hit sound controls and Rack sound link (specification)

Spec-first addition; implementations follow in separate PRs. Introduces optional
score format `hydrapy-tracker-2`, four track sound defaults, per-hit raw parameter
and machine locks, and the version-1 atomic Rack sound link. Version-1 scores,
their legacy CV fixture, ByteBus and direct CV maps remain unchanged. No firmware
is included. See [sound-locks.md](sound-locks.md) for scope and resolution rules.

## 2026-10-25 — short seeks are seeks

HydraPy `20e9276f74bd469a29a4d370086cae744ff72c66` (unchanged) · Rack module
`GorgonBridge` 2.2.1.

[cv-semantics.md](cv-semantics.md): at a transport change a follower that runs
its own clock jumps to the playhead however small the change. The Rack module
treated any difference under 0.1 beat as drift, so a seek shorter than that (a
fifth of a row back, say) was slewed at the 0.5% trim and stayed up to 27 ms
off for seconds. Release blocker in `release.json`.

## 2026-10-25 — native Rack ByteBus connector

HydraPy `67d08975a17b203652ecb53bb777e80446626816` (unchanged) · Rack module
`GorgonBridge` 2.2.0 (implementation follows this specification).

Adds [bytebus-output.md](bytebus-output.md): BUS OUT encodes Z, Roll, Feedback,
or a four-lane hit mask as one clocked byte, with canonical 0/10 V lines and
transport-aware publication. Existing score, bridge and direct CV semantics
are unchanged. This Rack-side connector needs no HydraPy implementation
change, pin move, or fixture regeneration; the fixture must remain identical.

## 2026-10-25 — first written contract

HydraPy `96a4cdb` (main after PR #47) · Rack module `GorgonBridge` 2.1.0.

Written from the implementations as they stood; nothing changed in either to
produce it. Describes:

- Score format `hydrapy-tracker-1`, with the optional `scenes` field added by
  HydraPy PR #46.
- Bridge API: `GET /api/bridge`, `POST /api/tracker_transport`,
  `POST /api/bridge_params`, the `studio.json` discovery file and the
  `X-Hydrapy-Token` header.
- Signal semantics as `TrackerCV` computes them, including the 5 V direct / 10 V
  Rack level split and the Rack 5 V option added in DSP_Emulation PR #5.
- The four JACK listening-audio ports added in HydraPy PR #44.
