# Per-hit sound controls (score version 2)

`hydrapy-tracker-2` adds track sound defaults and per-hit overrides to the existing
four-lane score. Version 1 stays valid and its clock, gates, pitches, velocities,
visual automation and scenes are unchanged. Writers use version 2 whenever they
write `sounds` or a cell's `locks`; version-1-only readers must reject version 2.
The browser audition voices do not implement these external-instrument controls.

## Data and resolution

Version 2 requires `sounds`, an array of four objects, one per audio lane:

```json
{"machine": null, "params": {"p1": 64, "p3": 100}}
```

Each object has exactly `machine` and `params`. `machine` is null (use the
instrument's selected machine) or an integer 0–191. `params` contains zero to
eight keys, `p1`–`p8`, each an integer 0–127. These are raw external-instrument
parameters, not MIDI notes or normalized visual controls. A Machinedrum backend
interprets them as SYN1–SYN8 and the OS's machine IDs. Unsupported machine IDs
must be reported by that backend rather than silently selecting another sound.

A non-null audio cell may additionally contain `locks`, an object with any of
`p1`–`p8` and `machine`, with the same integer ranges. A lock's machine cannot
be null. Empty locks are allowed. Unknown keys, booleans, non-integral numbers
and non-finite values are rejected in these objects.

For each sounding hit, start afresh from the lane's `sounds` entry and overlay
that cell's locks. Parameters absent from both remain **unspecified**, meaning
the instrument's native defaults for the resolved machine. Do not substitute
zero, carry a previous hit's locks forward, or modify the saved track defaults.
Resolve the machine before selecting its native parameter defaults. A null
machine resolves to the instrument's selected machine on that hit.

Example: track `p1=64`, row 0 lock `p1=20`, row 4 unlocked → 20 on row 0 and
64 on row 4. If the track has no p1 default, row 4 uses the instrument's native
p1 default. This is distinct from a held automation lane.

Rests, velocity zero and muted lanes do not trigger, restore or choke voices.
Deleting a lock removes only that override. Editing note/velocity, copying,
transposing, saving, importing and undo/redo must preserve locks. Editing kit
defaults or cells between triggers affects subsequent hits, not captured voices.
This revision has no triggerless locks, slides, sub-row events or pattern language.
Pitch remains the existing held 1 V/oct signal; it is not implicitly mapped onto
a machine-specific raw parameter such as SYN1.

## Rack sound link

The Gorgon Rack module exposes a receiver-owned, double-buffered expander link
to an adjacent compatible sound instrument. The public C++ layout is
[`sound-link.hpp`](sound-link.hpp), licensed BSD-3-Clause. It is independent of
the sound engine's license and includes no firmware or DSP code.

The compatible receivers are plugin `MachinemoduleRack`, model `Machinemodule`
(the Machinedrum), and plugin `MonomoduleRack`, model `Monomodule` (the
Monomachine), version 2.x of each. The receiver supplies two initialized `Packet`
buffers on its `leftExpander`. The Gorgon producer writes a whole packet into
the receiver's `producerMessage` and requests that receiver's message flip.
The receiver reads only its `consumerMessage`. All gates, pitches, velocities,
resolved controls and transport metadata therefore cross the boundary together.

Every packet includes a magic, ABI version, byte size, producer Rack sample frame,
transport revision, moving flag, reset voltage and its lanes. Each lane has gate
voltage, normalized velocity, held pitch, resolved machine (-1 = unspecified), an
eight-bit parameter-presence mask and eight raw values. The pitch is in volts at
1 V/oct with C4 = 0 V, whatever pitch reference the sender's own V/oct outputs
use, so a pitched receiver recovers the note as `60 + 12 × pitch` (the score's
note after the key's transposition). Masked-off values have
no meaning. Version-1 scores publish unspecified machine/parameters, retaining
their existing note behavior. Never infer a hit from a parameter change alone.

**Version 2** of the link has six lanes, one per lane of a `hydrapy-tracker-3`
score; a version-1 or -2 score leaves lanes 5 and 6 at rest (gate, velocity and
pitch 0, nothing specified). **Version 1** has four. The receiver's buffers
declare the version it reads, in their header (`Header`: magic, version and byte
size), and a sender writes a packet of that version or none. The Gorgon module
serves both; Machinemodule 2.2.0 reads version 2. [`sound-link.hpp`](sound-link.hpp)
keeps version 1's layout as `gorgon_sound::v1` for that. A sound in a version-3
score is the lane's kit sound with the playing candidate's locks over it, as
for version 2, held while the candidate's span lasts.

Validate the provider/receiver identities before reading/writing a packet, then
its header and finite values. Accept only frames no more than two samples old;
initial, stale, wrong-version and disconnected packets are inactive. Buffer
exchange adds an engine-sample delay. Consumers must use the linked gates for
linked locks, rather than pairing a delayed parameter packet with a separate
undelayed gate cable. A receiver can explicitly choose its external TRIG jack
instead, in which case it uses its own controls/CV and ignores the sound link.

Consumers resolve unspecified controls from their native defaults, overlay the
packet values, then add documented local knob/CV offsets and clamp to the
instrument's range. Trigger and the resulting complete control snapshot must
enter its renderer as one event. Any worker/render buffering must have bounded
queues, declared latency and overflow reporting; missed events must not sound
late. Engine-specific block quantization must be documented and tested.

This link does not alter the existing ByteBus jack or direct hardware CV maps.
Direct CV continues to play notes and visual lanes; raw sound locks in this
first implementation require a compatible Rack receiver. The editor must label
that scope. Per-hit raw-CV output jacks can be specified separately later.
