# Gorgon system roadmap — October 2026

A plan across Gorgon, HydraPy and DSP_Emulation, written from the design
decisions made on 2026-10-26. It is **not a contract change**: nothing here
alters `spec/`, the HydraPy pin or the Rack outputs until the PRs it names land.
Proposed home: Gorgon `docs/ROADMAP.md`, next to
[DRUM_MACHINE.md](DRUM_MACHINE.md), which it extends and in places supersedes.

Snapshot inspected: Gorgon `edfd116`, HydraPy pin `e5999b2`, DSP_Emulation
`3a7447b`. Versions: GorgonBridge 2.3.1, Machinemodule 2.0.0, Vastish 2.5.0,
OtisLab 2.8.0, ByteBusLab 2.2.0.

## 1. Decisions this plan is built on

1. **Gorgon's job is time and events. Sound lives in Rack.** No real-time
   model inference anywhere on the tick. RAVE is dropped. The Stable Audio
   fine-tune becomes an offline sample renderer (§3.F).
2. **Six lanes, not bound to engines.** A lane is a typed event stream — hit,
   pitch, velocity, locks, command — and routing to destinations is a separate
   layer. Six is the default, carried as a constant in v3, not hard-wired into
   the schema's shape forever.
3. **Timing: three numbers per pattern.** `rate` (rows per quarter note, from
   [pattern-timing.md](../spec/pattern-timing.md)), `bar` (rows per bar), `bars`
   (bars per pattern, up to 128 rows total). **The cycle is the bar**, not the
   pattern loop. Per-lane length is allowed (polymeter against the bar).
   Bar counters reset on pattern entry and at a transport change.
4. **One pure evaluator is the central artifact of v3.**
   `evaluate(score, transport, interval) → events`, with no hidden state: the
   same inputs give the same events in every implementation (HydraPy
   scheduler, browser audition, Rack engine). **Conditions live in the cell**,
   next to note, velocity and locks, and are typed data, not code.
   **Transforms live on the lane**, as a bounded tree. Probability is a hash of
   the seed and position, never a wall-clock dice roll. PRE/NEI are deferred
   because they are the only Elektron conditions that would need state.
5. **The byte bus is the control fabric.** Gorgon's existing BUS OUT stays;
   Gorgon gains BUS IN for FILL and mutes. Counter is built before Pipeline.
   Machinemodule's TRIG gains a bus mode.
6. **Retired:** DrumLab Percussion (Machinemodule is the drum destination;
   OTIS is the sampler). [pattern-timing.md](../spec/pattern-timing.md) is
   subsumed by score v3 rather than implemented on its own.
7. **Added destinations, no new sequencer code:** Vastish gains an audio input
   (K2500 Live Mode); a Monomodule adapter reuses the Machinemodule adapter;
   OTIS gains EPS-style patch-select bits; the Analog Four and Rytm are reached
   through Rack's Core MIDI modules.

Open questions are collected in §5.

## 2. Workstreams

Each item names its repository per [AGENTS.md](../AGENTS.md). Sizes are
rough: **S** an evening, **M** a few sessions, **L** a multi-PR feature.

| ID | Item | Repo | Size | Depends on |
| --- | --- | --- | --- | --- |
| A | Score v3: lanes, timing, conditions, transforms, bus I/O — merged (§3, phases 2–3) | Gorgon spec → HydraPy → Gorgon rack | L | — |
| B1 | Ratify byte bus §11 | DSP_Emulation docs | S | — |
| B2 | Counter module — shipped, ByteBusLab 2.3.0 | DSP_Emulation bytebus | M | B1 |
| B2b | Counter run modes (Loop, One-shot, Pendulum), ByteBusLab 2.4 | DSP_Emulation bytebus | S→M | B2 |
| B3 | Machinemodule BUS TRIG mode | DSP_Emulation machinemodule | S | B1 |
| B4 | Switch (with MIX), then Pipeline | DSP_Emulation bytebus | M | B2 |
| B5 | Lookup JUMP: clocked SHAPE step | DSP_Emulation bytebus | S | — |
| B6 | Lookup2: two-input transfer module | DSP_Emulation bytebus | M | B5 |
| B7 | Counter 2: rational increments and ratio tables | DSP_Emulation bytebus | M | B2 |
| B8 | Lookup library v2: envelope, sequence and density-ordered rhythm families | DSP_Emulation bytebus tools | M | — |
| C1 | Sound link v2: six lanes — merged, Gorgon #52 | Gorgon spec/rack + Machinemodule | M | A |
| C2a | Machinemodule adapter refactor, no behavior change | DSP_Emulation machinemodule | M | — |
| C2b | Monomodule Rack adapter | DSP_Emulation (new `vcv/monomodule`) | L | C1, C2a |
| D1 | Vastish AUDIO IN (mono, then poly) | DSP_Emulation vastish | S→M | — |
| D2 | Vastish chain mode (triple) | DSP_Emulation vastish | M | D1 |
| E1 | OTIS patch-select bits | DSP_Emulation otislab | S | — |
| E2 | Otis Kit + Capture | DSP_Emulation otislab | L | E1, A |
| F1 | Retire DrumLab | DSP_Emulation + this doc | S | — |
| F2 | Diffusion model as offline renderer | outside these repos | M | — |
| G1 | Latency measurement and per-destination offsets | Gorgon rack | M | — |
| G2 | Event trace in all three evaluators — merged, Gorgon #53 | Gorgon spec → HydraPy → rack | M | A |
| H1 | A4/Rytm as MIDI destinations — deferred | documentation only | S | — |

### A. Score v3 — `hydrapy-tracker-3`

