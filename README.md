# Gorgon

A CV and audio/video tracker, in development. One score, four 64-row patterns
of notes and automation, drives a picture, a modular synthesizer and VCV Rack
from a single playhead. Gorgon is the master clock; nothing follows an external
one.

This repository is the centre of the project: the **contract** every piece
implements, the **VCV Rack module**, and the **integration tests** that hold
the pieces together. The tracker itself runs inside
[HydraPy](https://github.com/JosiahAlanLee/hydrapy) Studio.

## The pieces

| Piece | Where | What it does |
| --- | --- | --- |
| Tracker and visuals | [hydrapy](https://github.com/JosiahAlanLee/hydrapy) — `hydrapy studio --tracker` | The score editor, the playhead, automation of a patch's controls, named scenes chosen per row, Program and Auxiliary outputs |
| Direct CV | hydrapy — `--cv-device`, `--cv-profile` | Clock, reset, run, gates, 1 V/oct pitch, velocity and automation as control voltage: a DC-coupled interface such as an Expert Sleepers ES-3, or an ES-40 with ESX-8GT/ESX-8CV expanders over encoded S/PDIF; PortAudio or native JACK/PipeWire |
| Rack module | [`rack/`](rack) here — plugin `GorgonBridge` | Follows Studio's playhead on Rack's sample clock and puts the score on jacks; CV patched in goes back to the patch's controls and transport; captures Studio's audio ports |
| Audio | hydrapy — Output routing | An optical soundtrack read from the picture and a synthesized breaks kit, as JACK/PipeWire ports |
| Contract | [`spec/`](spec) here | The score format, the bridge API, the signal semantics and the audio ports, with a conformance fixture |

![Gorgon module panel](rack/docs/panel-preview.png)

## Start

1. Install HydraPy and start Studio with the tracker:
   `hydrapy studio patches/gorgon_corridor.py --tracker`
   ([tracker guide](https://github.com/JosiahAlanLee/hydrapy/blob/main/docs/gorgon-tracker.md)).
2. For hardware CV, add `--cv-device "<interface>" --cv-map 8`, or a profile for
   ES-40 expanders ([ES-40 setup](https://github.com/JosiahAlanLee/hydrapy/blob/main/docs/es40.md)).
3. For Rack, install the `GorgonBridge` plugin from this repository's
   [releases](https://github.com/JosiahAlanLee/Gorgon/releases) or build
   [`rack/`](rack) against the Rack SDK, then add **Gorgon** to a patch. It
   finds Studio by itself ([manual](rack/docs/MANUAL.md)).
4. For audio into Rack, enable the JACK/PipeWire ports in Studio's Output routing
   and select them as Gorgon's capture device
   ([audio guide](https://github.com/JosiahAlanLee/hydrapy/blob/main/docs/gorgon-audio.md)).

## Status

Everything above exists and is tested in CI: HydraPy's suite (including
Chromium, and real JACK and PipeWire daemons), and here the Rack module's
engine against HydraPy's own outputs, the bridge against a real Studio,
sanitizers, Rack's API, and Windows/macOS builds.

The same Linux checks also pass on the workstation (2026-10-25: Ubuntu 22.04,
Rack SDK 2.6.6, HydraPy at the pin). The fixture is byte-identical, the follower
stays within 0.05 ms of a real Studio, and the schema check accepts all 202 scores.
The whole chain also runs in VCV Rack Free 2.6.6 on private JACK1 and PipeWire 1.0.7
servers at 48 kHz/256. Studio drives Gorgon and Gorgon plays a Vastish voice on every
gate. The clock holds 138 BPM to within 0.01 ms. The four captured channels match
Studio's ports sample for sample, to within float rounding, arriving 10.7 ms later
(16 ms in one JACK run).
An earlier headless run in Rack Pro showed only that the plugin loads: that copy of
Rack Pro was not signed in to a VCV account, and an unsigned Rack Pro processed no
audio in a later test.

What is **not** validated: physical interfaces on the workstation, the desktop's
own PipeWire session, a signed-in Rack Pro, the ES-40 digital path end to end,
display and device latency, and the Windows/macOS plugins inside Rack. The plugin
is not in the VCV Library yet.

## Working on it

Read [AGENTS.md](AGENTS.md) first: where a change goes, how the contract
changes, how the HydraPy pin moves. Short version: the contract in `spec/`
changes first, then HydraPy, then `rack/`.

## Lineage

The Rack module was developed in
[DSP_Emulation](https://github.com/JosiahAlanLee/DSP_Emulation) as `vcv/gorgon`
and moved here with its history on 2026-10-25. DSP_Emulation remains the home of
OTIS, Vastish and Spoon Bender, the instruments Gorgon's gates and pitch were
built to drive.

## License

BSD-3-Clause ([LICENSE](LICENSE)) for everything here, the Rack module included.
HydraPy is AGPL-3.0-or-later, following Hydra.
