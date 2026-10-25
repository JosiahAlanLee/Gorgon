# Audio ports

Informative. Studio can publish four listening-audio ports on JACK or
PipeWire's JACK layer; the Rack module captures them through Rack's audio
driver and puts them on jacks. The HTTP bridge never carries PCM.

| Studio port | Content | Rack capture channel | Rack jack |
| --- | --- | --- | --- |
| `optical_L` | first `Soundtrack` probe in the patch | 1 | OPT L |
| `optical_R` | second `Soundtrack` probe (the first duplicated if there is only one; silence if none) | 2 | OPT R |
| `breaks_L` | synthesized Breaks kit from audio lane 1 | 3 | BRK L |
| `breaks_R` | same mono signal | 4 | BRK R |

- JACK client name `HydraPy`; the server may suffix it when two clients share a
  name, so read the real names from Studio's Output routing panel.
- Float PCM in −1…1. Rack maps that to −10…10 V. Studio's default optical gain
  is 0.5 and Breaks gain 0.4, both 0–1 and saved per workspace in
  `.hydrapy/outputs.json`.
- Breaks: `C-2` is a kick, `D-2` a snare, any other note a hat; velocity and
  mute follow the score; lane 1 only. It is a deterministic synth, not sample
  playback.
- Both pairs are silent while Studio is paused. Breaks also stops when
  automation is disabled; Optical follows the picture, so it continues.
- Studio prepares audio a few periods ahead (`lead_blocks`, 2–8); the Rack
  module's **OFFSET** control does not move received PCM.

The encoded ES-40 S/PDIF stream is not audio and must never share these ports,
a mixer, a gain or a resampler. See HydraPy's
[es40.md](https://github.com/JosiahAlanLee/hydrapy/blob/main/docs/es40.md) and
[gorgon-audio.md](https://github.com/JosiahAlanLee/hydrapy/blob/main/docs/gorgon-audio.md).