This is the big one and it goes through the three-PR sequence. It should be
**several spec PRs**, one concern each, before any implementation.
**Merged 2026-10-26–27:** the four specs are Gorgon #41, #42, #43 and #46
([score-v3.md](../spec/score-v3.md), [evaluator.md](../spec/evaluator.md),
[transforms.md](../spec/transforms.md), [bus-io.md](../spec/bus-io.md)), with
follow-ups #47 and #48; HydraPy implements them in #63–#65, #67 and #68, and the
Rack module in Gorgon #49, #51 and #52 (GorgonBridge 2.4.0). The plan as written:

- **A-spec-1: lanes and timing.** Six lanes; `rate`/`bar`/`bars`; per-lane
  length; the generalized beat/row conversion; migration from v1/v2.
- **A-spec-2: the evaluator.** The event record (stable id, lane, rational
  beat position, duration, note, velocity, locks, origin: which cell and which
  transform step produced it), the `evaluate()` signature and its invariants,
  cell conditions, bar counters, seed, fill-from-beat. This PR carries
  **hand-computed vectors** for the awkward combinations — a 7-row lane against
  a 16-row bar, `1:3` plus `%` plus `ply 4`, a seek mid-bar, a FILL arriving
  mid-bar — in the style of `BYTE_BUS_SPEC.md` §6, so HydraPy has something to
  reproduce before the generated fixture exists. It also defines the trace
  format (§G2).
- **A-spec-3: lane transforms.** The bounded transform tree and its semantics.
- **A-spec-4: bus I/O.** BUS OUT hit mask widened; BUS IN defined; the
  bridge's transport endpoint extended so a follower can set `fill` and
  `mutes`.

Then one HydraPy PR per spec PR (validator, timing helpers, editor, browser
scheduler, direct CV, scene/automation evaluation), then the Rack PR with pin
move and regenerated fixture. §4 has the concrete proposal for A-spec-1..3.
The order of proof is: spec vectors → HydraPy reproduces them → fixture
generated from HydraPy → Rack matches the fixture. The fixture cannot precede
HydraPy, because HydraPy's `TrackerCV` is what generates it (AGENTS.md). Add
property-based tests in HydraPy over random lane lengths, conditions,
transforms and transport jumps, checking the evaluator's invariants rather
than fixed outputs; the fixture then samples that space.

**The internal event model is lane-indexed, not destination-indexed.** The
sound link's six channels, the Rack jack count and the hit-mask width are
projections of it, and none of them is the definition of a lane. v3 validates
exactly six lanes; the schema shape is a list.

**What changes in Rack** (A, with C1): GATES/VEL/V-OCT become six channels;
individual GATE jacks go to 1–6 if HP allows, else the poly jack plus Split.
Existing port IDs are preserved and new ones appended, as the manual requires.
The sound-link packet grows from four lanes to six under a new ABI version.
BUS OUT's hit mask uses bits 0–5; bit 6 is FILL, bit 7 reserved.

