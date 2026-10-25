# Gorgon manual

Gorgon connects VCV Rack to the Gorgon tracker in [HydraPy](https://github.com/JosiahAlanLee/hydrapy) Studio. Gorgon is the master clock: Rack follows its tempo, position and score, and CV patched into the module goes back into the visuals.

![Gorgon panel artwork](panel-preview.png)

## Setup

1. Start Studio on the same computer: `hydrapy studio patches/gorgon_corridor.py --tracker`. On start Studio writes its port and session token to `~/.hydrapy/studio.json`, readable only by you, and removes it on exit.
2. Add Gorgon to a Rack patch. The display shows **Linked** and the tempo once it finds Studio, usually within a second. If Studio restarts, Gorgon finds it again.

There is nothing to configure. To point Gorgon at a different file, set `HYDRAPY_STUDIO_FILE` before starting Rack. Studio only listens on 127.0.0.1, so Rack and Studio must run on the same machine.

## Outputs: from Gorgon

| Jack | Signal |
|---|---|
| CLOCK | 10 V pulses, 50% duty. Sixteenths by default; *Clock* in the context menu sets 1, 2, 4, 8 or 24 per quarter note |
| RESET | 5 ms trigger on start, resume or seek, and at the score/pattern loop boundary. Transport pulses are latched even when an update arrives late |
| RUN | 10 V while Gorgon plays |
| VEL | Polyphonic, 4 channels: each audio lane's last hit velocity, 0–10 V, held until the next hit |
| V/OCT | Polyphonic, 4 channels: held note pitch, 1 V/oct. C4 = 0 V by default; *Pitch zero* in the context menu also offers C3 = 0 V |
| GATE 1–4 | 10 V for half a row on each hit in that audio lane; muted lanes stay low. *Gate length* sets 25–100% of a row |
| GATES | The four gates as one polyphonic cable |
| Z | Dolly Z lane, 0–1 as 0–10 V |
| ROLL | Roll lane, ±45° as ±5 V |
| FDBK | Feedback lane, 0–0.9 as 0–10 V |

When Gorgon is paused or its automation is off, clock, reset, run and gates stop. The lanes hold their current value.

The pitch channels line up with VEL and GATES: use a polyphonic voice, or split the
four channels for separate voices. Notes use C0–G9 (sharps allowed); `C-4` means
C4. The key selector transposes by the chosen key minus D, matching the browser
and direct CV. AIR opens a gate and updates velocity while keeping the previous
pitch. Velocity zero opens no gate and preserves both held values. Muting a lane
also keeps its held values. Start/resume/seek clear held values before the next hit.

These follow Studio's `--cv-device` signals. Gates, clock, run and unipolar lanes
use 10 V instead of 5 V; pitch remains 1 V/oct and roll remains ±5 V.

## Inputs: to Gorgon

**CV 1–4** set the patch's named controls. 0–10 V spans the control's range: for a control from −2 to 2, 5 V gives 0. A control follows its CV only while a cable is patched.

By default each input takes the next control not already driven by a tracker lane, in the order the patch declares them. Choose a specific control, or none, under *CV n drives* in the context menu. Choices are saved with the Rack patch and use control names, so they survive editing the HydraPy patch.

A control bound to a tracker lane in Studio is driven by the score while automation is on, and CV for it is ignored until automation is off.

## Transport

- **PLAY** (button or trigger) starts Gorgon if it is stopped, and pauses it if it is playing.
- **RESET** (button or trigger) moves Gorgon to the start of the score, or of the looped pattern.

Both act on Studio, so the picture and every other Gorgon output move too.

## Timing

Studio's playhead reaches the module about 30 times a second. Between updates the module runs its own beat at the score's tempo on Rack's sample clock, and trims that tempo by at most 0.5% to stay aligned with Studio. Clock pulses, gates and held automation steps therefore land on exact samples and stay evenly spaced. Seeks, tempo changes, starting and stopping are followed as soon as the next update arrives, within about 35 ms.

The end-to-end test (`tests/studio_e2e.py`) reports follower phase error against a running Studio and requires less than 1 ms in that harness. It does not measure physical audio or display latency.

Transport changes carry a separate revision from score edits. The module emits
a complete reset pulse on receiving a seek, even when the playhead is already
past its first 5 ms. Editing a note, changing a control or receiving another
cached snapshot does not retrigger reset. The browser, direct CV and bridge use
the same pre-render timestamp, so rendering time is not added to one follower's
clock estimate. This does not compensate physical display or device latency.

**OFFSET** (±100 ms) moves Rack's outputs earlier (clockwise) or later than the picture, to make up for audio and video latency. To set it, patch GATE 1 into a percussive voice, give lane 1 a hit on every beat, and turn OFFSET until each hit lines up with a visible flash in Gorgon.

## Display and lights

The display shows the link state and tempo, then the pattern, row and beat, then what each CV input drives. **LINK** is lit while Studio answers. **CLK** follows the clock output. PLAY's light is lit while Gorgon plays.

If Studio stops answering, the module keeps the last tempo and position and carries on until Studio is back.
