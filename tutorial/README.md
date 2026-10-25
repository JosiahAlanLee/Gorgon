# The tutorial kit

Everything needed to film the Gorgon / HydraPy / VCV Rack modules tutorial on one
Linux machine, unattended, with Claude Code doing the filming: a stage (a private
X display, JACK server, Studio, Chromium, Rack and a terminal), a script of shots,
and the tools that record, narrate, edit and check them.

Not part of the Gorgon plugin or the contract. Nothing here is shipped to Rack users.

## Start

On the workstation, in this folder:

```sh
./bootstrap.sh          # writes config.yaml the first time; edit its paths and run it again
source work/venv/bin/activate
gtut preflight
```

Then start Claude Code here and give it the brief:

> Read CLAUDE.md and produce the tutorial. Ask me before anything that needs sudo,
> and before choosing the narration voice.

## Layout

| Path | What |
|---|---|
| `CLAUDE.md` | The brief for Claude Code: rules, phases, where to look when something fails |
| `config.example.yaml` | Every setting, with defaults; copy to `config.yaml` |
| `gtut/` | The harness. `gtut --help` lists the commands |
| `probe/` | `GorgonTutorialProbe`: a Rack module that reports where everything is on screen and draws highlights |
| `script/` | The shots, one YAML file per chapter, and `STYLE.md` for the narration |
| `patches/` | Rack patch builders (by port name, from the catalog) and harvested GUI state |
| `diagrams/` | SVG stills used by `still` shots |
| `cards/` | A HydraPy patch rendered behind title cards |
| `work/` | Everything generated (gitignored): stage state, takes, narration, segments, chapters, QC |

## How a shot becomes video

1. `gtut narrate` speaks the shot's narration a sentence at a time and finds when
   each `{cue}` falls.
2. `gtut shoot` brings up what the shot needs, sets the starting state, records
   the filmed windows and the programme sound, fires a slate, and runs the
   actions on their cues with a visible cursor.
3. `gtut compose` aligns sound to the slate, cuts to the narration, frames
   (crop and zoom keyframes), draws overlays and mixes the voice over the
   programme.
4. `gtut chapter` / `gtut final` join, normalise loudness, write captions and
   chapter markers, and run QC (loudness, black frames, freezes, OCR for private
   text, contact sheets).

## Tests

```sh
python -m pytest tests -q
```

The unit tests cover the patch builder, reference resolution, script validation
and compose's time mapping; they need neither Rack nor a display.
