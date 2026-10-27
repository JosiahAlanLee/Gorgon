# Bridge API

How a follower on the same computer links to a running HydraPy Studio. The
Rack module in [`../rack`](../rack) is the reference client; Studio's
`studio.py` is the reference server.

Studio listens on `127.0.0.1` only. Followers must run on the same machine.

## Discovery

On start, Studio writes a JSON file and removes it on exit if it is still its own:

```
~/.hydrapy/studio.json          (override the path with HYDRAPY_STUDIO_FILE)
{"url": "http://127.0.0.1:PORT", "port": PORT, "token": "…", "pid": 12345, "root": "/path/to/workspace"}
```

The file is created with mode `0600` and replaced atomically. A client polls for
it, reads `port` and `token`, and polls again if Studio stops answering: a
restarted Studio writes a new file with a new token. A client must not cache the
token across Studio restarts.

## Authentication

Every request carries the session token in a header:

```
X-Hydrapy-Token: <token>
```

Studio also requires `Host` to be `127.0.0.1:PORT` or `localhost:PORT`, and
`Origin`, if present, to match. POST bodies are JSON, `Content-Type:
application/json`, at most 2,000,000 bytes. A failed check answers `403
{"error": "Local session token required"}`. Any other failure answers `400
{"error": "…"}`. Responses are `application/json` with `Cache-Control: no-store`.

## Clock model

All times are in seconds. Studio has two clocks:

- **`clock_time`**, the render clock: the time the patch sees. It advances only
  while Studio is playing.
- **`wall`**, the monotonic system clock (`time.monotonic()`), at the instant the
  playhead was sampled. On the same host, this is the client's monotonic clock too.

A client estimates the current beat as:

```
clock(now) = clock_time + (playing ? now − wall : 0)
beat(now)  = anchor_beat + (enabled ? (clock(now) − anchor_time) · bpm / 60 : 0), wrapped into the cycle
```

Studio publishes the playhead before rendering each frame, so `wall` is not
delayed by render time. Studio and every follower (browser, direct CV, Rack)
use the same pre-render timestamp. A follower should run its own beat on its
own sample clock between updates and trim gently towards Studio's estimate
rather than jump; the Rack module trims tempo by at most 0.5 %. Followers
receive updates roughly 30 times a second.

`server_now` is `time.monotonic()` at the moment the response was built, so a
client can measure transit time.

## Endpoints

### `GET /api/bridge?revision=N`

The playhead, the patch's controls, and the score if it changed.

```json
{
  "revision": 12, "enabled": true, "playing": true, "transport_revision": 3,
  "anchor_beat": 16.0, "anchor_time": 41.25, "clock_time": 43.0, "wall": 10923.118,
  "controls": [{"name": "Dolly Z", "min": 0, "max": 1, "value": 0.35}, …],
  "document": { "format": "hydrapy-tracker-1", … },
  "server_now": 10923.119
}
```

- `document` is omitted when `N` equals the current `revision`. Send the last
  revision you hold; omit the query on the first request.
- `enabled` is tracker automation; `playing` is Studio's transport. Outputs
  move only when both are true.
- `controls[].value` is the control's base value (before any modulation the
  patch adds), clamped to `[min, max]`.
- Clients must ignore fields they don't recognise.
- For a version-3 document the playhead also carries `start`, `elapsed`, `entry`,
  `fill` and `pending`, in integer ticks at 1920 per quarter note
  ([evaluator.md](evaluator.md#playhead-fields)), and the transport endpoint's
  `beat` ranges over the current cycle ([score-v3.md](score-v3.md#transport));
  Studio rounds it to the nearest tick. `beat` and `anchor_beat` stay
  floating-point beats.

### `POST /api/tracker_transport`

```json
{"enabled": true, "playing": true, "beat": 16.0, "blackout": false}
```

Every field is optional and keeps its current value when absent. `beat` is
0–64. A request that contains `beat`, or that changes `enabled`, is a seek:
`transport_revision` increments. Enabling automation while an external clock
is armed in Studio is refused with `400`. The reply is the tracker snapshot:

```json
{"document": {…}, "revision": 12, "enabled": true, "beat": 16.0, "playing": true,
 "transport_revision": 4, "blackout": false}
```

The Rack module sends `{"enabled": true, "playing": true}` to start,
`{"playing": false}` to pause, and `{"beat": <cycle start>}` to reset.

### `POST /api/bridge_params`

```json
{"values": {"Dolly Z": 0.5, "Warp": -1.2}}
```

Sets named patch controls, at most 64 per request, each clamped to the
control's `[min, max]`. The reply says what happened to each:

```json
{"applied": ["Warp"], "ignored": {"Dolly Z": "driven by tracker automation"}}
```

Reasons: `unknown control`, `driven by tracker automation` (the control is
bound to a lane and automation is enabled), `not a finite number`.

### Other endpoints

Studio has more routes (`/api/tracker`, `/api/state`, `/api/frame`, …) for its
own editor and output windows. They are not part of this contract and may
change without notice. A follower uses only the three above.

## Follower obligations

- Treat a `transport_revision` change as a discontinuity: emit a complete reset
  pulse (even if the playhead is already past its first milliseconds), retrigger
  open gates and give the row at `anchor_beat` its landing gate
  ([cv-semantics.md](cv-semantics.md)), clear held pitch and velocity, and
  re-anchor.
- Do not re-trigger reset on a `revision` change, a controls change, or a
  repeated snapshot.
- When Studio stops answering, keep the last tempo and position and carry on;
  reconnect through the discovery file.
- Never send a control value for a control you have not seen in `controls`.