**What changes in HydraPy:** the editor's Audio page shows six lanes, a pattern
header with rate/bar/bars, a per-lane length, a condition field in the
selected-cell editor, and a transform list per lane. Direct CV gains two gates
(ES-40 maps are HydraPy's own, not contract). The browser audition synth
follows the same rules (it is the third implementation, per `spec/README.md`).

### B. Byte bus

- **B1.** Ratify [BYTE_BUS_SPEC.md](https://github.com/JosiahAlanLee/DSP_Emulation/blob/main/docs/BYTE_BUS_SPEC.md)
  §11 as written (name aside). The sample-rate vs latched question is already
  settled by "mode by channel count" and "latching modules own an internal
  clock"; nothing downstream should wait on it.
- **B2. Counter** — shipped as ByteBusLab 2.3.0 (DSP_Emulation #32, #33;
  contract `vcv/bytebus/docs/COUNTER.md`): a latching module per §5 with an
  increment from a knob or a second bus, a modulus, a CARRY pulse, RESET to a
  seed word and Register's clock ownership. It was the missing piece of
  Counter → Lookup (rhythm family) → Machinemodule TRIG, the bytebeat-kit chain,
  and of Counter → Lookup → Converter DAC → any filter, an ESQ-1/SQ-80-style
  DOC oscillator. Erfurt is the reference role; the module is original.
- **B2b. Counter run modes.** A contract change to `COUNTER.md` and
  ByteBusLab 2.4, adding **One-shot** (count until an addition would wrap, then
  hold) and **Pendulum** (up to the top, reflect, down to zero, reflect) beside
  today's **Loop**, so Counter → Lookup → Converter is a ramp, an envelope or a
  triangle as well as a phase. The reference for a bouncing count is SCANNER's
  edge reflection rather than META's pendulum, which reverses on a trigger. The
  contract has to settle, and the proposal is: one-shot counts up only and
  holds at `MOD − 1` whatever the overshoot, with CARRY once on the edge that
  reaches the hold, so `MOD − k` is a large step up rather than "down by k" and
  a falling envelope is the rising one through Lookup's Curve shape 32;
  pendulum treats the increment, knob or bus, as a magnitude and carries the
  direction as its own state, saved with the patch, set to up by RESET/LOAD,
  with CARRY on each reflection; an overshoot clamps to the end word rather
  than folding back, so every swing reaches 0 and `MOD − 1` and shows each for
  one edge (`MOD` 8, increment 3: 0 3 6 7 4 1 0 3 …); Engine rate follows the
  same rules every sample; MODE changes take effect on the next edge from the
  current word. In Loop nothing changes, including `MOD − k` meaning "down by
  k". Shipped as ByteBusLab 2.4.0 (DSP_Emulation #44) as proposed; its edge
  cases (a 00 increment holds, a word above the top goes to it with CARRY,
  the direction survives MODE changes) are in `COUNTER.md`, Run modes.
- **B3. Machinemodule BUS TRIG.** Today a poly cable into TRIG is sixteen tracks,
  channel = track, and a bus works only by accident: bit *k* rising fires track
  *k*+1, channel 9 fires track 9 on every publication, and a bit held high
  across sample instants never retriggers. A 9-channel cable is ambiguous
  between "9 tracks" and "8 bits + CLK", so this must be explicit: a context-menu
  mode (or a BUS jack) in which a hit fires on each CLK rising edge where the
  bit is high, bits 0–7 → tracks 1–8. The semantics are the bus spec's §5 and
  nothing else: a hit on track *k*+1 at engine sample *s* iff the `Reader`
  reports a rising CLK edge at *s* and bit *k* of `rawWord` is high at *s*.
  Data and CLK share the cable, so module processing order cannot split them;
  the one-sample hop is the documented latency. Bus spec untouched. With B2
  this is the first playable result of the whole bus effort. Shipped as
  MachinemoduleRack 2.1.0 (`vcv/machinemodule/docs/BUS_TRIG.md`).

  Two usage rules that the manual should carry, because the v3 BUS OUT word
  (A-spec-4) is not a trigger stream: it is a state word — bits 0–5 the hit
  mask, bit 6 FILL, bit 7 reserved — published on CLOCK edges and transport
  changes, and Machinemodule's bus mode fires on *every* publication where a
  bit is high. Patched straight in, FILL would retrigger track 7 while held and
  a long hit would retrigger on every clock. So: Gorgon drives Machinemodule
  through the sound link or GATES, never BUS OUT; BUS TRIG is for Counter,
  Lookup and Register sources, where "a hit on each clock while the bit is
  high" is the rhythm the chain is for. A v3 word that must reach BUS TRIG goes
  through an AND mask that clears bits 6–7 (Vastish FUN today, Lookup2 later).
  The mask removes FILL, not the per-clock retrigger: a hit still fires on every
  CLOCK edge its span covers, so the word gives one trigger per hit only when
  CLOCK pulses once per row (4 PPQN at rate 1) and the lane has no `ply`;
  faster clocks repeat each hit and slower ones skip rows. A patched TRIG also
  takes that instance off the sound link, so it gets no per-hit locks.
- **B4. Switch, then Pipeline.** Order flipped from the current roadmap.
  Pipeline is the canon-of-the-kit effect; Switch is the A/B pattern-byte
  commutator and reclocker, plus a MIX mode, `out = A + (B − A) · c / 255` with
  `c` a third byte — the Via's crossfade-under-a-contour as a word. Both are
  small once Counter has set the pattern for latching modules. Switch is
  ByteBusLab 2.6.0 (DSP_Emulation #46, `vcv/bytebus/docs/SWITCH.md`): Select
  (a bit of C chooses A or B), Bits (each bit of C chooses) and Mix, with B and
  C falling back to knob words, transparent on A's clock or reclocked on its
  own. Pipeline is ByteBusLab 2.8.0 (DSP_Emulation #49,
  `vcv/bytebus/docs/PIPELINE.md`): a 1–64 edge delay over a 128-word history,
  SPREAD delaying bit *k* by *k* × SPREAD more edges (the scramble role, as the
  canon of the kit), and LOOP repeating the last DELAY words. Its echo meets
  Switch exactly on time with Pipeline in A and the pattern in B.
- **B5. Lookup JUMP.** A SHAPE step on the existing Lookup, so logic patched
  in changes the transfer function rhythmically. JUMP is a **bus input with a
  bit selector**, not a gate: SHAPE advances by one (wrapping) on each
  publication where the selected bit is high, exactly the BUS TRIG rule, so two
  adjacent high words make two steps and a bit held across publications steps
  on each. A gate-edge JUMP would merge adjacent highs. The step is persistent;
  a held offset ("+1 while high") is a different thing and already exists as
  attenuated SHAPE CV (SHAPE CV adds 64 positions per 10 V, so one shape is
  10/64 V, about 0.156 V; a 10 V bit straight in is one full turn and lands
  where it started). SCANNER's JUMP, which reverses the scan or teleports it
  to its origin, is the inspiration, not the behaviour.

  Gorgon offers JUMP no play-start event. A **play start** meaning for BUS OUT
  bit 7 was proposed in review and dropped (2026-10-27): its publications off
  the CLOCK grid would be extra clocks for every receiver on the cable. The v3
  word stays a state word with bit 7 reserved, so B5 needs nothing from
  A-spec-4; a BUS OUT lane bit patched into JUMP steps once per publication
  its hit covers, the B3 rule. Shipped as ByteBusLab 2.5.0 (DSP_Emulation
  #45): an unclocked JUMP or a gate steps on the bit's rising edges, and RESET
  also clears the steps.
- **B6. Lookup2.** A two-input transfer module, `f[family][shape][A][B]` → byte,
  with JUMP from B5. It subsumes the bit-logic roles (Lipsk, Gera) as the
  shapes `A XOR B` and `A AND B` and adds everything between — product,
  min/max, difference, 2D folds, Walsh of two words. Sizing: a 64 KiB table
  per shape is nothing, but 64 shapes per family is 4 MiB per family, so the
  algebraic shapes (logic, arithmetic, min/max, comparison) are computed in
  code and only the genuinely tabular families are stored, at most two, with
  sixteen shapes or fewer each. SCANNER's two-input scan is the inspiration:
  there the MAP parameter chooses the combining operation (add, multiply,
  difference, lighten; OR, AND, XOR, > on the logic outputs) and Z deforms the
  terrain. The tables here are original and ship through the same generator
  and library-identity rules as Lookup's. When it lands, `BYTE_BUS_SPEC.md`'s
  role table moves Lipsk/Gera from Vastish FUN to Lookup2. Shipped as
  ByteBusLab 2.9.0 (DSP_Emulation #50) with every family computed, so no tables
  and no library identity: Logic (all sixteen two-input functions), Arithmetic
  (sums, mix, ring, product, min/max, comparisons), a continuous two-input Fold
  and 64 two-word Walsh maps, each one-to-one in A and in B; the role table
  moved.
- **B7. Counter 2.** A RATIO mode: the increment is `256 · N / D` per clock with
  a fractional accumulator, giving a phase word at N/D of CLK — SYNC's
  clock-ratio contour as a byte, and a polymeter source for the modular locked
  to Gorgon's CLOCK. SYNC's scale grids (integers, odds, multiples of three;
  rhythmic harmonics, no-triplet, triplet, dotted) become a Lookup family that
  maps a byte to a ratio. This changes Counter's "increment is a byte" model,
  so it is a second version, not part of B2. Shipped as the Counter 2 module,
  ByteBusLab 2.10.0 (DSP_Emulation #51): N 1–16 and D 1–64 on knobs, an exact
  integer accumulator, one step per clock edge. The ratio Lookup family is
  library v3, ByteBusLab 2.11.0 (DSP_Emulation #56): Lookup's FAMILY 10 maps a
  byte to a ratio word, `(n − 1) << 4 | (d − 1)`, over eight grids named after
  SYNC's (Integers, Odds, Threes, Harmonics, Straight, Triplet, Dotted, All), and
  Counter 2's new RATIO input takes that word in place of N and D.
- **B8. Lookup library v2.** Three families the v1 library lacks, all authoring
  work in `generate_lookup.py` rather than DSP: an **envelope** family
  (expo/log pairs, plateaus and cliffs, lumps, variable sustain) so a one-shot
  Counter through Lookup is an envelope generator; a **sequence** family
  (steps, ridges, Euclidean ridges, bounce) for stepped CV from a counter; and
  a **density-ordered rhythm** family in which each SHAPE step adds a hit
  without removing one, so SHAPE CV is a deterministic fill control: Gorgon's
  FILL bit through an attenuator at 10/64 V onto SHAPE CV is "fill = density
  + 1" while held — GATESEQ's PTN knob, whose documentation says only that
  density increases clockwise; the nesting property is our design — and the
  non-random counterpart of `%p`. Library v1 is immutable, so this is a new
  library identity. Library v2 is ByteBusLab 2.7.0 (DSP_Emulation #47):
  Envelope, Sequence and Density as FAMILY 7–9, after Custom, so saved FAMILY
  values keep their meaning; v1 regenerates byte-identical.
- **Not needed:** a bus-aware control machine in Machinemodule. Converter DAC →
  a P1–P8 CV input is already byte-as-parameter-lock, latched at the hit, and
  mono CV broadcasts to all tracks. Per-track byte values don't fit on one cable;
  don't design for them.

### C. Elektron side

- **C1. Sound link v2.** Six lanes in `spec/sound-link.hpp`, ABI version bump,
  Machinemodule receiver updated. Lands with the Rack PR of A. Machinemodule
  gets lanes 1–6 of its sixteen tracks; the rest stay on TRIG/CV. Merged with
  A's Rack PR, Gorgon #52 ([sound-locks.md](../spec/sound-locks.md#rack-sound-link)),
  and Machinemodule 2.2.0 (DSP_Emulation #38) reads version 2.
- **C2. Monomodule adapter — two stages, not one.** Monomodule (Shnolk,
  AGPL-3.0) is a sibling of the Machinemodule core on the same dsp56300
  emulator; the Machinemodule adapter already compiles Monomodule's resampler.
  - **C2a. Refactor the Machinemodule adapter into shared infrastructure with
    no behavior change**: worker thread, SPSC queues, generations, sound-link
    receiver, P-CV capture at trigger, OS loading. Regression: the existing
    portable and Rack tests, plus the README's live hit-timing measurement
    (24.2 ms ±0.35 after Gorgon's gate) reproduced on the workstation. Write
    down the AGPL/BSD boundary of the shared code *before* extracting it.
  - **C2b. Monomodule on that infrastructure.** This is more than
    configuration: the MnM is pitched, so the lock path carries the lane's
    note as the track note (the MD adapter's "does not guess which parameter
    is pitch" rule inverts); its parameter pages differ; and the FX machines
    mean a track can process external audio, so the adapter exposes an audio
    input per track or a separate FX module. Six tracks, OS 1.32B. Live
    validation is Linux only, as with the MD.
  A failure in C2b should be attributable to the emulation integration, not to
  the refactor; that is what the stage split buys.
- **Deferred, deliberately:** MD master reverb/delay/EQ/dynamics (Rack effects
  fill it), UW RAM machines (Spoon Bender CAPTURE → OTIS already resamples the
  whole patch, which is the UW's signature move done better), CTR machines.

### D. Vastish

- **D1. AUDIO IN (Live Mode).** One input that replaces the oscillator block at
  the engine rate, through the gate-free DSP path BUS wave already uses
  ([BUS_SPEC.md](https://github.com/JosiahAlanLee/DSP_Emulation/blob/main/vcv/vastish/BUS_SPEC.md)
  §BUS wave). Mono first: one input, voice 1's chain, every DSP block exercised
  with external audio, feedback through Rack's one-sample loop checked. Then
  polyphonic, with the audio cable's channel count establishing voices the way
  pitch/gate cables do now, so Machinemodule's sixteen TRACKS or the modular go
  through VAST shapers and filters per channel. The K2500 could do this for one
  input. Highest leverage item on the list; "small" applies to the mono step.
- **D2. Chain mode.** Vastish immediately to the right of Vastish continues the
  chain (the Séance row pattern): the K2600's triple modular processing. Real,
  but it is more of the same; after D1 and the bus work. Shipped as VastishLab
  2.8.0 (DSP_Emulation #52, `vcv/vastish/docs/CHAIN.md`), opt-in from the right
  Vastish's menu: each voice takes the left voice after its blocks and before
  its amp, on the left's notes and gates, through Rack's expander messages
  (one sample per hop), and applies its own amp envelope.

### E. OTIS

- **E1. Patch-select bits.** The EPS idiom: two buttons, four layer
  combinations per hit, which is how EPS players varied a part live. Two bits
  (from the existing FRAME BUS word, or a second bus input — decide in the
  contract) select among four stored region/loop/degradation presets per hit.
  With Gorgon's hit mask or a Lookup supplying the bits, this is the
  sampler-side replacement for DrumLab's retired four-lane kit.
- **E2. Otis Kit and Capture** stay as described in DRUM_MACHINE.md §"An Otis
  flavor" — a bounded bank, slot/slice per hit, record-then-play with
  generation handoff. E1 is the first step toward it; the rest waits for the
  v3 event path so hits carry slot and region as locks. Otis Kit's playback is
  OtisLab 2.10.0 (DSP_Emulation #53, `vcv/otislab/docs/KIT.md`): eight slots
  packed into one ES5505 memory and played on OTIS's own voice code, slot *n*
  as chip voice *n*, bit-identical to OTIS playing each sample alone; GATE
  channel *n* or SLOT's bits 0–2 choose the slot per hit, and a folder of F2
  renders loads into the slots. Capture is OtisLab 2.11.0 (DSP_Emulation #55,
  `vcv/otislab/docs/CAPTURE.md`): an 8 HP expander right of Otis Kit that
  records IN, the kit's own output or both for 1/4 to 32 beats (of CLOCK, or at
  TEMPO) into a chosen slot. The take replaces the slot only when complete, a
  hit already sounding keeps the take it started with, and takes save with the
  patch. Slot and region as sound-link locks, and per-slot outputs, are
  possible follow-ups.

### F. Retirements and redirections

- **F1. DrumLab.** Archive `vcv/drumlab` (README pointer, no release bundle,
  CI job removed). DRUM_MACHINE.md's "First implementation: DrumLab Percussion"
  gets a status line pointing here and at Machinemodule. Nothing in the
  contract referenced it.
- **F2. Diffusion model → offline renderer.** A CLI, living with the model and
  not in these repos: prompt or caption in, WAV out, into a folder OTIS can
  load (one sample now; a bank once E2 exists). Each WAV gets a JSON sidecar
  manifest, in the spirit of OTIS's Lookup export: model and checkpoint, seed,
  prompt/caption, generation parameters, sample rate and channel layout, BPM
  and key, and — computed after rendering — transient positions and candidate
  loop points. BPM and key alone are not enough to make a rendered hit
  reusable later. The nonsense-word caption scheme is the prompt vocabulary.
  Gorgon never calls it on a tick; a "render" is a thing you do between takes. The ControlNet idea (record
  Machinemodule with pitch/timing labels for a second fine-tune) stays a
  research note until there is a reason to prefer a generated hit over a
  Machinemodule one. The manifest exists (2026-10-27): the breaks model's
  `newbreak.py` writes one beside every WAV through `render_manifest.py`, on
  the workstation in `shared/claude/breaks`, and the copy check adds its
  result. Its transients match breakgen's labelled hits at recall 0.875 and
  precision 0.876. OTIS loads the WAVs as they are. The caption vocabulary is
  still the descriptive one the model was trained on.
- **F3. pattern-timing.md.** Its `rate` concept, conversion formulas and
  transport-discontinuity rules are carried into A-spec-1 unchanged; its fixed
  64 rows and four lanes are not. Mark it superseded in `spec/README.md` once
  A-spec-1 merges, rather than deleting it.

### G. Timing

- **G1. Measurement and per-destination offsets.** Gorgon's single OFFSET
  (±100 ms, picture vs Rack) stays the contract-level control. Alignment
  between Rack destinations (Machinemodule's ~23 ms queue, a Monomodule
  adapter, the ES-3 path, USB MIDI to the Elektron boxes) is a Rack patching
  matter today. The plan: a small measurement utility — emit a click on a gate,
  listen on an audio input, report the round trip in samples — if nothing in
  the VCV Library already does it, plus a table in the Gorgon manual of
  measured offsets per destination on the workstation. A number that is stored,
  not a session ritual. Ableton Link stays out: the contract says nothing
  follows an external clock, and nothing here needs it.
- **G2. Event trace.** With six lanes, conditions, a bridge, Rack and several
  destinations, "the hit didn't sound" has at least five causes. Each
  implementation of the evaluator can emit a trace line per event: event id,
  intended beat, evaluation result and why (which condition, which transform),
  dispatch time, and where measurable the observed arrival (Machinemodule
  already counts missed blocks and reports delay; the sound-link packet
  already carries the producer sample frame). HydraPy writes JSONL; Rack
  writes a bounded ring the menu can dump. The trace format is defined in
  A-spec-2 so all three implementations agree on it. Phase 2, not "when
  something breaks". Merged: the format is [evaluator.md](../spec/evaluator.md#trace)
  (Gorgon #42), HydraPy writes it with `--tracker-trace` (HydraPy #65), and the
  Rack module's ring is Gorgon #53.

### H. External MIDI destinations

- **H1.** Deferred as far-future (2026-10-26); kept here as written.
  Analog Four and Rytm MKI: each track answers to a note on its own
  channel and the parameter set is exposed over CC/NRPN. Gorgon's lanes →
  Rack → Core **CV-MIDI** (gate/pitch/velocity → notes) and **CV-CC** (a lane's
  automation or a Lookup byte through Converter → CC). No new code; a documented
  patch and a G1 offset entry each. The Elektron sequencers stay unused;
  Gorgon's conditions and 128-row patterns apply to both boxes regardless of
  their OS. Whether their MKI firmware has its own trig conditions is
  irrelevant to this path.

## 3. Sequence

**Phase 0 — ratify (no code).** B1. The §5 questions. Agree this document.

**Phase 1 — cheap, independent, immediately playable.** No contract change;
each is one PR in DSP_Emulation. **Done 2026-10-26:**

1. D1 Vastish AUDIO IN — mono (2.6.0), then polyphonic (2.7.0, #37).
2. B2 Counter — ByteBusLab 2.3.0 (#32, #33). B2b run modes are a follow-up.
3. B3 Machinemodule BUS TRIG — MachinemoduleRack 2.1.0.
4. F1 DrumLab retired from README, dev docs and CI; directory kept.
5. H1 deferred as far-future; G1's measurement utility still open (Phase 4: Ping).

After phase 1 the chain Counter → Lookup → Machinemodule plays, the modular
and the drum machine run through Vastish, and the bus experiment has had its
evening. If it does not click, nothing in phase 2 depended on it beyond BUS IN.

**Phase 2 — the event architecture, as spec.** A-spec-1 through A-spec-4 as
separate PRs, each reviewable on its own. A-spec-2 is the one to argue over:
the evaluator, event identity, the hand-computed vectors and the trace format.
Treat this phase as protocol design, not feature work; the PR review *is* the
architectural review before any implementation starts. **Done 2026-10-26–27:**
A-spec-1 to A-spec-4 are Gorgon #41, #42, #43 and #46, with follow-ups #47 and
#48 from the HydraPy implementation.

**Phase 3 — integration.** HydraPy implements the four specs with property
tests; fixture generated; Rack matches, with C1 and the pin/fixture move; G2
trace on both sides; the full event path validated on the workstation. This is
where the tracker becomes the instrument you will take to a show, so it is not
rushed to fit around phase 1. Merged 2026-10-27: HydraPy #63–#65, #67 and #68;
the Rack module's evaluator and fixture3 (Gorgon #49), BUS IN (#51), version-3
playback with C1's sound link v2 and the pin move (#52), and G2's trace (#53).
The workstation validation of the full event path is not part of these merges.

**Phase 4 — destinations on the new contract.** C2a refactor, then C2b
Monomodule; E1 OTIS patch select; G1 offsets table. C2a and C2b are merged
(DSP_Emulation #39–#41; Gorgon #54, #55). E1 is DSP_Emulation #43, OTIS 2.9.0,
taking §5.6's default. G1 is Ping, a latency meter in GorgonBridge (nothing in
the VCV Library sends a pulse and times its return), with the measured table in
the [Gorgon manual](../rack/docs/MANUAL.md#destination-latency).

**Phase 5 — depth.** B2b Counter run modes; B4 Switch and Pipeline; B5 Lookup
JUMP, then B6 Lookup2, B7 Counter 2 and B8 library v2 in whatever order the
patches ask for; D2 chain mode; E2 Otis Kit and Capture; F2's manifest and
renderer workflow; the tutorial video kit updated for six lanes.
Progress, 2026-10-27: B2b, B5, B4's Switch and B8 are ByteBusLab 2.4.0–2.7.0
(DSP_Emulation #44–#47), and F2's manifest is written beside the model. On the
defaults Josiah approved the same day, Pipeline, B6 and B7 followed as
ByteBusLab 2.8.0–2.10.0 (#49–#51), D2 as VastishLab 2.8.0 (#52) and E2's Otis
Kit playback as OtisLab 2.10.0 (#53). Then E2's Capture as OtisLab 2.11.0 (#55)
and the ratio Lookup family, library v3, as ByteBusLab 2.11.0 (#56). Left: the
tutorial kit. The six lanes settled the GUI decisions it waited on (Josiah,
2026-10-27), so it is unblocked.

## 4. Score v3 proposal (for A-spec-1..3)

Concrete enough to be argued with. Field names are provisional.

This is the proposal as it went into review; it is kept as the design record.
The ratified contract is [score-v3.md](../spec/score-v3.md),
[evaluator.md](../spec/evaluator.md), [transforms.md](../spec/transforms.md)
and [bus-io.md](../spec/bus-io.md), and where this section differs from them,
they win. The places where ratification changed the proposal are marked
*Ratified:* below.

### Shape

```
format     "hydrapy-tracker-3"
bpm, key, pattern, loop, sound, modes, bindings     unchanged
seed       integer                                   drives every probability
mutes      [bool…]                                   one per lane
lanes      [{name}…]                                 display names; v3 validates exactly 6
sounds     [sound…]                                  as version 2, one per lane
patterns   [pattern | null ×128]                     eight banks of 16, played by a chain
```

*Ratified:* 128 patterns in eight banks (A–H, 1–16), with a `chain` of 1–256
pattern indices as the song order; the proposal kept four
([score-v3.md](../spec/score-v3.md#shape)).

Lane-indexed arrays are lists whose length v3 pins to six by validation, not
by shape, so a later format can change the count without changing the model.

A pattern:

```
name       1–40 characters
rate       0.25 | 0.5 | 1 | 2 | 4 | 8     rows per quarter note = 4·rate   (pattern-timing.md)
bar        rows per bar, 1–32, default 16
bars       bars per pattern, 1–8;  rows = bar·bars ≤ 128
lanes      [{len, cells, transforms}…]              one per lane
             len        1–rows, default rows         the lane loops every len rows
             cells      [cell ×len]                  null or {note, vel, locks?, cond?}
             transforms [transform…]                 ordered; each a node tree, depth ≤ 3
keys, easing, scenes                                 as before; rows 0–(rows−1)
```

### Time

From pattern-timing.md with `rows[p]` in place of 64:

```
R[p] = 4·rate[p]              rows per quarter note
L[p] = rows[p] / R[p]         pattern length in beats
S[p] = Σ L[q] for q < p       start in the song
```

Within pattern `p` at beat `b`: `row_position = (b − S[p])·R[p]`,
`row = ⌊row_position⌋`, `row_phase` its fraction. For lane `n`,
`lane_row = row mod len[n]`. The **bar count** is position-based:
`bar_count = visits · bars[p] + ⌊row / bar[p]⌋`, where `visits` is how many
times the playing cycle has wrapped into this pattern since the transport
anchor, `⌊(beat − anchor_beat) / cycle_length⌋` when looping one pattern and
derived from the chain schedule otherwise. Row 48 of a 16-row-bar pattern is
therefore bar 3 whether you played into it or seeked to it, and when looping
one pattern `1:4` fires once per four loops. It is a pure function of the
transport anchor and the beat, so the fixture stays stateless. A transport
change re-anchors and so resets `visits` to 0; a seek never changes which bar
a row is in.

Gate, clock PPQN, reset and run semantics are unchanged from cv-semantics.md,
with `row` now ranging over `rows[p]` and the lane's cell taken at `lane_row`.

### Conditions

`cond` on a cell is a typed object with a `not` flag; display codes in
parentheses. The cell **fires** on a bar when its condition is true; a cell
with no condition always fires.

| kind | fields | true when | code |
| --- | --- | --- | --- |
| every | a, b (1 ≤ a ≤ b ≤ 8) | bar_count mod b == a − 1 | `a:b` |
| prob | p (1–99) | hash(seed, pattern, lane, row, bar_count) mod 100 < p | `%p` |
| fill | — | `fill` is true | `F` |
| first | — | bar_count == 0 | `1ST` |

*Ratified:* `prob` hashes the pattern `row`, not `lane_row`, so a short lane's
repeats draw independently; the proposal used `lane_row`
([evaluator.md](../spec/evaluator.md#chance)).

Negation prefixes `!`. **PRE and NEI** (Elektron's "previous condition on this
track / on the neighbour track was true") are the only stateful ones and are
deferred to v3.1 with a pure definition still to be written; shipping four
stateless kinds first keeps the fixture honest.

A lane whose `len` does not divide `bar` sees a given cell at different bar
positions; its condition applies to every occurrence within a qualifying bar.
This is correct and should be documented so it does not read as a bug.

### Transforms

Per lane, ordered, applied to the lane's events for the bar being queried.
Original subset; no Strudel code import (DRUM_MACHINE.md's position holds).

| transform | fields | effect on the bar's events |
| --- | --- | --- |
| every | a, b (1 ≤ a ≤ b ≤ 8), inner | apply `inner` on bars where bar_count mod b == a − 1 |
| rot | k (rows) | rotate the lane's cells by k rows within its len |
| rev | — | reverse the lane's cells within its len |
| degrade | p (1–99) | each event fires with probability p, same hash family as `prob` |
| ply | n (2–4) | each event repeats n times evenly within its row (ratchet); sub-row hits carry the parent's locks |

A transform is a typed node; `every` holds a child node, so the list is a
tree with depth capped at 3 in the schema. v3 implements these five node
types. The tree shape, the per-event `origin` field and a lane-indexed (not
destination-indexed) event record are what keep the door open to hierarchical
composition and stream routing later without a format change.

*Ratified:* `every` takes `a:b`, the same test as the `a:b` condition; the
proposal had `every n` (2–8), true when `bar_count mod n == 0`
([transforms.md](../spec/transforms.md#shape)). The proposal also said that none
of the tree, `origin` or the lane-indexed record was implemented yet; all three
now are, in the schema's lanes and transforms and in both evaluators
([evaluator.md](../spec/evaluator.md#evaluate)).

Generators — Euclidean fills, a mini-notation string — are **editor tools that
bake into cells**, not score fields. The evaluator's invariants are the ones
DRUM_MACHINE.md already lists: querying a bar equals concatenating its partial
queries; no duplicate boundary hits; no wall clock; order-independent.

### FILL and BUS IN

`fill` is transport state owned by Studio — it travels in the playhead
snapshot (bridge-api.md), not in the score document — and it is **effective
from a beat, not from receipt**: the snapshot carries `fill` as a transition
`{beat, on}`, and the evaluator reads the value in force at the event's beat.
Sources: an editor key (Studio stamps the beat); a Rack **BUS IN** (nine
channels, bus spec levels) whose bits 0–5 are lane mutes and bit 6 is FILL; or
a plain FILL gate jack. A FILL change that originates in Rack is stamped with
the module's current beat, applied by the Rack engine to its own outputs at
that beat immediately, and reported to Studio through
`POST /api/tracker_transport` extended with `fill` and `mutes` fields
(`bridge_params` is for patch controls and is not the right endpoint). Studio
and the other followers then apply it from the stamped beat, late by one poll
(~35 ms) but never disagreeing about which bar it took effect in. The picture
being 35 ms behind on a fill is invisible; the Rack outputs are not late. If
Rack's stamped beat is already past for Studio, Studio applies it from the
stamp, not from receipt: the record is consistent even when the delivery was
not. Mutes get the same transition shape. BUS OUT's hit mask gains bit 6 =
FILL so a patch can see the state it set.

*Ratified:* BUS IN shares BUS OUT's layout, lanes on bits 0–5, FILL on bit 6 and
bit 7 reserved; the proposal had FILL on bit 0 and the mutes on bits 1–6
([bus-io.md](../spec/bus-io.md#one-word-layout)). For the rest of this
subsection, bus-io.md's bus layer, stamps, windows and lease are the contract.

### Migration

v1 and v2 load as: six lanes with lanes 5–6 empty, `rate 1`, `bar 16`,
`bars 4`, `len 64`, no conditions, no transforms, `seed 0`. Opening or saving
does not upgrade; the first v3 edit does, explicitly, with undo restoring the
old format — pattern-timing.md's rule.

## 5. Decisions to ratify

Defaults are what the plan assumes if nobody objects.

1. **Six lanes** — what fixes six? If it is the Monomachine's six tracks, say
   so in the spec; it changes nothing but it should be written down.
2. **Pattern count** stays four in v3. Raising it is a separate migration.
   *Superseded:* ratified as 128 patterns in eight banks, with a chain
   ([score-v3.md](../spec/score-v3.md#decisions-this-page-encodes)).
3. **FILL semantics**: effective-from-beat with Rack applying its own changes
   immediately and Studio reconciling from the stamp (default), vs
   Studio-only ownership with ~35 ms applied to everyone. The default keeps
   one record and gives Rack zero latency; the cost is the transition shape in
   the snapshot.
3a. **`evaluate()` is pure** — no `state` argument. The price is deferring
   PRE/NEI (decision 5); the benefit is the fixture and three implementations
   that can be compared at any beat.
4. **BUS IN bit map**: FILL + six mutes (default), vs FILL only with a
   separate mute input.
5. **PRE/NEI** deferred to v3.1 (default), vs defined now with a recursion
   bound.
6. **OTIS patch-select**: two bits of the existing FRAME BUS word, vs a second
   bus input. Default: a second input, so frame scanning and layer selection
   can come from different sources. E1 (DSP_Emulation #43) takes the default:
   a PATCH input.
7. **Counter before Pipeline** in the bus roadmap (default yes).
8. **Byte bus name** — still "Harriot" pending a better one. Not blocking.

## 6. Release-blocking, already enforced

These are not guidance; CI fails without them, and the plan relies on that:

- `rack/tests/fixture.txt` byte-identical to the pinned HydraPy's output.
- `spec/score.schema.json` accepts and rejects what HydraPy's `validate()` does.
- Existing Rack port IDs preserved; new ports appended (the Rack API tests for
  `bytebus-output.md` already check old output IDs).
- v1 and v2 scores load and play unchanged; opening or saving never upgrades.

## 7. Parked

Pinned as interesting, with no phase and no claim on time. Each is written down
so the engines and generators built above are built in a way that keeps it
possible.

- **A bus firmware for the Via dev board.** The ByteBusLab engines are
  header-only C++ with no Rack dependency, and the Lookup library comes from a
  Python generator, which is the same shape as Via's own dev environment. The
  board (STM32F373, 1V/oct on a 12-bit ADC, two 16-bit CV inputs, a DAC that
  drives the A/B crossfader and comes out as CV, main and aux logic I/O, six
  touch sensors) has two logic lines and no ribbon, so the byte cannot leave the
  module: the product is one Leibniz chain in a box — CV or logic in → byte →
  Counter/Register → Lookup → DAC — with the touch sensors choosing family,
  shape and mode. That is META's architecture with the slope generator replaced
  by a byte pipeline, driving the Via's own crossfader and S&H. Rule for the
  Rack work: keep the engines Rack-free and the tables generator-produced, so
  the engines re-target without a rewrite. The library does not: Lookup's
  `Library` today owns a mutable 96 KiB array loaded with `std::ifstream`, and
  the STM32F373 has at most 32 KiB of SRAM, so the tables would have to become
  flash-resident constant arrays, probably reduced, behind a different loader,
  with flash and real-time budgets checked. Not started; it is an embedded
  project with a flash cycle and the agent cannot hold the module.
- **New applets for the uO_C pair.** Same status.

## 8. Not in this plan

- Real-time model inference as a voice.
- Strudel or Vortex code import. An original pattern subset is not Strudel
  support, and the Vortex repository is archived on GitHub with development on
  Codeberg; vendoring ideas, not depending on the package.
- MD master effects, UW RAM machines, CTR machines.
- A Leibniz-compatible hardware interface. The bus is Rack-only by design.
- New hardware.

## 9. Review notes (2026-10-26)

A second review (GPT Sol) of the first draft led to these changes: the
evaluator named as the central artifact with a stable event record; spec-level
hand-computed vectors and property tests; lane-indexed arrays as validated
lists; transforms as a bounded tree; FILL effective-from-beat; BUS TRIG
semantics pinned to the bus spec's Reader; the Monomodule adapter split into a
refactor stage and an integration stage; Vastish AUDIO IN first and mono
first; a sample manifest for the offline renderer; the event trace (G2).

Two suggestions were not taken. A `state` argument on `evaluate()` would make
the fixture stateful and the three implementations harder to compare; PRE/NEI
are deferred instead. "Complete the shared fixtures before modifying HydraPy"
inverts the repository's process, where the fixture is generated from HydraPy;
hand-computed spec vectors fill that role before HydraPy, the fixture after.

A second pass the same evening extracted four concepts from the Starling Via
firmwares (SCANNER, META, SYNC, GATESEQ) into B4–B8 and the Counter run modes,
and opened §7 for the Via dev-board firmware and the uO_C applets.

The Via extraction was reviewed on #45 (Code, then GPT). Taken: Counter had
already shipped, so its run modes became B2b with their open decisions listed;
a 10 V bit on SHAPE CV is a full turn, so the density claim now goes through an
attenuator and JUMP is a persistent step, not a held offset; JUMP became
bus-aware so adjacent high words each step; Lookup2's tables are bounded and
its algebraic shapes computed; the Via references were corrected (MAP, not Z,
selects SCANNER's operation; its JUMP reverses or teleports; META's pendulum
reverses on a trigger; GATESEQ promises only that density rises); the v3 BUS
OUT word is a state word, not a trigger stream, with usage rules under B3; the
parked firmware's library needs a real port to a 32 KiB-SRAM part. A follow-up
pass recorded H1 as deferred, proposed One-shot as up-only and Pendulum as
clamping at its ends, and noted that the AND mask doesn't stop per-clock
retriggers. A "play start" definition for bit 7 was proposed and dropped: the
v3 BUS OUT word stays a state word with bit 7 reserved.
