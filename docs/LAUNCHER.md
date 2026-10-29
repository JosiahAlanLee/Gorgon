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

Install your distribution's PipeWire JACK tools, including `pw-jack`, `pw-cli`,
`pw-dump` and `pw-top`, and WirePlumber. `wmctrl` is optional: it lets
`gorgon stop` request a clean Rack window close, and with `xrandr` it keeps the
picture and Rack on different monitors. This is a user-session launcher: it runs no
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
the default is a separate window. With two or more monitors, `start` keeps the
picture and Rack apart on X11: a fullscreen picture keeps its monitor and Rack's
window moves to another (the primary one when it is free), and a picture window
moves off Rack's monitor. Rack uses its saved patch and device choices,
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
  "quantum": 1024,
  "start_services": true,
  "connections": []
}
```

Use the exact playback group printed by `doctor`; the example name is not a
hardware channel map. Rate and quantum are per-client requests, not global
forced-clock settings. The default 1024 frames (21 ms at 48 kHz) is what Studio's
JACK client sustains on a desktop session: on a Ryzen 5 5600X, 256 and 512 frames
dropped audio for the first seconds after Play. Lower it only if yours keeps up.
The ready message reports the rate and quantum Studio's JACK client actually
runs at (read with `pw-top` under PipeWire).
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
loaded as saved. Under PipeWire, Rack's own JACK clients are listed after every
other device (`priority.session = -1`): Rack 2 numbers devices in listing order
when it looks one up and again when it opens it, so its first audio client could
otherwise take the number of the device the next module opens. Optional explicit
connection pairs can restore additional routes once their ports exist:

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

The opt-in integration test starts a private real PipeWire server, a test output
whose node ID sits above freed ones (as on a long-running desktop server), the
pinned Studio and Rack Free with the real Gorgon plugin. It captures
audio through the entire chain, checks score restoration, repeated start, stop,
relaunch with a changed Studio client name, and survival of an unrelated client
and the pre-existing audio server. No physical interface or firmware is used.
Unit tests cover malformed profiles, ambiguous devices, PID reuse, startup
failure cleanup, capture-route ownership, the session quantum and window
placement; placement was also checked on a two-monitor GNOME (X11) desktop. The
workstation's actual device/CV routes still need workstation testing.

## Launcher changelog

### 0.1.1

- Discard malformed session-state containers and ignore invalid process records
  so status and stop remain usable without signalling unverified processes.
- Remove a newly created invalid profile when initialization fails validation,
  allowing a corrected `init` command to retry.
- Treat missing or timed-out `wmctrl` commands as an unsuccessful window-close
  request so optional desktop tools cannot interrupt app cleanup.

## Render, record a live performance, and measure lane latency

Install the optional Python capture dependencies with `pip install 'gorgon-live[capture]'`.
The host also needs `jackd`, `xvfb-run` (with `xauth`), FFmpeg/ffprobe and Rack Free.
Capture runs in the foreground and stops its own children on completion or Ctrl+C.
It copies the profile's Studio workspace and Rack plugins into a **new** output
directory and retains logs there. Existing output directories are refused. The
original workspace, Rack patch, plugin folder and desktop audio services are not
changed. Use an output directory outside the source workspace.

Your profile must select a saved `rack.patch` containing exactly one Core Audio-16.
Wire its first two inputs to the stereo mix and inputs 3–16 to stems or diagnostic
CV. The launcher gives this module a private 16-channel recording device, and
points Gorgon's optical input at the actual Studio client. Embedded `.vcv` assets
are preserved. Install required plugins in the profile's `rack.user_dir`, or add
built plugin directories with repeated `--plugin-dir /path/to/plugin` options.
The copied patch preserves the remaining instrument/routing choices.

```sh
gorgon --config rig.json render score.json --output /path/to/new-take --seconds 40

gorgon --config rig.json live score.json --record --native-output DP-5 \
  --output /path/to/new-live-take --seconds 384
