# Contract changelog

Newest first. Each entry names the HydraPy commit and Rack module version it
describes.

## 2026-10-25 — short seeks are seeks

HydraPy `20e9276f74bd469a29a4d370086cae744ff72c66` (unchanged) · Rack module
`GorgonBridge` 2.2.1.

[cv-semantics.md](cv-semantics.md): at a transport change a follower that runs
its own clock jumps to the playhead however small the change. The Rack module
treated any difference under 0.1 beat as drift, so a seek shorter than that (a
fifth of a row back, say) was slewed at the 0.5% trim and stayed up to 27 ms
off for seconds. Release blocker in `release.json`.

## 2026-10-25 — native Rack ByteBus connector

HydraPy `67d08975a17b203652ecb53bb777e80446626816` (unchanged) · Rack module
`GorgonBridge` 2.2.0 (implementation follows this specification).

Adds [bytebus-output.md](bytebus-output.md): BUS OUT encodes Z, Roll, Feedback,
or a four-lane hit mask as one clocked byte, with canonical 0/10 V lines and
transport-aware publication. Existing score, bridge and direct CV semantics
are unchanged. This Rack-side connector needs no HydraPy implementation
change, pin move, or fixture regeneration; the fixture must remain identical.

## 2026-10-25 — first written contract

HydraPy `96a4cdb` (main after PR #47) · Rack module `GorgonBridge` 2.1.0.

Written from the implementations as they stood; nothing changed in either to
produce it. Describes:

- Score format `hydrapy-tracker-1`, with the optional `scenes` field added by
  HydraPy PR #46.
- Bridge API: `GET /api/bridge`, `POST /api/tracker_transport`,
  `POST /api/bridge_params`, the `studio.json` discovery file and the
  `X-Hydrapy-Token` header.
- Signal semantics as `TrackerCV` computes them, including the 5 V direct / 10 V
  Rack level split and the Rack 5 V option added in DSP_Emulation PR #5.
- The four JACK listening-audio ports added in HydraPy PR #44.
