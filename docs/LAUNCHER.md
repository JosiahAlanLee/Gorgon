# One-command Linux sessions

After one-time installation and profile setup:

```sh
gorgon start
```

This opens the Gorgon tracker, HydraPy's native picture window and VCV Rack.
Studio and Rack run through `pw-jack`, sharing the desktop PipeWire server.
Startup waits for Studio's actual audio ports and Rack's playback connections;
a live process alone is not considered a ready audio session. Playback starts
paused; press Space in the tracker when ready.

## Install once

Use the Python environment in which you run HydraPy. From the Gorgon checkout:

```sh
python -m pip install -e .
```

HydraPy needs its `live` and `jack` extras for the native window and audio
ports (`python -m pip install -e '/path/to/hydrapy[live,jack]'`). Gorgon's
`hydrapy.pin` names the compatibility-tested Studio revision. Rack 2 Free and
the GorgonBridge plugin must also be installed. The launcher discovers `Rack`
on PATH and common `~/opt/Rack2Free/Rack` / `/opt/Rack2/Rack` locations.

Install your distribution's PipeWire JACK tools, including `pw-jack` and
`pw-cli`, and WirePlumber. `wmctrl` is optional for requesting a clean Rack
window close from `gorgon stop`. This is a user-session launcher: it runs no
sudo commands, installs no OS packages, and does not replace PulseAudio or
rewrite ALSA configuration. Set up the desktop audio system once using your
distribution's supported procedure.

## Use your existing rig

Point the profile at the workspace and Rack patch you already use:

```sh
gorgon init \
  --workspace ~/src/hydrapy \
  --visual-patch patches/gorgon_corridor.py \
  --rack ~/opt/Rack2Free/Rack \
  --rack-patch ~/Music/gorgon-rig.vcv
gorgon doctor
gorgon start
```

`--hydrapy /path/to/venv/bin/hydrapy` selects a different Python environment.
`--rack-user /path/to/Rack2` selects another Rack user folder containing your
plugins. `--monitor DP-5` opens the native picture fullscreen on that monitor;
the default is a separate window. Rack uses its saved patch and device choices,
including Machinemodule and the path to your own firmware. The launcher neither
copies nor installs firmware. Save your Rack changes normally with Ctrl+S.

The profile is `~/.config/gorgon/launch.json` (or `$XDG_CONFIG_HOME/gorgon/launch.json`).
Paths in it are relative to the profile, except `studio.patch`, which is inside
`workspace`. Commands are argument arrays, executed without a shell. The profile
is not overwritten by subsequent `start` or `init` commands.

If no profile exists, `gorgon start` creates one and a starter workspace under
`~/.local/share/gorgon/session`. The starter contains the visual patch, an editable
four-pattern score and Gorgon → Audio 2 listening patch using Studio's Breaks
kit. It requires GorgonBridge but no extra voice plugin or firmware. Existing
scores and patches are not overwritten. Use `gorgon init --rack-patch ...` before
your first start to use your existing Machinemodule rig instead of this starter.

## Audio selection and routing

`gorgon doctor` lists actual JACK port names. The starter uses WirePlumber's
default sink when it can resolve it; with one physical playback device it can
use that device. Multiple unresolved devices require one explicit choice:

```json
"audio": {
  "device": "Digi 003 Rack Multichannel",
  "backend": "pipewire",
  "rate": 48000,
  "quantum": 256,
  "start_services": true,
  "connections": []
}
```

Use the exact playback group printed by `doctor`; the example name is not a
hardware channel map. Rate and quantum are per-client requests, not global
forced-clock settings. The ready message reports the actual server values.
If PipeWire is unavailable, `start_services: true` permits starting its user
socket and WirePlumber through `systemctl --user start`. Services already in
use are never restarted, and the launcher does not start a competing `jackd`.
For an existing native JACK setup, use `backend: "jack"` and start that server
your usual way; `JACK_DEFAULT_SERVER` and your library selection are inherited.

The managed starter reconnects only its newly created four-channel Rack capture
client to this Studio's actual port names, including name suffixes. Other
clients' existing connections are retained. Its saved device names are refreshed
on startup, preserving modules, cables, settings and embedded archive assets.
An existing custom Rack patch is
loaded as saved. Optional explicit connection pairs can restore additional
routes once their ports exist:

```json
"connections": [
  ["{studio}:optical_L", "Your capture client:input_1"],
  ["{studio}:optical_R", "Your capture client:input_2"]
]
```

`{studio}` expands to the actual Studio client name. Hardware CV and encoded
ES-40 routes stay in your existing Studio/Rack configuration; the launcher does
not guess ADAT/S/PDIF channels, change polarity, or process encoded data as audio.
Extra Studio CLI options such as `--cv-profile` can be placed in `studio.args`.

## Session controls

```sh
gorgon status
gorgon doctor
gorgon stop
```

`start` returns after readiness. Repeating it reuses the managed processes.
The tracker score saved in the workspace is restored automatically; shutdown
saves its current server-side edits. Closing Rack ends the managed Studio too.
`stop` requests a window close through `wmctrl`, retaining Rack's save dialog.
If Rack is waiting for you, finish saving/closing its window. Without `wmctrl`,
close Rack directly. `gorgon stop --force` explicitly permits terminating apps
with unsaved Rack edits. Headless Rack (`rack.args: ["-h"]`) receives its normal
stdin exit request. Shared PipeWire services and unrelated apps remain running.

`gorgon start --foreground` keeps supervision in the terminal. `--no-browser`
suppresses opening the editor. Headless sessions also set `browser: false` and
`studio.native_output: false` in the profile. `--config /path/to/profile.json`
before the subcommand selects another profile. One managed Rack rig runs at a
time; an existing unmanaged Rack is reported instead of terminated.

Logs and process state live in `$XDG_RUNTIME_DIR/gorgon/<profile-id>` (falling
back to `$XDG_STATE_HOME/gorgon`). The private discovery file is passed to both
apps, leaving another Studio's discovery file alone. Stop validates process
birth and PID namespace before signalling; stale state cannot target a recycled
PID. Logs/discovery files are private to your account; status omits the token.

## Validation

The opt-in integration test starts a private real PipeWire server, a null output
device, the pinned Studio and Rack Free with the real Gorgon plugin. It captures
audio through the entire chain, checks score restoration, repeated start, stop,
relaunch with a changed Studio client name, and survival of an unrelated client
and the pre-existing audio server. No physical interface or firmware is used.
Unit tests cover malformed profiles, ambiguous devices, PID reuse, startup
failure cleanup and capture-route ownership. Native desktop placement and the
workstation's actual device/CV routes still need workstation testing.