```

`render` is offscreen, silent and isolated on a new private **dummy** JACK server.
It runs at wall-clock speed, not faster than real time. It writes `capture.wav`
(16-channel float), `mix.wav`, `stems/channel-03.wav` through `channel-16.wav`,
`studio.mp4`, `render.mp4`, timing/xrun/discontinuity diagnostics in `capture.json`, per-block JACK and
monotonic timestamps in `capture-clock.npz`, and app logs.
The mix and stems preserve recorded levels; normalization is not applied.
Xvfb chooses a free display numbered 147 or higher for headless apps. `live`
displays Studio fullscreen on the requested monitor and otherwise uses the same
private capture path; it is silent by default too.

`render --jack-server NAME` and `live --jack-server NAME` name the dummy server the
command will create. Each refuses a name already in use. To use a real device, start the desired native JACK
server yourself, then explicitly pass **both** `--hardware --jack-server NAME`.
Only this mode attaches to an existing native server. With a PipeWire profile,
`live --hardware` (without `--jack-server`) instead uses the launcher's `pw-jack`
wrapper and existing PipeWire service. It refuses a stopped service rather than
starting or reconfiguring desktop audio. The mix is sent to the profile's
`audio.device`, or `--audio-device CLIENT` when supplied; PipeWire's default sink
is resolved by the existing launcher logic. No hardware route is made without
`--hardware`. Use `--link SOURCE DESTINATION` repeatedly for additional JACK
connections. Profile `audio.connections` are also applied. `{studio}` and
`{capture}` expand to the actual client names. Sixteen repeated `--source PORT`
options can select explicit JACK capture sources instead of rewriting Audio-16.
The original `gorgon start` retains the profile's existing PipeWire handling.

Options include `--warmup` (seconds, default 6), `--jack-timeout` (ms, default
10000), `--size WIDTHxHEIGHT`, `--fps`, `--optical-gain` and `--lead-blocks`.
Sample rate and period come from the profile's `audio.rate` and `audio.quantum`.
JACK1's system library path is selected for these capture processes.

Studio score uploads and transport changes use the profile's `timeout` (seconds),
allowing scene compilation to finish. Video recording start/stop requests allow
at least 120 seconds, or the profile timeout if longer. Readiness polls retain
a three-second request timeout. A request timeout reports the Studio endpoint
and its timeout.

By default the mux aligns the recording request and audio callbacks using their
monotonic timestamps. Studio/encoder startup adds an **unmeasured** delay; this is
an estimate. For a patch whose centre visibly flashes with a particular lane,
pass `--sync-gate-channel N` (a 1-based capture channel carrying that gate) and
`--sync-delay-ms MS` (the measured gate-to-sound delay). This correlates centre
brightness rises with gate edges, as in the Carrier harness, and writes
`av-sync.json` with the best lag, correlation, runner-up and ambiguity flag.
Periodic or unrelated imagery can give ambiguous alignment: inspect that report
and the video before accepting a take. `--av-offset-ms MS` adds a manual correction
(positive skips more audio at video frame zero). This is not a universal visual
onset detector.

```sh
gorgon --config probe-rig.json measure-latency --output /path/to/new-calibration
```

For measurement, wire mix to Audio-16 channels 1/2, then each lane's **undelayed**
gate and isolated sounding voice to pairs 3/4, 5/6, …, 13/14. Inputs 15/16 may be
silent. The command clears Gorgon's lane offsets in the private patch, plays a
short C4 probe score (one hit per second), records the six paths and writes
`latency.json`. Lane sound defaults come from the loaded Studio document, or
from `--score SCORE.json` when supplied. Clear any external gate delays yourself. The report includes
per-lane median, percentiles and hit count, and a `laneOffsetMs` array that delays
faster voices to the slowest voice. It refuses fewer than three audible hits per
lane, silence and a spread over 100 ms. Apply that array to Gorgon's module JSON
while stopped (see the Rack manual). It does not guess offsets or rewrite your
original patch. Calibration depends on the selected voice/envelope and buffers;
a sharp probe envelope gives a more useful latency than a slow pad.
