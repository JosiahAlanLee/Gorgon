# A drum-machine direction for Gorgon and Rack

This is a design assessment and implementation sequence, **not an extension of
the score contract**. The current score, HydraPy pin and Rack outputs are unchanged.
Snapshot inspected: Gorgon `cb27eb7`, HydraPy `a6031d7`, DSP_Emulation `4587b48`.

## The important gap

The biggest obstacle is event semantics, rather than the number of synthesis
algorithms. Today's four music lanes contain only note and velocity. Z, Roll and
Feedback are continuous visual automation, not per-hit sound settings. Adding
more continuously held CV lanes would leave a locked sound in force until another
value arrived, and could retune a sounding tail. That would miss the behavior this
project is trying to capture.

Elektron's manual describes parameter locks as deviations from a kit's settings
for one note, with an unlocked next note restoring the kit value (printed p. 48).
The UW sample machines add start/end regions, envelopes and tempo-relative repeat
controls; live capture has separate record and play machines (Appendix A, pp.
A-15–A-16). These are useful behavioral references, without specifying the
manufacturer's DSP implementation. [Elektron manual, mirrored by B&H][md-manual].

Our stronger target is a repeatable audiovisual drum instrument: synthesized
percussion and Otis-derived samples share the same score events, and those events
can also drive scenes and ByteBus operations. It need not reproduce the original
machine's sound, limitations, panel or firmware.

## What can be reused

| Existing piece | Useful now | Missing for this goal |
| --- | --- | --- |
| Gorgon tracker and bridge | Master transport; sample-clock Rack follower; four polyphonic gates, held pitch and velocity; pattern editing and recovery | Kit sound definitions, per-hit sound locks, sub-row events, flexible lane count, queued kit/pattern recall |
| OTIS | ES5505-derived integer playback, WAV/recording, chip filters, regions, reverse, short windows, resampling and sample sharing | Sample bank, amplitude/hold/decay envelopes, per-hit playback snapshots, safe overlapping record/play generations |
| Vastish | Pitched/FM-like layers, envelopes, modulation and sample oscillator | A compact kit-oriented percussion interface; using a large voice patch for every drum is cumbersome |
| Spoon Bender | Effects and captures shared into OTIS | A sequenced capture destination and explicit record length, rather than capture only through the current interface |
| Séance / ByteBus | Spectral processing, reproducible transforms, clocked control | Optional destinations for drum events; neither needs replacing |

## First implementation: DrumLab Percussion

