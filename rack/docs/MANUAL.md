# Gorgon manual

Gorgon connects VCV Rack to the Gorgon tracker in [HydraPy](https://github.com/JosiahAlanLee/hydrapy) Studio. Gorgon is the master clock: Rack follows its tempo, position and score, and CV patched into the module goes back into the visuals.

![Gorgon panel artwork](panel-preview.png)

## Setup

1. Start Studio on the same computer: `hydrapy studio patches/gorgon_corridor.py --tracker`. On start Studio writes its port and session token to `~/.hydrapy/studio.json`, readable only by you, and removes it on exit.
2. Add Gorgon to a Rack patch. The display shows **Linked** and the tempo once it finds Studio, usually within a second. If Studio restarts, Gorgon finds it again.

Gorgon is 14 HP wide; it was 12 HP before 2.4.0. In a patch saved with an older
Gorgon, check the module to its right: the Machinemodule sound link needs it
flush against Gorgon's right edge.

The score/CV bridge discovers Studio automatically. To point it at a different file, set `HYDRAPY_STUDIO_FILE` before starting Rack. Studio only listens on 127.0.0.1, so Rack and Studio must run on the same machine. Audio capture has a separate setup below.

## Outputs: from Gorgon

| Jack | Signal |
|---|---|
| CLOCK | 10 V pulses, 50% duty. Sixteenths by default; *Clock* in the context menu sets 1, 2, 4, 8 or 24 per quarter note |
| RESET | 5 ms trigger on start, resume or seek, and at the score/pattern loop boundary. Transport pulses are latched even when an update arrives late |
| RUN | 10 V while Gorgon plays |
| VEL | Polyphonic, one channel per audio lane (6 for a version-3 score, 4 for versions 1 and 2): the lane's last hit velocity, 0–10 V, held until the next hit |
| V/OCT | Polyphonic, one channel per audio lane: held note pitch, 1 V/oct. C4 = 0 V by default; *Pitch zero* in the context menu also offers C3 = 0 V |
| GATE 1–6 | 10 V for half a row on each hit in that audio lane; muted lanes stay low. *Gate length* sets 25–100% of a row. A start or seek that arrives after its hit's gate would have closed still plays that hit, late; a seek retriggers an open gate. GATE 5 and 6 play only version-3 scores |
| GATES | The gates as one polyphonic cable, one channel per audio lane |
| Z | Dolly Z lane, 0–1 as 0–10 V |
| ROLL | Roll lane, ±45° as ±5 V |
| FDBK | Feedback lane, 0–0.9 as 0–10 V |
| OPT L / OPT R | Optical Soundtrack audio from capture channels 1/2, PCM −1…1 as −10…10 V |
| BRK L / BRK R | Synthesized Breaks kit from capture channels 3/4, PCM −1…1 as −10…10 V |
| BUS OUT | One clocked byte: channels 1–8 are bits 0–7, channel 9 is the publication clock; all lines exactly 0/10 V |

When Gorgon is paused or its automation is off, clock, reset, run and gates stop. The lanes hold their current value.

The pitch channels line up with VEL and GATES: use a polyphonic voice, or split the
channels for separate voices. Notes use C0–G9 (sharps allowed); `C-4` means
C4. The key selector transposes by the chosen key minus D, matching the browser
and direct CV. AIR opens a gate and updates velocity while keeping the previous
pitch. Velocity zero opens no gate and preserves both held values. Muting a lane
also keeps its held values. Start/resume/seek clear held values before the next hit.

**CV output levels** in the context menu selects 10 V (existing Rack default)
or 5 V (matching Studio's direct CV). It scales clock, reset, run, gates, held
velocity, Z and feedback together. Pitch remains 1 V/oct, roll remains ±5 V,
and the audio jacks retain their separate scaling. BUS OUT always uses 0/10 V.
The setting is saved with
the Rack patch. Existing port IDs are preserved; the four audio outputs are
appended after V/OCT, BUS OUT after the audio outputs, GATE 5 and 6 after BUS
OUT, and BUS IN after CV 4.

## Version-3 scores

A `hydrapy-tracker-3` score (HydraPy's tracker editor saves one as soon as an
edit needs it) has six audio lanes, 128 patterns in banks A–H, a chain, and
per-pattern rate, bar and length. Gorgon plays it the way Studio's direct CV
does: the same candidates from the same evaluator, so conditions, `ply`,
`degrade`, `rot`, `rev` and `every` sound the same in Rack as in Studio.

- Six lanes: GATE 1–6, and six channels on GATES, VEL and V/OCT.
- Each `ply` sub-step has its own gate, a gate-length fraction of its own span.
- A note whose condition fails, or that a transform drops, opens no gate and
  leaves the lane's held velocity and pitch alone, as a mute does.
- FILL (Studio's FILL key, or a module's bus layer) and the lane mutes are
  judged once per hit, when it starts: a mute that arrives while a note sounds
  doesn't cut it.
- CLOCK and RESET count from the start of the chain, or of the looped pattern.
  RESET sends the playhead there.
- The display shows the playing pattern's code and its bar and row, as the
  editor does: `A01 002:03` is pattern A01, bar 2, row 3.

## BUS IN

**BUS IN** mutes lanes and turns on FILL from a byte bus, for version-3 scores
([bus-io.md](../../spec/bus-io.md)). High is on:

| Bit (channel) | Default |
| --- | --- |
| 0–5 (1–6) | Mute lanes 1–6 |
| 6 (7) | FILL |
| 7 (8) | Unused |

This is the layout BUS OUT's hit mask publishes, so a word that goes out, through
logic and back in means the same lanes both ways. *BUS IN bits* in the context
menu moves any of the seven to another bit, or to none, and shows the word as it
arrives. The map is saved with the patch.

- The bus adds to the score: a lane muted in the editor stays muted, and FILL
  from the editor stays on. An all-zero word, which a fresh Register or Counter
  sends, does nothing.
- Unclocked (8 channels or fewer), every sample is read. Clocked (9 or more,
  CLK on channel 9), the word changes on CLK's rising edges: a Register latched
  on Gorgon's CLOCK changes mutes on rows, a Counter's CARRY at modulus 16 on bars.
- A change counts from the first tick Gorgon hasn't played yet. A mute that
  arrives while a note sounds doesn't cut it; the lane's next hit is muted.
- A loop from BUS OUT through logic back into BUS IN acts on the next row: the
  word comes back at least two samples after the row it describes.
- Gorgon plays its own BUS IN at once and sends it to Studio, so the picture,
  direct CV and other Gorgon modules follow, about one bridge poll later. One
  module holds the bus at a time. If another module's BUS IN already holds it,
  the menu says so, and this one follows that bus until the other lets go (3
  seconds after it last sends).
- Unpatching BUS IN, removing the module or closing the patch clears every
  mute and FILL it set.
- A version-1 or -2 score ignores BUS IN.

## ByteBus

Patch **BUS OUT** directly to a ByteBusLab **Clocked Register**, **Lookup**,
Vastish **BUS IN**, or OTIS **FRAME BUS**. One polyphonic cable carries the
whole byte and its clock. Use VCV Free **Split** to inspect the eight bits and
the ninth clock channel individually. The included
[example patch](../examples/gorgon-bytebus.vcv) has a register, Split and DAC.

Right-click **BUS OUT source** to choose what it sends. The menu also shows the
held word in hexadecimal.

| Source | Mapping |
| --- | --- |
| Z (default) | 0–1 → 00–FF |
| Roll | −45° to +45° → 00–FF; 0° → 80 |
| Feedback | 0–0.9 → 00–FF |
| Hit mask (lanes 1–4) | Bits 0–3 indicate the current cell's unmuted hits; bits 4–7 stay zero |
| Hit mask (lanes 1–6, FILL) | For a version-3 score: bits 0–5 are lanes 1–6 with a hit that plays, bit 6 is FILL on the current row, bit 7 stays zero |

Automation values round to the nearest byte, half up, after clamping. To
program a particular word `n` from Z, set a held Z key to `n/255`. Hit mask
counts AIR, ignores muted/zero-velocity cells, and uses the hit's presence
rather than its gate pulse: Gate length does not change it. At 4 PPQN, it
publishes one rhythm word per sixteenth; other Clock settings sample the
current cell more or less often. For a version-3 score a lane's bit covers its
hit's whole span, each `ply` sub-step its own; a hit whose condition fails or that
is muted leaves it 0. FILL's bit changes only at row starts. BUS IN reads the
same layout, so a word sent round through logic means the same lanes coming
back ([bus-io.md](../../spec/bus-io.md#one-word-layout)).

BUS OUT samples and holds on CLOCK's rising edge, publishing a one-engine-sample
strobe on channel 9. It publishes every acquisition, even if the word repeats.
Data and clock travel together. Start/resume/seek also publish the current
source if the update arrives between clock edges. Every strobe has a low
sample after it; a seek arriving immediately after a strobe waits through
that low sample. Changing source waits for the next acquisition. Pause or
disabled automation holds the byte and stops publication, with no fallback
clock. A new/reloaded module starts at 00 until playback; source selection
is saved, the held word is not.

For **OTIS**, select FRM mode. The byte chooses a frame across its region;
pitch, gates and polyphony still come from separate inputs. For **Lookup**,
BUS OUT is the lookup address, not a family selector. Its table can remap the
sequence before another bus destination.

The [connector contract](../../spec/bytebus-output.md) defines levels and
transport details. BUS OUT is generated on Rack's sample clock. It adds no
Strudel dependency and uses existing HydraPy scores unchanged.

## JACK / PipeWire audio

1. Install HydraPy's `jack` extra, start the audio server, and enable **JACK /
   PipeWire audio** under the tracker's **Output routing**.
2. Right-click Gorgon, open **Audio device**, select the JACK driver, then open
   **Audio device** again and choose Studio's client (`HydraPy`) as the capture
   device, channels 1–4. With PipeWire, run both
   Studio and Rack through `pw-jack` when required by the distribution.
3. When Rack opens that device it connects Studio's `optical_L`, `optical_R`,
   `breaks_L`, `breaks_R` ports to capture channels 1–4 itself, in that order;
   check them in the patchbay. This happens again whenever the patch is loaded
   with Studio already running. If Studio restarts while Rack is running, the
   connections are not remade and OPT/BRK fall silent: connect the four ports
   to Rack's capture channels in the patchbay. With any other capture device,
   connect them by hand.
4. Connect the module's four audio jacks to a mixer/processor and an ordinary
   Rack Audio output module. Gorgon itself only captures audio.

Optical reads the patch's first two Soundtracks (one is duplicated, none is
silent). Breaks is the first tracker's synthesized kick/snare/hat kit, not
recorded break samples. Browser Sound remains an independent audition path.
Studio's software buffer lead adds latency; **OFFSET** changes the score/CV
timing, not received PCM. At 48 kHz with 256-frame periods and the default
lead, a Breaks hit reached BRK 16–22 ms after the GATE 1 pulse for the same
row (about 21.5 ms in four test runs of five). To line up a Rack voice played
from the gates with Breaks, set OFFSET to about −20 ms (counterclockwise,
later). Measure your own setup by patching GATE 1 and BRK L into a scope.
Audio conversion follows Rack's engine sample rate.
The bounded queue drops excess backlog rather than growing latency indefinitely;
disconnect/underrun produces silence. Counters are in the context menu.

Keep any ES-40 encoded S/PDIF route separate from this listening-audio path;
never resample/mix/gain it through these jacks. Studio's per-pair polarity
setting (Output routing) applies only when HydraPy's own encoder drives the
ES-40. When Rack's 8GT/8CV and ES-4(0) encoders feed the S/PDIF pair through an
Audio module, that setting has no effect on the stream. Diagnose that path with
`hydrapy es40-check`, whose WAV bypasses every encoder. Connect exactly one
source to the S/PDIF playback pair: JACK sums multiple connections to a port,
which corrupts encoded words. See HydraPy's
[audio guide](https://github.com/JosiahAlanLee/hydrapy/blob/main/docs/gorgon-audio.md)
and [ES-40 guide](https://github.com/JosiahAlanLee/hydrapy/blob/main/docs/es40.md).

## Inputs: to Gorgon

**CV 1–4** set the patch's named controls. 0–10 V spans the control's range: for a control from −2 to 2, 5 V gives 0. A control follows its CV only while a cable is patched.

By default each input takes the next control not already driven by a tracker lane, in the order the patch declares them. Choose a specific control, or none, under *CV n drives* in the context menu. Choices are saved with the Rack patch and use control names, so they survive editing the HydraPy patch.

A control bound to a tracker lane in Studio is driven by the score while automation is on, and CV for it is ignored until automation is off.

## Transport

- **PLAY** (button or trigger) starts Gorgon if it is stopped, and pauses it if it is playing.
- **RESET** (button or trigger) moves Gorgon to the start of the score, or of the looped pattern.

Both act on Studio, so the picture and every other Gorgon output move too.

## Timing

Studio's playhead reaches the module about 30 times a second. Between updates the module runs its own beat (for a version-3 score, its own ticks, 1920 a beat) at the score's tempo on Rack's sample clock, and trims that tempo by at most 0.5% to stay aligned with Studio. Clock pulses, gates and held automation steps therefore land on exact samples and stay evenly spaced. Seeks, tempo changes, starting and stopping are followed as soon as the next update arrives, within about 35 ms. A seek is followed exactly however short it is (even a fraction of a row): the module jumps on Studio's transport revision, and trims its tempo only for unmarked drift. The hit a start or seek lands on still plays when its update arrives after that hit's gate would have closed (with *Gate length* 50%, 54 ms at 138 BPM): its gates open on arrival for the length they would have had, closing 1 ms before the next row so its hit still triggers. A seek that arrives while a gate is open closes it for 1 ms first, so the hit it lands on triggers again rather than continuing the open gate. Without these, a late update skipped the hit, a cue made just after a hit could merge with it, and with OFFSET set earlier the first downbeat after Play could be lost.

The end-to-end test (`tests/studio_e2e.py`) reports follower phase error against a running Studio and requires less than 1 ms in that harness. It does not measure physical audio or display latency.

Transport changes carry a separate revision from score edits. The module emits
a complete reset pulse on receiving a seek, even when the playhead is already
past its first 5 ms. Editing a note, changing a control or receiving another
cached snapshot does not retrigger reset. The browser, direct CV and bridge use
the same pre-render timestamp, so rendering time is not added to one follower's
clock estimate. This does not compensate physical display or device latency.

**OFFSET** (±100 ms) moves Rack's outputs earlier (clockwise) or later than the picture, to make up for audio and video latency. To set it, patch GATE 1 into a percussive voice, give lane 1 a hit on every beat, and turn OFFSET until each hit lines up with a visible flash in Gorgon.

## Event trace

When a hit didn't sound, the trace says why. Gorgon keeps its last 4096 events:
every version-3 candidate it evaluates, and each change BUS IN makes. **Save
event trace…** in the context menu writes them as JSON lines, in the format
HydraPy writes with `--tracker-trace` ([evaluator.md](../../spec/evaluator.md#trace)).
Each candidate line has its id, tick, lane, note, velocity and locks, and
whether it fires. `why` names what decided: a mute (score or bus), a condition
with its value, or a `degrade` with its draw. `origin` gives the play, pattern,
row and transforms. Gorgon adds `x_frame`, the Rack engine frame it evaluated
the line on.

Compare it with Studio's trace from the same session: the same `id`, with the
same `transport_revision` and `start`, is the same event in both.

## Display and lights

The display shows the link state and tempo, then the pattern, row and beat (for a version-3 score, the pattern's code with bar and row), then what each CV input drives. **LINK** is lit while Studio answers. **CLK** follows the clock output. PLAY's light is lit while Gorgon plays.

If Studio stops answering, the module keeps the last tempo and position and carries on until Studio is back.


## Machinemodule sound link (Gorgon 2.3.0; six lanes from 2.4.0)

Place **Machinemodule** (`MachinemoduleRack` plugin) immediately to the right of
Gorgon, load your Machinedrum OS in its context menu, and leave its TRIG input
unpatched. Gorgon sends each lane's resolved machine/SYN1–8 controls together
with its gate, velocity, pitch and transport in one expander packet. The
receiver's blue light indicates a live link. No extra parameter cables are needed.

Machinemodule 2.2.0 and later take six lanes (sound link version 2): a
version-3 score plays tracks 1–6, and an older score tracks 1–4. Gorgon still
serves an earlier Machinemodule, with four lanes (version 1), but a Gorgon
before 2.4.0 can't link to Machinemodule 2.2.0: update Gorgon first.

In the matching HydraPy Audio editor, open **External Rack instrument**. Set
track defaults and per-hit locks as raw integers: machine 0–191, P1–P8 0–127.
Blank controls inherit. Each hit starts from fresh defaults, and absent values
remain unspecified until the receiving machine supplies its native defaults.
Editing these controls upgrades a score to `hydrapy-tracker-2`; older readers
reject it. Untouched version-1 scores continue to play with the instrument's
selected machine/defaults. Browser audition and physical direct CV keep their
existing behavior. Notes are still available on Gorgon's ordinary V/oct output;
they do not implicitly rewrite a machine's raw P1 control.

Machinemodule's local offsets and eight polyphonic parameter-CV inputs add to
these controls and latch at each hit. Patching its TRIG input overrides the link.
The first adapter buffers 1024 host samples plus rate-conversion delay and rounds
triggers forward to the native 32-sample boundary. Its menu reports delay,
missed blocks and unavailable machine IDs. Align other voices or adjust Gorgon's
timing offset for your monitoring path; no physical workstation latency has
been measured here. Set Machinemodule's native tempo manually to match Gorgon.

The Rack module contains only the public BSD sound-link ABI; firmware and the
AGPL emulation core live in the separate receiver plugin. See
[`../../spec/sound-locks.md`](../../spec/sound-locks.md) and
[`../../spec/sound-link.hpp`](../../spec/sound-link.hpp) for the contract.
