# Rack ByteBus output

Contract v1. Written against HydraPy `67d08975a17b203652ecb53bb777e80446626816`
and DSP_Emulation `4587b48293929448419eff428126d6f8008b7872`. The hit mask for
version-3 documents ([below](#version-3)) is part of
[bus-io.md](bus-io.md) (A-spec-4) and implemented in the Rack module
`GorgonBridge` 2.4.0.

This is a Rack-side encoding of the existing automation lanes and hit state.
It adds no score fields, bridge endpoints, Python signals, or direct hardware
CV mappings. HydraPy, `hydrapy.pin`, and the existing CV fixture stay unchanged.
The Rack implementation follows this document. The receiving modules follow
DSP_Emulation's [ByteBus specification](https://github.com/JosiahAlanLee/DSP_Emulation/blob/4587b48293929448419eff428126d6f8008b7872/docs/BYTE_BUS_SPEC.md).

## Cable

BUS OUT always emits exactly nine polyphonic channels:

| Rack channel | Meaning |
| --- | --- |
| 1–8 | Data bits 0–7, least significant bit first |
| 9 | Word publication clock |

Every line is exactly 0 or 10 V, including when the module's ordinary CV
profile is 5 V. Data and clock are written in the same engine sample. All
existing output IDs remain unchanged; BUS OUT is appended as output ID 17.
One byte is one word, not eight pitched voices.

## Byte source

A patch-saved **BUS OUT source** option selects one source. Default: **Z**.
For an automation lane, normalize its value to `u` using the ranges below,
clamp `u` to 0–1, then compute `floor(255*u + 0.5)`. A non-finite lane value
maps to 00. The conversion uses lane units, independent of output voltage.

| Source | Normalized value / word |
| --- | --- |
| Z | `u = z` (0–1) |
| Roll | `u = (roll + 45) / 90` (−45° to +45°) |
| Feedback | `u = feedback / 0.9` (0–0.9) |
| Hit mask | Bit `n−1` is 1 if lane `n` has a present, unmuted hit in the current cell; bits 4–7 are zero |

Hit mask uses the cell's hit state, independent of the gate pulse's duty
cycle. AIR with positive velocity counts; zero velocity and muted lanes do
not. A silent cell publishes 00. Thus changing Gate length does not change
the mask, even when BUS is sampled after a gate has closed.

Examples: Z = 0 → 00; Z = 0.5 → 80; Z = 1 → FF. Roll = 0° → 80.
Feedback = 0.9 → FF. Hits in lanes 1 and 4 → 09 (channels 1 and 4 high).

### Version 3

For a `hydrapy-tracker-3` document the hit-mask source publishes the same word
layout that BUS IN reads ([bus-io.md](bus-io.md#one-word-layout)):

| Bit | Meaning |
| --- | --- |
| 0–5 | Lane `n` on bit `n − 1`: a candidate of lane `n` that fires covers the sampled tick, `at ≤ u < at + duration` ([evaluator.md](evaluator.md#evaluate)) |
| 6 | FILL as conditions see it on the row containing the sampled tick: Studio's or the bus layer's, at that row's start |
| 7 | 0, reserved for "entry start" |

A candidate that doesn't fire publishes 0 for its whole span, including one
muted at its `at` by the score or the bus layer; a mute that arrives during the
span doesn't clear the bit. A `ply` sub-step counts for its own span. Bit 6
changes only on row starts, the ticks at which FILL is read. As for versions 1 and 2, the mask follows the
candidate's span, not the gate fraction. Version 1 and 2 documents publish as
in the table above, so the fixture and the existing tests are unchanged. The
menu names the source "Hit mask (lanes 1–6, FILL)" for a version-3 document.

## Acquisition and publication

The module samples the selected source and emits a one-engine-sample high
on channel 9 at each rising edge of its CLOCK output. The Clock/PPQN option
and OFFSET therefore affect both. The byte holds between publications,
including when the selected lane uses continuous interpolation. Repeated
bytes still publish new clock pulses.

Start, resume, or a changed `transport_revision` while moving also requests
publication of the source at the current playhead position. This delivers a
seek even if the first update arrives after CLOCK's rising edge. It is one
publication, not a second pulse when a clock edge happens simultaneously.
An ordinary score revision or repeated transport snapshot does not request
publication. Source selection alone does not request publication; the next
clock/transport publication acquires the newly selected source.

Every high strobe is followed by at least one low sample. If a transport
request arrives immediately after a strobe, that sample goes low and the
request publishes on the following sample, sampling the source then. Further
transport requests during the gap coalesce to the latest position. Ordinary
clock edges during that gap are discarded rather than queued.

When stopped, disabled, or not yet linked, channel 9 is low and the byte
holds. Stopping cancels a pending transport publication. There is no internal
clock fallback. A new instance or patch reload starts at byte 00, with no
publication until it is moving; the held byte itself is not patch-saved.
If Studio stops answering after playback has begun, the existing follower
continues, so BUS continues with the other score outputs.

All acquisition, latching, and line generation happen on Rack's audio thread
without allocation or locks. Rack adds its normal one-sample cable delay per
module hop. This connector does not establish physical clock transport or
audio/display latency.

## Conformance

Portable tests cover all 256 automation codes for each lane, all 16 hit
masks, canonical levels, repeated words, edge timing at 44.1/48/96 kHz,
transport publication, mandatory low gaps, stopped-clock hold, source changes,
and non-finite inputs. Rack API tests cover the actual nine-channel port,
5 V profile independence, JSON options, and compatibility of old output IDs.
The existing score/CV fixture must remain byte-identical.