**Status: retired** (DSP_Emulation #36). Machinemodule is now the drum destination and OTIS the sampler; see [roadmap §F1](ROADMAP.md#f-retirements-and-redirections). The rest of this section describes the prototype as it was.

The companion DSP_Emulation work introduces an independent `DrumLab` plugin with
one `Percussion` module. It is a synthesis prototype, not a sampler or a complete
Machinedrum implementation.

- Six original models: swept-sine kick, body/noise snare, inharmonic/noise hat,
  percussive FM, a damped membrane-mode bank and clustered-noise clap.
- The default four-channel kit maps directly to Gorgon's four audio lanes. The
  module accepts up to sixteen trigger channels, but Gorgon still has four lanes.
- Machine, tuning, decay, color, contour, drive, bit depth, rate reduction and
  velocity are captured on each trigger. Existing tails retain their settings.
  A lane is monophonic, with a 1 ms retrigger crossfade.
- Direct Gorgon gates/pitch/velocity work with the existing contract; no extra
  browser runtime or clock is introduced. New sound-lock editing is still absent
  from Gorgon. External CV can supply per-hit changes in the meantime.
- Reset restarts the noise sequence. Individual audio and a stereo mix allow
  external filtering, sends and resampling into OTIS/Spoon Bender.

The [module manual][drumlab] describes pitch conventions, CV scales and validation.
The example patch connects the existing polyphonic ports. Keep Gorgon's gate
length below 100%: adjacent full-row gates do not have distinct rising edges.
The later event path must represent hits explicitly, independently of gate width.

## Next: kit defaults and explicit events

Keep sound state separate from transport and from the visual automation lanes.
A track refers to a kit sound; each event overlays only its own locks onto that
sound's defaults. The merge produces a complete immutable sound snapshot.

For example, a kit decay of 180 ms with a 45 ms lock on row 4 gives a 45 ms hit
at row 4 and a 180 ms hit on the next unlocked trigger. Neither changes the
already sounding hit. A rest does not implicitly trigger, restore or choke a
voice. A separate modulation path can deliberately bend a sustained voice later.

Before changing the score, specify:

1. A bounded event record: stable identity, lane, rational beat position,
   duration, note, velocity, sound/slot reference and typed parameter locks.
   No arbitrary Python or JavaScript in a score.
2. Kit ownership, editable defaults, kit save/reload and pattern recall rules.
   A live Control All gesture should have an explicit reload target.
3. Lock precedence, clamp/validation rules, absent locks, rests, muted events,
   triggerless modulation, slide and choke behavior. Triggerless modulation and
   slides must not silently acquire hit-snapshot semantics.
4. Retriggers and swing/microtiming as explicit timed events. Sub-row hits must
   retain their own resolved controls and not depend on video frames.
5. Deterministic seed and cycle numbering, revision changes, pause/resume, seek,
   loop wrap and pattern launch. An event already sounded must not be replayed
   by an ordinary edit or bridge refresh.
6. A bounded Rack event/snapshot representation evaluated on Rack's clock. The
   bridge polls around 30 Hz; sending an HTTP request for each hit would defeat
   the existing timing architecture. Samples remain in Rack, not JSON payloads.
7. Old-score compatibility, unsupported-feature reporting and the same event
   results for offline rendering, direct CV and Rack.

Follow [AGENTS.md](../AGENTS.md): spec-only PR first, HydraPy implementation and
tests second, Rack implementation plus pin/fixture third. This proposal does not
smuggle optional fields into `hydrapy-tracker-1` that old players would silently
ignore. Expanding from four lanes is a separate migration across schema, editor,
browser audition, bridge, direct CV, mute state and fixtures.

## An Otis flavor: a kit sampler and a capture companion

The pair is built, in DSP_Emulation's OtisLab plugin: **Otis Kit** plays a bank
and **Otis Capture** writes new generations into it
([KIT.md][otis-kit], [CAPTURE.md][otis-capture]). As this section asked, they
reuse OTIS's playback arithmetic rather than instantiating complete OTIS engines
with their own converters, recorders and sample memories: Otis Kit's eight slots
are regions of one sample memory, played on one chip's voices through one
output converter.

The bank is small and bounded: eight slots, one voice each. A hit selects a
slot and latches its take, its PITCH CV and its LEVEL CV. The slot's knobs stay
live instead of being captured by the hit: TUNE, DECAY, LEVEL, FILTER and START
act on a sounding hit as they move, and REV is read when playback starts. Hits
are one-shots; a falling gate does not stop them. Tempo-relative repeats
through the shared event scheduler are still to come. The relevant sound is an
EPS-era playback character with new drum behavior; a 12-bit setting alone would
not reproduce UW capture or playback.

Capture should record an external input, the drum mix, or an effects return for a
specified beat length. The destination is an explicit bank slot. Publish a new
immutable sample generation only after the take completes; existing voices keep
the generation they started with until they end. Bound memory and use an
off-audio-thread retirement path. Never resize/free sample vectors, block on
sample copying, or destroy a live generation in the audio callback.

Begin with one-shot playback and record-then-play. Simultaneous read/write of a
live circular recording buffer is a later, different feature with specified
read/write distance and feedback limits. Reworking a bank slot likewise needs
generation semantics, not mutation underneath a sounding hit.

Required sampler checks include slot isolation, old-tail/new-generation overlap,
end/reverse boundaries, amplitude envelopes, recorded sample timing, save/reload,
memory limits, stereo pairs and the existing OTIS arithmetic regression tests.

## Where Strudel ideas help

Strudel represents patterns as queries over a cycle interval, with fractional
event times. Its time transforms include `fast`, `slow`, rotation/offset and
Euclidean structure. Those ideas fit *above* the common event representation;
the synth and sampler do not need to know whether a hit came from the grid or a
generator. [Pattern model][strudel-patterns] · [Time transforms][strudel-time].

Parameter locks do not require Strudel integration. Implement kit defaults and
event snapshots first. Then add a small Python pattern layer that queries
half-open intervals and emits the same validated events. Start with sequence,
Euclidean structure, cycle alternation, fast/slow and rotation, with repeatable
probability only after the seed/cycle rules exist.

Generated patterns should be inspectable and bakeable into the tracker. Test
that querying a whole cycle equals concatenating adjacent partial queries,
including triplets, wraps and seeks. There must be no duplicate boundary hits,
extra wall clock, unbounded stream consumption or dependency on query order.
Direct Strudel-code import is a separate adapter with its own compatibility and
license review; an original subset of pattern concepts is not Strudel support.

## Order of work

1. Play the independent percussion prototype through current Gorgon and assess
   the sounds and controls on the workstation.
2. Define and implement kits plus a small set of true per-hit sound locks,
   including unlocked-note restoration. Prove the grid-to-Rack path first.
3. Build Otis Kit one-shot playback, then the capture companion and generation
   handoff. Use the same resolved hit parameters throughout.
4. Add retriggers/microtiming and the pattern query layer to that event path.
5. Expand track count and performance tools; add Control All/reload, sends and
   richer modulation without making basic note entry harder.

This order can change after auditioning the first instrument. The prototype
establishes one usable destination; it does not establish sonic equivalence to
Elektron's machines or complete the sampler/sequencer work.

[md-manual]: https://www.bhphotovideo.com/lit_files/85384.pdf
[drumlab]: https://github.com/JosiahAlanLee/DSP_Emulation/blob/main/vcv/drumlab/docs/MANUAL.md
[otis-kit]: https://github.com/JosiahAlanLee/DSP_Emulation/blob/main/vcv/otislab/docs/KIT.md
[otis-capture]: https://github.com/JosiahAlanLee/DSP_Emulation/blob/main/vcv/otislab/docs/CAPTURE.md
[strudel-patterns]: https://strudel.cc/technical-manual/patterns/
[strudel-time]: https://strudel.cc/learn/time-modifiers/
