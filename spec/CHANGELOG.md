# Contract changelog

Newest first. Each entry names the HydraPy commit and Rack module version it
describes.

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
