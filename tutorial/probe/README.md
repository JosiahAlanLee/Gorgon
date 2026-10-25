# GorgonTutorialProbe

A VCV Rack 2 plugin with one module, **Director**, for filming Rack from outside.
It does nothing to audio. It exists so a program driving the real mouse (`gtut`)
knows exactly where every jack, knob and menu item is on screen.

Put a Director anywhere in a patch (the tutorial's generated patches park one far
right). The first one installs an agent in Rack's scene that outlives patch loads,
so one Rack session can be driven through many patches.

In a rack the Director draws nothing, not even its panel, so it never shows on camera;
its box is still there (and in `layout.json`). The module browser shows its panel.

## Files

The folder is `$GTUT_PROBE_DIR`, or `<Rack user folder>/gtut-probe`.

**`layout.json`**, rewritten five times a second (atomically):

```json
{
  "time": 1234.5, "frame": 5120, "seq": 42, "fps": 59.8, "patch": "/…/rack-voice.vcv",
  "window": {"pos": [0, 1080], "size": [1920, 1080], "pixelRatio": 1, "windowRatio": 1},
  "zoom": 1.6, "viewAnimating": false,
  "modules": [{"id": 1, "plugin": "GorgonBridge", "model": "Gorgon", "name": "Gorgon",
               "box": [x, y, w, h], "grid": [x, y],
               "ports": [{"type": "output", "index": 0, "name": "Clock", "centre": [x, y], "cables": 1}],
               "params": [{"index": 2, "name": "Timing offset", "value": 0, "min": -100, "max": 100,
                           "display": "0", "box": [x, y, w, h], "centre": [x, y]}]}],
  "menu": [{"text": "CV 1 drives", "rightText": "▸", "disabled": false, "box": [], "centre": []}],
  "log": ["loaded /…/rack-voice.vcv"]
}
```

All coordinates are X screen pixels (window position plus Rack's own coordinates
scaled by pixelRatio / windowRatio). `menu` lists the items of every open context
menu, submenus included. `seq` is the last command run.

**`commands.jsonl`**: append one JSON object per line. Unknown ops are logged.

| op | fields | effect |
|---|---|---|
| `view` | `modules` (ids or slugs) or `module`, `zoom`, `margin`, `seconds` | animate zoom and scroll to fit those modules (all but the Director if none) |
| `highlight` | `module`, `type` (input/output/param, or none for the module), `index`, `label`, `seconds` | a pulsing amber ring and a caption |
| `clear` | | remove highlights |
| `param` | `module`, `index`, `value` or `scaled` (0–1 of range), `seconds` | ramp a param smoothly (exact and repeatable) |
| `flash` | `seconds` | white over the whole window: the sync slate |
| `load` / `save` | `path` | load or save a patch (runs between frames, from the agent, so the Director being replaced is safe) |
| `catalog` | `path` | every installed model's params (name, range, default), inputs, outputs and panel width in HP |
| `interval` | `seconds` | layout write interval |
| `dump` | | write the layout now |

Add `"seq": n` to any command; the next layout reports it, so a client can wait
for its command to have run.

## Build

```sh
make RACK_DIR=/path/to/Rack-SDK dist     # dist/GorgonTutorialProbe-2.0.0-lin-x64.vcvplugin
```

`gtut setup plugins` builds and installs it into the stage's Rack user folder
only. Don't install it into the Rack you play with.
